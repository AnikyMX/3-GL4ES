// =============================================================================
//  texture_pbo.c — PBO Pool & Async Texture Upload (GLES 3.0+)
//
//  Implementasi PBO pool untuk mempercepat texture upload di Minecraft 1.12.2:
//
//  Cara kerja upload via PBO (GLES 3.0+):
//  1. Bind PBO ke GL_PIXEL_UNPACK_BUFFER
//  2. Alokasi/update storage PBO via glBufferData (GL_STREAM_DRAW)
//  3. Map PBO ke CPU via glMapBufferRange(GL_MAP_WRITE_BIT|GL_MAP_INVALIDATE_BUFFER_BIT)
//  4. Copy pixel data ke PBO (CPU write ke mapped pointer)
//  5. Unmap PBO (glUnmapBuffer) — commit data ke GPU
//  6. Call glTexImage2D/glTexSubImage2D dengan offset = NULL
//     → Driver mulai DMA async dari PBO ke texture, TIDAK block CPU
//  7. Unbind PBO
//
//  Pool management:
//  - 4 slot PBO, round-robin allocation
//  - Tiap slot menyimpan kapasitas yang sudah dialokasi → reuse jika size <= capacity
//  - glBufferData hanya dipanggil ulang jika size > capacity (realloc PBO)
//
//  Threshold: hanya texture >= 16KB (64x64 RGBA) yang pakai PBO.
//  Texture lebih kecil: overhead PBO > manfaat, langsung sync upload lebih cepat.
// =============================================================================

#include "texture_pbo.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "../gl/init.h"
#include "../gl/pixel.h"      // pixel_sizeof
#include "../gl/debug.h"
#include "../glx/hardext.h"

//#define DEBUG_PBO
#ifdef DEBUG_PBO
#define PDBG(a) a
#else
#define PDBG(a)
#endif

// ── Global pool instance ──────────────────────────────────────────────────────
gl4es_pbo_pool_t gl4es_pbo_pool = {
    .slots     = {{0}},
    .next_slot = 0,
    .inited    = 0
};

// =============================================================================
//  texture_pbo_init — Alokasi dan pre-create PBO pool
//
//  Dipanggil dari init.c setelah GetHardwareExtensions() jika use_pbo == 1.
//  Membuat PBO object IDs tapi BELUM mengalokasi storage (lazy allocation).
// =============================================================================
void texture_pbo_init(void) {
    if (gl4es_pbo_pool.inited) return;
    if (!globals4es.use_pbo) return;

    LOAD_GLES(glGenBuffers);
    LOAD_GLES(glGetError);
    if (!gles_glGenBuffers) {
        SHUT_LOGD("PBO init: glGenBuffers tidak tersedia, PBO dinonaktifkan\n");
        globals4es.use_pbo = 0;
        return;
    }

    // Generate semua PBO IDs sekaligus
    GLuint ids[PBO_POOL_SIZE] = {0};
    gles_glGenBuffers(PBO_POOL_SIZE, ids);
    if (gles_glGetError() != GL_NO_ERROR) {
        SHUT_LOGD("PBO init: glGenBuffers gagal, PBO dinonaktifkan\n");
        globals4es.use_pbo = 0;
        return;
    }

    for (int i = 0; i < PBO_POOL_SIZE; i++) {
        gl4es_pbo_pool.slots[i].id       = ids[i];
        gl4es_pbo_pool.slots[i].capacity = 0;
        gl4es_pbo_pool.slots[i].in_use   = 0;
    }
    gl4es_pbo_pool.next_slot = 0;
    gl4es_pbo_pool.inited    = 1;

    SHUT_LOGD("PBO init: pool %d slots, threshold=%d bytes\n",
              PBO_POOL_SIZE, PBO_SIZE_THRESHOLD);
}

// =============================================================================
//  texture_pbo_cleanup — Hapus semua PBO dari GPU
//
//  Dipanggil dari close_gl4es() atau saat context destroy.
// =============================================================================
void texture_pbo_cleanup(void) {
    if (!gl4es_pbo_pool.inited) return;

    LOAD_GLES(glDeleteBuffers);
    if (gles_glDeleteBuffers) {
        GLuint ids[PBO_POOL_SIZE];
        for (int i = 0; i < PBO_POOL_SIZE; i++)
            ids[i] = gl4es_pbo_pool.slots[i].id;
        gles_glDeleteBuffers(PBO_POOL_SIZE, ids);
    }

    memset(&gl4es_pbo_pool, 0, sizeof(gl4es_pbo_pool_t));
    SHUT_LOGD("PBO pool cleanup selesai\n");
}

// =============================================================================
//  get_free_pbo_slot — Ambil slot PBO bebas dari pool
//
//  Menggunakan round-robin dengan wrap-around.
//  Jika semua slot sedang in_use (sangat jarang), paksa ambil next_slot
//  (overwrite — DMA sebelumnya diasumsikan sudah selesai setelah 4 frame).
// =============================================================================
static gl4es_pbo_entry_t* get_free_pbo_slot(void) {
    if (!gl4es_pbo_pool.inited) return NULL;

    // Cari slot bebas mulai dari next_slot
    for (int i = 0; i < PBO_POOL_SIZE; i++) {
        int idx = (gl4es_pbo_pool.next_slot + i) & (PBO_POOL_SIZE - 1);
        if (!gl4es_pbo_pool.slots[idx].in_use) {
            gl4es_pbo_pool.next_slot = (idx + 1) & (PBO_POOL_SIZE - 1);
            return &gl4es_pbo_pool.slots[idx];
        }
    }
    // Semua in_use: paksa overwrite next_slot (pool cycle)
    int idx = gl4es_pbo_pool.next_slot;
    gl4es_pbo_pool.next_slot = (idx + 1) & (PBO_POOL_SIZE - 1);
    gl4es_pbo_pool.slots[idx].in_use = 0; // reset forced
    PDBG(printf("PBO pool: forced reuse slot %d\n", idx);)
    return &gl4es_pbo_pool.slots[idx];
}

// =============================================================================
//  gl4es_pbo_upload_tex_image — Upload glTexImage2D via PBO
//
//  Return 1 jika berhasil via PBO.
//  Return 0 jika caller harus fallback ke synchronous gles_glTexImage2D().
// =============================================================================
int gl4es_pbo_upload_tex_image(GLenum target, GLint level,
                                GLenum internalformat,
                                GLsizei width, GLsizei height,
                                GLint border,
                                GLenum format, GLenum type,
                                const GLvoid *pixels)
{
    // ── Guard conditions ──────────────────────────────────────────────────────
    if (!globals4es.use_pbo)       return 0;
    if (!gl4es_pbo_pool.inited)    return 0;
    if (pixels == NULL)            return 0;
    if (width <= 0 || height <= 0) return 0;

    // Hitung ukuran data
    int pixel_size = pixel_sizeof(format, type);
    if (pixel_size <= 0) return 0;
    GLsizei data_size = width * height * pixel_size;

    // Threshold: texture kecil lebih cepat synchronous
    if (data_size < PBO_SIZE_THRESHOLD) return 0;
    // Terlalu besar: lewati PBO (sangat jarang terjadi)
    if (data_size > PBO_MAX_SIZE) return 0;

    LOAD_GLES(glBindBuffer);
    LOAD_GLES(glBufferData);
    LOAD_GLES2(glMapBufferRange);
    LOAD_GLES2(glUnmapBuffer);
    LOAD_GLES(glTexImage2D);
    LOAD_GLES(glGetError);

    if (!gles_glBindBuffer || !gles_glBufferData ||
        !gles_glMapBufferRange || !gles_glUnmapBuffer ||
        !gles_glTexImage2D)
        return 0;

    // ── Ambil PBO slot ────────────────────────────────────────────────────────
    gl4es_pbo_entry_t* slot = get_free_pbo_slot();
    if (!slot || slot->id == 0) return 0;

    slot->in_use = 1;

    // ── Bind PBO ke GL_PIXEL_UNPACK_BUFFER ────────────────────────────────────
    gles_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, slot->id);
    if (gles_glGetError() != GL_NO_ERROR) {
        gles_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        slot->in_use = 0;
        return 0;
    }

    // ── Alokasi storage PBO (lazy — hanya jika perlu resize) ─────────────────
    // GL_STREAM_DRAW: data ditulis CPU sekali, dibaca GPU sekali (perfect untuk upload)
    if (data_size > slot->capacity) {
        gles_glBufferData(GL_PIXEL_UNPACK_BUFFER, data_size, NULL, GL_STREAM_DRAW);
        if (gles_glGetError() != GL_NO_ERROR) {
            gles_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            slot->in_use = 0;
            return 0;
        }
        slot->capacity = data_size;
    }

    // ── Map PBO ke CPU address space ──────────────────────────────────────────
    // GL_MAP_WRITE_BIT:              CPU akan menulis ke buffer
    // GL_MAP_INVALIDATE_BUFFER_BIT:  isi lama tidak perlu disimpan (skip GPU sync)
    // GL_MAP_UNSYNCHRONIZED_BIT:     skip implicit GPU sync (driver-level opt)
    //   → Catatan: GL_MAP_UNSYNCHRONIZED_BIT aman karena kita BARU write dan
    //     belum ada GPU work yang bergantung pada buffer ini saat ini
    void* pbo_ptr = gles_glMapBufferRange(
        GL_PIXEL_UNPACK_BUFFER,
        0,
        data_size,
        GL_MAP_WRITE_BIT |
        GL_MAP_INVALIDATE_BUFFER_BIT |
        GL_MAP_UNSYNCHRONIZED_BIT
    );

    if (!pbo_ptr) {
        // Map gagal — fallback ke synchronous
        gles_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        slot->in_use = 0;
        PDBG(printf("PBO glMapBufferRange gagal untuk size=%d\n", data_size);)
        return 0;
    }

    // ── Copy pixel data ke PBO ────────────────────────────────────────────────
    memcpy(pbo_ptr, pixels, (size_t)data_size);

    // ── Unmap (commit data ke GPU buffer) ────────────────────────────────────
    if (gles_glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER) != GL_TRUE) {
        // Unmap gagal (buffer corrupted — sangat jarang)
        gles_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        slot->in_use = 0;
        return 0;
    }

    // ── Upload texture via PBO ────────────────────────────────────────────────
    // Dengan PBO bound, NULL pointer = offset 0 dari awal PBO
    // Driver akan melakukan DMA async → glTexImage2D RETURNS IMMEDIATELY
    gles_glTexImage2D(target, level, internalformat,
                      width, height, border,
                      format, type, NULL);   // NULL = offset 0 dari PBO

    // ── Unbind PBO ───────────────────────────────────────────────────────────
    // WAJIB unbind setelah call agar operasi GL berikutnya tidak terpengaruh
    gles_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    // Slot sudah tidak "in_use" setelah unbind — GPU handles DMA secara mandiri
    slot->in_use = 0;

    PDBG(printf("PBO upload glTexImage2D: %dx%d fmt=0x%x size=%d bytes (async)\n",
                width, height, format, data_size);)
    return 1;
}

// =============================================================================
//  gl4es_pbo_upload_tex_subimage — Upload glTexSubImage2D via PBO
//
//  Return 1 jika berhasil via PBO, 0 jika fallback ke synchronous.
//
//  Sama dengan tex_image tapi untuk partial update:
//  glTexSubImage2D dipanggil dengan offset = NULL (dari awal PBO)
// =============================================================================
int gl4es_pbo_upload_tex_subimage(GLenum target, GLint level,
                                   GLint xoffset, GLint yoffset,
                                   GLsizei width, GLsizei height,
                                   GLenum format, GLenum type,
                                   const GLvoid *pixels)
{
    // ── Guard conditions ──────────────────────────────────────────────────────
    if (!globals4es.use_pbo)       return 0;
    if (!gl4es_pbo_pool.inited)    return 0;
    if (pixels == NULL)            return 0;
    if (width <= 0 || height <= 0) return 0;

    int pixel_size = pixel_sizeof(format, type);
    if (pixel_size <= 0) return 0;
    GLsizei data_size = width * height * pixel_size;

    if (data_size < PBO_SIZE_THRESHOLD) return 0;
    if (data_size > PBO_MAX_SIZE)        return 0;

    LOAD_GLES(glBindBuffer);
    LOAD_GLES(glBufferData);
    LOAD_GLES2(glMapBufferRange);
    LOAD_GLES2(glUnmapBuffer);
    LOAD_GLES(glTexSubImage2D);
    LOAD_GLES(glGetError);

    if (!gles_glBindBuffer || !gles_glBufferData ||
        !gles_glMapBufferRange || !gles_glUnmapBuffer ||
        !gles_glTexSubImage2D)
        return 0;

    gl4es_pbo_entry_t* slot = get_free_pbo_slot();
    if (!slot || slot->id == 0) return 0;
    slot->in_use = 1;

    gles_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, slot->id);
    if (gles_glGetError() != GL_NO_ERROR) {
        gles_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        slot->in_use = 0;
        return 0;
    }

    if (data_size > slot->capacity) {
        gles_glBufferData(GL_PIXEL_UNPACK_BUFFER, data_size, NULL, GL_STREAM_DRAW);
        if (gles_glGetError() != GL_NO_ERROR) {
            gles_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            slot->in_use = 0;
            return 0;
        }
        slot->capacity = data_size;
    }

    void* pbo_ptr = gles_glMapBufferRange(
        GL_PIXEL_UNPACK_BUFFER,
        0,
        data_size,
        GL_MAP_WRITE_BIT |
        GL_MAP_INVALIDATE_BUFFER_BIT |
        GL_MAP_UNSYNCHRONIZED_BIT
    );

    if (!pbo_ptr) {
        gles_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        slot->in_use = 0;
        return 0;
    }

    memcpy(pbo_ptr, pixels, (size_t)data_size);

    if (gles_glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER) != GL_TRUE) {
        gles_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        slot->in_use = 0;
        return 0;
    }

    // glTexSubImage2D dengan NULL offset → baca dari PBO
    gles_glTexSubImage2D(target, level, xoffset, yoffset,
                          width, height, format, type, NULL);

    gles_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    slot->in_use = 0;

    PDBG(printf("PBO upload glTexSubImage2D: %dx%d at (%d,%d) size=%d bytes\n",
                width, height, xoffset, yoffset, data_size);)
    return 1;
}
