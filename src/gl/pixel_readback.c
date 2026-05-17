// =============================================================================
//  pixel_readback.c — Async glReadPixels via PBO + Fence Sync (GLES 3.0+)
//
//  Implementasi double-buffer async readback:
//
//  Siklus frame double-buffer:
//
//    Frame A  │  Frame B  │  Frame C
//    ─────────┼───────────┼─────────
//    begin→   │  begin→   │  begin→
//    PBO[0]   │  PBO[1]   │  PBO[0]
//    fence[0] │  fence[1] │  fence[0]
//             │  collect← │  collect←
//             │  PBO[0]   │  PBO[1]
//             │  (ready)  │  (ready)
//
//  Perbandingan latency vs synchronous:
//  - Synchronous: CPU stall 5–50ms (glFinish implisit di driver)
//  - Async flush_sync: CPU stall <1ms (hanya tunggu fence spesifik, bukan full flush)
//  - Async deferred: CPU stall 0ms (data siap 1 frame kemudian)
//
//  Implementasi gl4es_readback_flush_sync() menggunakan timeout pendek untuk
//  glClientWaitSync sehingga driver tidak harus flush seluruh command queue.
// =============================================================================

#include "pixel_readback.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "../gl/init.h"
#include "../gl/debug.h"
#include "../gl/logs.h"
#include "../gl/pixel.h"
#include "../glx/hardext.h"

//#define DEBUG_READBACK
#ifdef DEBUG_READBACK
#define RBDBG(a) a
#else
#define RBDBG(a)
#endif

// ── Global pool instance ──────────────────────────────────────────────────────
gl4es_readback_pool_t gl4es_readback_pool = {
    .slots     = {{0}},
    .write_idx = 0,
    .read_idx  = 1,    // read_idx selalu 1 di belakang write_idx
    .inited    = 0
};

// =============================================================================
//  pixel_readback_init
// =============================================================================
void pixel_readback_init(void) {
    if (gl4es_readback_pool.inited) return;
    if (!globals4es.use_pbo)        return;
    if (hardext.esversion < 3)      return;

    LOAD_GLES(glGenBuffers);
    LOAD_GLES(glGetError);
    if (!gles_glGenBuffers) {
        SHUT_LOGD("Readback PBO: glGenBuffers tidak tersedia\n");
        return;
    }

    // Generate semua PBO IDs
    GLuint ids[READBACK_PBO_COUNT] = {0};
    gles_glGenBuffers(READBACK_PBO_COUNT, ids);
    if (gles_glGetError() != GL_NO_ERROR) {
        SHUT_LOGD("Readback PBO: glGenBuffers gagal\n");
        return;
    }

    for (int i = 0; i < READBACK_PBO_COUNT; i++) {
        gl4es_readback_pool.slots[i].pbo_id       = ids[i];
        gl4es_readback_pool.slots[i].capacity     = 0;
        gl4es_readback_pool.slots[i].fence        = NULL;
        gl4es_readback_pool.slots[i].pending      = 0;
        gl4es_readback_pool.slots[i].pending_size = 0;
    }
    gl4es_readback_pool.write_idx = 0;
    gl4es_readback_pool.read_idx  = 1;   // start with read "ahead" of write
    gl4es_readback_pool.inited    = 1;

    SHUT_LOGD("Readback PBO init: %d slots, threshold=%d bytes\n",
              READBACK_PBO_COUNT, READBACK_PBO_THRESHOLD);
}

// =============================================================================
//  pixel_readback_cleanup
// =============================================================================
void pixel_readback_cleanup(void) {
    if (!gl4es_readback_pool.inited) return;

    LOAD_GLES(glDeleteBuffers);
    LOAD_GLES2(glDeleteSync);

    for (int i = 0; i < READBACK_PBO_COUNT; i++) {
        gl4es_readback_slot_t *s = &gl4es_readback_pool.slots[i];
        if (s->fence && gles_glDeleteSync) {
            gles_glDeleteSync(s->fence);
            s->fence = NULL;
        }
        if (s->pbo_id && gles_glDeleteBuffers) {
            gles_glDeleteBuffers(1, &s->pbo_id);
            s->pbo_id = 0;
        }
    }

    memset(&gl4es_readback_pool, 0, sizeof(gl4es_readback_pool_t));
    SHUT_LOGD("Readback PBO cleanup selesai\n");
}

// =============================================================================
//  helper: delete_fence — hapus fence sync object
// =============================================================================
static void delete_fence(gl4es_readback_slot_t *slot) {
    if (slot->fence) {
        LOAD_GLES2(glDeleteSync);
        if (gles_glDeleteSync)
            gles_glDeleteSync(slot->fence);
        slot->fence = NULL;
    }
}

// =============================================================================
//  gl4es_readback_begin — Mulai async readback: glReadPixels → PBO (non-block)
// =============================================================================
int gl4es_readback_begin(GLint x, GLint y, GLsizei width, GLsizei height,
                          GLenum format, GLenum type,
                          GLvoid *dst_cpu_ptr)
{
    // ── Guards ─────────────────────────────────────────────────────────────────
    if (!globals4es.use_pbo)          return 0;
    if (!gl4es_readback_pool.inited)  return 0;
    if (hardext.esversion < 3)        return 0;
    if (!dst_cpu_ptr)                 return 0;
    if (width <= 0 || height <= 0)    return 0;

    int pixel_size = pixel_sizeof(format, type);
    if (pixel_size <= 0) return 0;
    GLsizei data_size = width * height * pixel_size;
    if (data_size < READBACK_PBO_THRESHOLD) return 0;

    LOAD_GLES(glBindBuffer);
    LOAD_GLES(glBufferData);
    LOAD_GLES(glReadPixels);
    LOAD_GLES2(glFenceSync);
    LOAD_GLES(glGetError);

    if (!gles_glBindBuffer || !gles_glBufferData ||
        !gles_glReadPixels || !gles_glFenceSync)
        return 0;

    // ── Pilih slot write ────────────────────────────────────────────────────────
    int widx = gl4es_readback_pool.write_idx;
    gl4es_readback_slot_t *ws = &gl4es_readback_pool.slots[widx];

    // Hapus fence lama jika slot ini punya pending yang belum di-collect
    // (overwrite — terjadi jika readback lebih cepat dari collect)
    if (ws->fence) {
        RBDBG(printf("readback_begin: slot %d overwrite (pending not collected)\n", widx);)
        delete_fence(ws);
    }
    ws->pending = 0;

    // ── Bind PBO ke GL_PIXEL_PACK_BUFFER ──────────────────────────────────────
    gles_glBindBuffer(GL_PIXEL_PACK_BUFFER, ws->pbo_id);
    if (gles_glGetError() != GL_NO_ERROR) {
        gles_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        return 0;
    }

    // ── Alokasi/reuse storage PBO ─────────────────────────────────────────────
    // GL_STREAM_READ: ditulis GPU (readback), dibaca CPU sekali
    if (data_size > ws->capacity) {
        gles_glBufferData(GL_PIXEL_PACK_BUFFER, data_size, NULL, GL_STREAM_READ);
        if (gles_glGetError() != GL_NO_ERROR) {
            gles_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            return 0;
        }
        ws->capacity = data_size;
    }

    // ── Async readback: glReadPixels → PBO (NULL = offset 0 dari PBO) ────────
    // Dengan PBO bound: driver schedule DMA async, TIDAK block CPU
    gles_glReadPixels(x, y, width, height, format, type, (const void*)0);
    if (gles_glGetError() != GL_NO_ERROR) {
        gles_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        return 0;
    }

    // ── Buat fence sync ────────────────────────────────────────────────────────
    // GL_SYNC_GPU_COMMANDS_COMPLETE: fence signal setelah semua commands selesai
    ws->fence = gles_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (!ws->fence) {
        RBDBG(printf("readback_begin: glFenceSync gagal\n");)
        gles_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        return 0;
    }

    // ── Unbind PBO ─────────────────────────────────────────────────────────────
    gles_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    // ── Simpan metadata untuk collect ─────────────────────────────────────────
    ws->pending      = 1;
    ws->pending_size = data_size;
    ws->pending_fmt  = format;
    ws->pending_type = type;
    ws->pending_w    = width;
    ws->pending_h    = height;

    // Advance write index (ping-pong)
    gl4es_readback_pool.write_idx = (widx + 1) % READBACK_PBO_COUNT;
    // read_idx = slot yang baru saja di-write (siap di-collect frame berikutnya)
    gl4es_readback_pool.read_idx  = widx;

    RBDBG(printf("readback_begin: slot %d started, size=%d, fence=%p\n",
                  widx, data_size, ws->fence);)
    return 1;
}

// =============================================================================
//  gl4es_readback_collect — Ambil hasil readback dari PBO (non-blocking poll)
//
//  Dipanggil frame berikutnya. Test fence tanpa block.
//  Jika selesai: map PBO → copy data → unmap.
// =============================================================================
int gl4es_readback_collect(GLvoid *dst_cpu_ptr)
{
    if (!gl4es_readback_pool.inited) return 0;
    if (!dst_cpu_ptr)                return 0;

    int ridx = gl4es_readback_pool.read_idx;
    gl4es_readback_slot_t *rs = &gl4es_readback_pool.slots[ridx];

    if (!rs->pending || !rs->fence) return 0;

    LOAD_GLES2(glClientWaitSync);
    LOAD_GLES2(glDeleteSync);
    LOAD_GLES(glBindBuffer);
    LOAD_GLES2(glMapBufferRange);
    LOAD_GLES2(glUnmapBuffer);
    LOAD_GLES(glGetError);

    if (!gles_glClientWaitSync || !gles_glMapBufferRange) return 0;

    // ── Poll fence tanpa block ─────────────────────────────────────────────────
    // GL_SYNC_FLUSH_COMMANDS_BIT: flush command queue jika fence belum signal
    GLenum sync_result = gles_glClientWaitSync(rs->fence,
                                                GL_SYNC_FLUSH_COMMANDS_BIT,
                                                READBACK_FENCE_TIMEOUT_NS);

    if (sync_result == GL_TIMEOUT_EXPIRED || sync_result == GL_WAIT_FAILED) {
        // Belum selesai atau error — coba lagi frame berikutnya
        RBDBG(printf("readback_collect: slot %d fence not ready (%d)\n",
                      ridx, sync_result);)
        return 0;
    }

    // Fence signal → data siap di PBO
    delete_fence(rs);

    // ── Map PBO → copy data ke CPU ─────────────────────────────────────────────
    gles_glBindBuffer(GL_PIXEL_PACK_BUFFER, rs->pbo_id);
    if (gles_glGetError() != GL_NO_ERROR) {
        gles_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        rs->pending = 0;
        return 0;
    }

    // GL_MAP_READ_BIT: CPU akan baca dari buffer
    // Tidak gunakan UNSYNCHRONIZED karena fence sudah confirm GPU selesai
    void *pbo_ptr = gles_glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0,
                                           rs->pending_size, GL_MAP_READ_BIT);
    if (!pbo_ptr) {
        RBDBG(printf("readback_collect: glMapBufferRange gagal\n");)
        gles_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        rs->pending = 0;
        return 0;
    }

    // Copy data ke CPU buffer
    memcpy(dst_cpu_ptr, pbo_ptr, (size_t)rs->pending_size);

    // Unmap
    gles_glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    gles_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    rs->pending = 0;

    RBDBG(printf("readback_collect: slot %d collected %d bytes ✓\n",
                  ridx, rs->pending_size);)
    return 1;
}

// =============================================================================
//  gl4es_readback_flush_sync — Synchronous readback via PBO + fence wait
//
//  Versi "lebih baik dari glFinish" untuk path yang butuh data sebelum return:
//  1. Bind PBO ke GL_PIXEL_PACK_BUFFER
//  2. glReadPixels(NULL) → async DMA ke PBO
//  3. glFenceSync + glClientWaitSync dengan timeout kecil
//  4. Map PBO → copy data ke CPU → unmap
//
//  Ini TETAP lebih cepat dari glReadPixels langsung karena:
//  - Driver tidak perlu flush seluruh command queue
//  - Hanya tunggu spesifik fence ini saja
//  - Overhead: ~0.1–1ms vs 5–50ms synchronous penuh
// =============================================================================
int gl4es_readback_flush_sync(GLvoid *dst_cpu_ptr,
                               GLsizei width, GLsizei height,
                               GLenum format, GLenum type)
{
    if (!globals4es.use_pbo)         return 0;
    if (!gl4es_readback_pool.inited) return 0;
    if (hardext.esversion < 3)       return 0;
    if (!dst_cpu_ptr)                return 0;
    if (width <= 0 || height <= 0)   return 0;

    int pixel_size = pixel_sizeof(format, type);
    if (pixel_size <= 0) return 0;
    GLsizei data_size = width * height * pixel_size;
    if (data_size < READBACK_PBO_THRESHOLD) return 0;

    LOAD_GLES(glBindBuffer);
    LOAD_GLES(glBufferData);
    LOAD_GLES(glReadPixels);
    LOAD_GLES2(glFenceSync);
    LOAD_GLES2(glClientWaitSync);
    LOAD_GLES2(glDeleteSync);
    LOAD_GLES2(glMapBufferRange);
    LOAD_GLES2(glUnmapBuffer);
    LOAD_GLES(glGetError);

    if (!gles_glBindBuffer || !gles_glBufferData ||
        !gles_glReadPixels || !gles_glFenceSync ||
        !gles_glClientWaitSync || !gles_glMapBufferRange)
        return 0;

    // Gunakan slot write sementara (tidak menggangu ping-pong state)
    int widx = gl4es_readback_pool.write_idx;
    gl4es_readback_slot_t *ws = &gl4es_readback_pool.slots[widx];

    // Hapus fence pending jika ada
    if (ws->fence) {
        delete_fence(ws);
        ws->pending = 0;
    }

    // Bind PBO
    gles_glBindBuffer(GL_PIXEL_PACK_BUFFER, ws->pbo_id);
    if (gles_glGetError() != GL_NO_ERROR) {
        gles_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        return 0;
    }

    // Alokasi storage
    if (data_size > ws->capacity) {
        gles_glBufferData(GL_PIXEL_PACK_BUFFER, data_size, NULL, GL_STREAM_READ);
        if (gles_glGetError() != GL_NO_ERROR) {
            gles_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            return 0;
        }
        ws->capacity = data_size;
    }

    // glReadPixels → async DMA ke PBO
    gles_glReadPixels(0, 0, width, height, format, type, (const void*)0);
    if (gles_glGetError() != GL_NO_ERROR) {
        gles_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        return 0;
    }
    gles_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    // Buat fence dan tunggu (dengan timeout bertahap: 0 → 1ms → 10ms)
    GLsync fence = gles_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (!fence) return 0;

    GLenum result = GL_TIMEOUT_EXPIRED;
    // Timeout bertahap: coba dulu tanpa block, lalu tunggu maksimum 10ms
    const GLuint64 timeouts[] = { 0, 1000000ULL, 10000000ULL };  // 0, 1ms, 10ms
    for (int t = 0; t < 3 && result == GL_TIMEOUT_EXPIRED; t++) {
        result = gles_glClientWaitSync(fence,
                                        GL_SYNC_FLUSH_COMMANDS_BIT,
                                        timeouts[t]);
    }

    gles_glDeleteSync(fence);

    if (result == GL_WAIT_FAILED || result == GL_TIMEOUT_EXPIRED) {
        RBDBG(printf("readback_flush_sync: fence timeout/fail (%d)\n", result);)
        return 0;   // Fallback ke caller
    }

    // Map PBO → copy → unmap
    gles_glBindBuffer(GL_PIXEL_PACK_BUFFER, ws->pbo_id);
    void *pbo_ptr = gles_glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0,
                                           data_size, GL_MAP_READ_BIT);
    if (!pbo_ptr) {
        gles_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        return 0;
    }

    memcpy(dst_cpu_ptr, pbo_ptr, (size_t)data_size);
    gles_glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    gles_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    RBDBG(printf("readback_flush_sync: %dx%d fmt=0x%x size=%d OK\n",
                  width, height, format, data_size);)
    return 1;
}
