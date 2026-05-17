#ifndef _GL4ES_PIXEL_READBACK_H_
#define _GL4ES_PIXEL_READBACK_H_

// =============================================================================
//  pixel_readback.h — Async glReadPixels via PBO + Fence Sync (GLES 3.0+)
//
//  Modul ini menggantikan synchronous glReadPixels() yang memblokir CPU/GPU
//  dengan readback asinkron menggunakan:
//    - GL_PIXEL_PACK_BUFFER  : DMA dari framebuffer → PBO tanpa block CPU
//    - glFenceSync           : deteksi selesai tanpa glFinish() (GLES 3.0+)
//    - Double-buffer pattern : PBO[0] sedang diisi GPU, PBO[1] dibaca CPU
//
//  MASALAH ORIGINAL (texture_read.c glReadPixels):
//  - `gles_glReadPixels(...)` dipanggil dengan pointer CPU langsung
//  - Driver harus tunggu GPU flush semua pending commands → glFinish implisit
//  - CPU stall untuk durasi 5–50ms per call pada GPU mobile
//  - Minecraft: screenshot (F2), FBO readback screenshot, map renderer
//
//  SOLUSI:
//  Frame N:   glReadPixels(NULL → PBO[idx])   ← async DMA, return segera
//             glFenceSync(...)                 ← tandai selesai
//
//  Frame N+1: glClientWaitSync(fence, 0, 0)   ← test selesai TANPA block
//             if selesai: glMapBufferRange(PBO[prev_idx], READ) → copy → unmap
//
//  Keuntungan untuk Minecraft 1.12.2:
//  - Screenshot (F2): CPU tidak freeze selama readback berlangsung
//  - FBO readback (glCopyTexImage2D path): frame rate tidak drop saat copy
//  - MC rendering thread tidak stall menunggu GPU flush
//
//  Catatan:
//  - Readback PBO hanya untuk path yang bisa terima delay 1 frame
//  - Untuk path yang butuh data immediate (pixel_convert saat itu juga),
//    fallback ke synchronous glReadPixels tetap dipertahankan
//  - TIDAK digunakan untuk readback yang langsung diproses di frame yang sama
//    (contoh: glCopyTexImage2D immediate mode)
// =============================================================================

#include "../gl/loader.h"
#include "gles.h"
#include "../gl/init.h"
#include "../glx/hardext.h"

// ── Konstanta ─────────────────────────────────────────────────────────────────

// Jumlah slot PBO pack (2 = double buffering, cukup untuk MC screenshot path)
#define READBACK_PBO_COUNT     2

// Threshold minimum readback (byte) untuk pakai PBO async.
// Read kecil (< 64KB) lebih efisien synchronous — overhead setup PBO tidak worth it.
// 64×64×4 = 16384 byte (thumbnail)
// 256×256×4 = 262144 byte (chunk map)
// 1920×1080×4 = 8294400 byte (screenshot)
#define READBACK_PBO_THRESHOLD (64 * 64 * 4)   // 16 KB

// Timeout untuk glClientWaitSync: 0 = non-blocking (poll saja)
// Kita tidak pernah block di sini — jika fence belum signal, return fallback
#define READBACK_FENCE_TIMEOUT_NS  0

// =============================================================================
//  gl4es_readback_slot_t — Satu slot dalam double-buffer readback
// =============================================================================
typedef struct {
    GLuint  pbo_id;       // GL buffer object ID untuk GL_PIXEL_PACK_BUFFER
    GLsizei capacity;     // Kapasitas byte yang sudah dialokasi
    GLsync  fence;        // Fence sync (non-NULL = GPU masih bekerja atau sudah selesai)
    int     pending;      // 1 = ada readback yang menunggu hasil
    GLsizei pending_size; // Ukuran data yang pending (byte)
    GLenum  pending_fmt;  // Format yang digunakan readback pending
    GLenum  pending_type; // Type yang digunakan readback pending
    GLsizei pending_w;    // Width readback pending
    GLsizei pending_h;    // Height readback pending
} gl4es_readback_slot_t;

// =============================================================================
//  gl4es_readback_pool_t — Double-buffer pool untuk async readback
// =============================================================================
typedef struct {
    gl4es_readback_slot_t slots[READBACK_PBO_COUNT];
    int   write_idx;    // Slot yang sedang digunakan untuk readback baru
    int   read_idx;     // Slot yang datanya sudah bisa di-map (frame sebelumnya)
    int   inited;
} gl4es_readback_pool_t;

// ── Global instance ────────────────────────────────────────────────────────────
extern gl4es_readback_pool_t gl4es_readback_pool;

// =============================================================================
//  API Publik
// =============================================================================

// Inisialisasi double-buffer PBO pack pool.
// Dipanggil dari init.c setelah GetHardwareExtensions() jika use_pbo == 1.
void pixel_readback_init(void);

// Cleanup seluruh pool + delete fence objects.
// Dipanggil dari close_gl4es().
void pixel_readback_cleanup(void);

// ── gl4es_readback_begin ───────────────────────────────────────────────────────
// Mulai async readback: glReadPixels → PBO (non-blocking).
//
// Alur:
//  1. Cek kondisi (use_pbo, size >= threshold, GLES 3.0+)
//  2. Bind PBO[write_idx] ke GL_PIXEL_PACK_BUFFER
//  3. Alokasi/reuse storage via glBufferData(GL_STREAM_READ)
//  4. glReadPixels(x, y, w, h, fmt, type, NULL) → DMA ke PBO
//  5. Buat fence sync → tandai GPU masih bekerja
//  6. Unbind PBO, simpan metadata, advance write_idx
//
// Return 1 = readback dimulai (data tersedia SATU FRAME KEMUDIAN via _collect).
// Return 0 = tidak bisa async, caller harus gunakan synchronous path.
int gl4es_readback_begin(GLint x, GLint y, GLsizei width, GLsizei height,
                          GLenum format, GLenum type,
                          GLvoid *dst_cpu_ptr);

// ── gl4es_readback_collect ────────────────────────────────────────────────────
// Coba ambil hasil readback dari PBO sebelumnya (non-blocking fence check).
//
// Dipanggil di awal frame berikutnya SEBELUM _begin() baru.
// Jika fence sudah signal → map PBO → copy ke dst → unmap.
// Jika fence belum signal → return 0 (data belum siap).
//
// dst_cpu_ptr: buffer tujuan (harus sama dengan yang di-pass ke _begin)
// Return 1 = data berhasil di-copy ke dst_cpu_ptr.
// Return 0 = belum siap atau tidak ada readback pending.
int gl4es_readback_collect(GLvoid *dst_cpu_ptr);

// ── gl4es_readback_flush_sync ─────────────────────────────────────────────────
// Versi SYNCHRONOUS: tunggu hingga fence selesai, lalu map dan copy.
// Digunakan ketika caller benar-benar butuh data sebelum return
// (path yang tidak bisa terima delay 1 frame).
//
// Ini tetap lebih baik dari glReadPixels langsung karena GPU tidak perlu
// flush seluruh command queue — hanya menunggu fence yang spesifik.
//
// Return 1 = data tersedia di dst_cpu_ptr.
// Return 0 = timeout / error / fallback ke caller.
int gl4es_readback_flush_sync(GLvoid *dst_cpu_ptr,
                               GLsizei width, GLsizei height,
                               GLenum format, GLenum type);

// =============================================================================
//  Macro untuk texture_read.c
//
//  Gantikan gles_glReadPixels(...) dengan GL4ES_READPIXELS_ASYNC(...).
//  Jika tidak bisa async (kondisi tidak terpenuhi), fallback otomatis.
//
//  Dua mode:
//  - DEFERRED: data tersedia frame berikutnya (untuk screenshot background)
//  - SYNC: data tersedia sebelum return (untuk FBO copy path)
// =============================================================================

// Mode SYNC (default untuk glReadPixels): gunakan fence sync → tunggu → copy
// Lebih baik dari glFinish karena hanya tunggu readback spesifik ini
#define GL4ES_READPIXELS_SYNC(x, y, w, h, fmt, type, dst)                    \
    do {                                                                       \
        if (globals4es.use_pbo && hardext.esversion >= 3 &&                   \
            (dst) != NULL && (w) > 0 && (h) > 0) {                           \
            if (!gl4es_readback_flush_sync((dst), (w), (h), (fmt), (type))) { \
                LOAD_GLES(glReadPixels);                                       \
                gles_glReadPixels((x), (y), (w), (h), (fmt), (type), (dst)); \
            }                                                                  \
        } else {                                                               \
            LOAD_GLES(glReadPixels);                                           \
            gles_glReadPixels((x), (y), (w), (h), (fmt), (type), (dst));     \
        }                                                                      \
    } while(0)

#endif // _GL4ES_PIXEL_READBACK_H_
