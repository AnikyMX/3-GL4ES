#ifndef _GL4ES_LIST_VAO_H_
#define _GL4ES_LIST_VAO_H_

// =============================================================================
//  list_vao.h — Per-renderlist Native VAO Cache (GLES 3.0+)
//
//  Modul ini mengakselerasi draw_renderlist() untuk display list yang berisi
//  geometri murni (ispurerender_renderlist() == true).
//
//  MASALAH ORIGINAL:
//  draw_renderlist() memanggil listActiveVBO() setiap frame untuk setup
//  vertex attrib state ke glstate->vao. Ini mahal karena:
//  1. Memodifikasi per-attrib glstate setiap draw call
//  2. Driver harus validasi kembali seluruh pipeline state
//  3. Untuk MC 1.12.2: ribuan display list chunk di-replay setiap frame
//
//  SOLUSI — NATIVE VAO FAST PATH (GLES 3.0+):
//  Setiap renderlist yang pure-render dan sudah punya VBO (use_vbo_array == 2)
//  mendapatkan satu native VAO yang dikonfigurasi SEKALI saat pertama kali draw.
//  Frame berikutnya: hanya glBindVertexArray(id) → glDraw*().
//
//  Ini menghilangkan seluruh overhead listActiveVBO/listInactiveVBO
//  dan per-attrib glVertexAttribPointer setiap frame.
//
//  Kondisi aktifasi:
//  1. globals4es.use_native_vao == 1  (GLES 3.0+, set di init.c)
//  2. ispurerender_renderlist() == true
//  3. list->use_vbo_array == 2        (VBO sudah terbuat oleh list2VBO)
//  4. list->vao_id == 0               (belum punya VAO) ATAU fingerprint berubah
//
//  Fallback: jika kondisi di atas tidak terpenuhi → path lama (listActiveVBO)
//
//  Field yang ditambah ke renderlist_t (via patch list.h):
//    GLuint vao_id;       // native VAO id, 0 = belum ada / invalid
//    GLuint ibo_id;       // EBO terpisah jika indices bukan dalam vbo_array
//    uint32_t vao_cfg_fingerprint; // hash konfigurasi attrib saat VAO dibuat
// =============================================================================

#include "list.h"
#include "buffers.h"   // ATT_VERTEX, ATT_COLOR, ATT_NORMAL, ATT_MULTITEXCOORD0, dll.
#include "loader.h"
#include "gles.h"
#include "../gl/init.h"
#include "../glx/hardext.h"

// =============================================================================
//  API Publik
// =============================================================================

// ── list_vao_draw_pure ─────────────────────────────────────────────────────────
// Entry point utama. Dipanggil dari draw_renderlist() sebelum path lama.
//
// Alur:
//  1. Cek kondisi (use_native_vao, ispurerender, use_vbo_array==2)
//  2. Hitung fingerprint konfigurasi attrib saat ini
//  3. Jika list->vao_id == 0 atau fingerprint berubah:
//     a. Delete VAO lama jika ada
//     b. Buat native VAO baru
//     c. glBindVertexArray(new_id)
//     d. Bind VBO + setup attrib via glVertexAttribPointer
//     e. Bind IBO (indices) jika ada
//     f. Simpan vao_id + fingerprint baru
//  4. glBindVertexArray(list->vao_id)
//  5. call realize_textures(1)
//  6. glDraw* sesuai ilen/len
//  7. glBindVertexArray(0) jika VEND_ARM
//
// Return 1 jika draw berhasil via native VAO.
// Return 0 jika fallback ke draw_renderlist path lama.
int list_vao_draw_pure(renderlist_t *list);

// ── list_vao_invalidate ────────────────────────────────────────────────────────
// Hapus native VAO milik list ini (dipanggil dari free_renderlist).
// Juga dipanggil jika VBO di-update (list2VBO dijalankan ulang).
void list_vao_invalidate(renderlist_t *list);

// ── list_vao_cleanup_all ───────────────────────────────────────────────────────
// Dipanggil dari close_gl4es() untuk membersihkan semua VAO yang masih aktif.
// (opsional — biasanya context destroy sudah membersihkan semua GL objects)
void list_vao_cleanup_all(void);

// =============================================================================
//  Fingerprint helper — 32-bit hash dari konfigurasi VBO attrib renderlist
//
//  Menggabungkan: bit attrib aktif + vbo_array id + pointer offset setiap attrib
//  Cukup 32-bit karena renderlist jarang berubah setelah di-compile.
// =============================================================================
static inline uint32_t list_vao_fingerprint(const renderlist_t *list) {
    uint32_t fp = 0;
    uint32_t mask = 0;
    if (list->vert)      { mask |= (1u << 0); fp ^= (uint32_t)(uintptr_t)list->vbo_vert;      }
    if (list->color)     { mask |= (1u << 1); fp ^= (uint32_t)(uintptr_t)list->vbo_color;     fp = (fp << 3) | (fp >> 29); }
    if (list->secondary) { mask |= (1u << 2); fp ^= (uint32_t)(uintptr_t)list->vbo_secondary; fp = (fp << 5) | (fp >> 27); }
    if (list->fogcoord)  { mask |= (1u << 3); fp ^= (uint32_t)(uintptr_t)list->vbo_fogcoord;  fp = (fp << 7) | (fp >> 25); }
    if (list->normal)    { mask |= (1u << 4); fp ^= (uint32_t)(uintptr_t)list->vbo_normal;    fp = (fp << 11)| (fp >> 21); }
    for (int i = 0; i < MAX_TEX; i++) {
        if (list->tex[i]) {
            mask |= (1u << (5 + i));
            fp ^= (uint32_t)(uintptr_t)list->vbo_tex[i];
            fp = (fp << 13) | (fp >> 19);
        }
    }
    fp ^= (mask << 16) | (uint32_t)list->vbo_array;
    fp ^= (uint32_t)list->len;
    if (list->indices) fp ^= (uint32_t)(uintptr_t)list->indices ^ (uint32_t)list->ilen;
    return fp;
}

// =============================================================================
//  Macro untuk patch di listdraw.c
//
//  Tempatkan di awal blok draw geometri di draw_renderlist(), setelah
//  list2VBO dipanggil (use_vbo_array==2 terpenuhi).
//
//  Jika LIST_VAO_FAST_PATH return 1 → langsung lanjut ke iterasi berikutnya.
//  Jika return 0 → jatuh ke path lama (listActiveVBO + fpe_glDrawArrays).
// =============================================================================
#define LIST_VAO_FAST_PATH(list)                                         \
    (globals4es.use_native_vao &&                                        \
     hardext.esversion >= 3 &&                                           \
     list_vao_draw_pure(list))

#endif // _GL4ES_LIST_VAO_H_
