#ifndef _GL4ES_TEXTURE_PBO_H_
#define _GL4ES_TEXTURE_PBO_H_

// =============================================================================
//  texture_pbo.h — PBO Pool & Async Texture Upload API (GLES 3.0+)
//
//  Modul ini menyediakan PBO (Pixel Buffer Object) pool untuk mengakselerasi
//  texture upload di gl4es. Digunakan oleh texture.c untuk menggantikan
//  CPU-synchronous glTexImage2D / glTexSubImage2D dengan path asinkron DMA.
//
//  Target: GLES 3.0+ (use_pbo flag dari globals4es — diset di init.c)
//  Fallback: jika PBO tidak tersedia / gagal → kembali ke path synchronous
//
//  Keuntungan untuk Minecraft 1.12.2:
//  - Chunk loading: atlas texture (512×512–2048×2048) upload tidak block CPU
//  - Stutter berkurang: glTexImage2D returns immediately, CPU lanjut ke chunk berikutnya
//  - GPU handles DMA transfer: bandwidth GPU dipakai, bukan CPU bus
// =============================================================================

#include "../gl/loader.h"
#include "gles.h"

// ── Konstanta pool ────────────────────────────────────────────────────────────
// PBO_POOL_SIZE: jumlah PBO yang di-pre-allocate
// Gunakan 4 untuk ping-pong double-buffer dengan 2 frame lookahead.
// Minecraft 1.12.2 jarang upload > 4 texture besar secara bersamaan per frame.
#define PBO_POOL_SIZE    4

// Threshold minimum ukuran (byte) untuk menggunakan PBO.
// Texture kecil (< 64×64 RGBA = 16384 byte) lebih efisien di-upload langsung.
// Texture besar (atlas chunk) mendapat benefit signifikan dari PBO.
#define PBO_SIZE_THRESHOLD  (64 * 64 * 4)   // 16 KB

// Ukuran maksimum single PBO (8MB — cukup untuk atlas 2048x2048 RGBA)
#define PBO_MAX_SIZE     (8 * 1024 * 1024)

// =============================================================================
//  gl4es_pbo_entry_t — Satu slot PBO dalam pool
// =============================================================================
typedef struct {
    GLuint  id;         // OpenGL buffer object ID (0 = belum dialokasi)
    GLsizei capacity;   // Kapasitas byte yang saat ini dialokasi
    int     in_use;     // 1 = sedang digunakan oleh upload aktif
} gl4es_pbo_entry_t;

// =============================================================================
//  gl4es_pbo_pool_t — Pool PBO global (satu per context)
// =============================================================================
typedef struct {
    gl4es_pbo_entry_t slots[PBO_POOL_SIZE];
    int               next_slot;    // round-robin index untuk allocation
    int               inited;
} gl4es_pbo_pool_t;

// ── Global pool instance (diinisialisasi oleh texture_pbo_init) ────────────────
extern gl4es_pbo_pool_t gl4es_pbo_pool;

// =============================================================================
//  API Publik
// =============================================================================

// Inisialisasi pool PBO — dipanggil dari init.c setelah GetHardwareExtensions()
// hanya jika globals4es.use_pbo == 1
void texture_pbo_init(void);

// Cleanup seluruh pool PBO — dipanggil dari close_gl4es()
void texture_pbo_cleanup(void);

// ── gl4es_pbo_upload_tex_image ─────────────────────────────────────────────────
// Pengganti gles_glTexImage2D() untuk path upload dengan PBO.
//
// Ketika berhasil (return 1): upload dilakukan via PBO asinkron.
// Ketika gagal (return 0): caller harus fallback ke gles_glTexImage2D() langsung.
//
// Alur:
//  1. Hitung size = width * height * pixel_sizeof(format, type)
//  2. Jika size < PBO_SIZE_THRESHOLD atau use_pbo=0 → return 0 (fallback)
//  3. Cari PBO slot bebas di pool
//  4. glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo_id)
//  5. glBufferData(..., GL_STREAM_DRAW) — alokasi / reuse
//  6. glMapBufferRange(write+invalidate) → copy pixels → glUnmapBuffer
//  7. glTexImage2D(... offset=0 ...) → driver melakukan DMA async
//  8. glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0) — unbind
int gl4es_pbo_upload_tex_image(GLenum target, GLint level,
                                GLenum internalformat,
                                GLsizei width, GLsizei height,
                                GLint border,
                                GLenum format, GLenum type,
                                const GLvoid *pixels);

// ── gl4es_pbo_upload_tex_subimage ─────────────────────────────────────────────
// Pengganti gles_glTexSubImage2D() untuk path upload dengan PBO.
//
// Return 1 = berhasil, 0 = fallback ke synchronous path.
int gl4es_pbo_upload_tex_subimage(GLenum target, GLint level,
                                   GLint xoffset, GLint yoffset,
                                   GLsizei width, GLsizei height,
                                   GLenum format, GLenum type,
                                   const GLvoid *pixels);

// ── Macro helper untuk texture.c ──────────────────────────────────────────────
// Gunakan macro ini di tempat gles_glTexImage2D() dipanggil untuk otomatis
// memilih PBO path atau synchronous path berdasarkan kondisi runtime.
//
// CATATAN: hanya pakai di path yang TIDAK sedang compiling display list
// dan BUKAN path null-data (pixels == NULL).

#define GL4ES_TEX_IMAGE_2D(target, level, internalformat, width, height, \
                            border, format, type, pixels)                  \
    do {                                                                   \
        if (globals4es.use_pbo && (pixels) != NULL &&                      \
            !gl4es_pbo_upload_tex_image((target), (level), (internalformat),\
                (width), (height), (border), (format), (type), (pixels))) {\
            gles_glTexImage2D((target), (level), (internalformat),          \
                (width), (height), (border), (format), (type), (pixels));  \
        } else if (!(globals4es.use_pbo) || (pixels) == NULL) {            \
            gles_glTexImage2D((target), (level), (internalformat),          \
                (width), (height), (border), (format), (type), (pixels));  \
        }                                                                   \
    } while(0)

#define GL4ES_TEX_SUBIMAGE_2D(target, level, xoffset, yoffset,           \
                               width, height, format, type, pixels)       \
    do {                                                                   \
        if (globals4es.use_pbo && (pixels) != NULL &&                      \
            !gl4es_pbo_upload_tex_subimage((target), (level), (xoffset),   \
                (yoffset), (width), (height), (format), (type), (pixels))){\
            gles_glTexSubImage2D((target), (level), (xoffset), (yoffset),  \
                (width), (height), (format), (type), (pixels));           \
        } else if (!(globals4es.use_pbo) || (pixels) == NULL) {            \
            gles_glTexSubImage2D((target), (level), (xoffset), (yoffset),  \
                (width), (height), (format), (type), (pixels));           \
        }                                                                   \
    } while(0)

#endif // _GL4ES_TEXTURE_PBO_H_
