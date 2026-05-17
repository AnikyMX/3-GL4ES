// =============================================================================
//  list_vao.c — Per-renderlist Native VAO Cache (GLES 3.0+)
//
//  Implementasi native VAO cache untuk display list rendering.
//
//  Cara kerja per list:
//
//  Frame 1 (list belum punya VAO):
//    list->vao_id == 0
//    → buat native VAO baru
//    → bind VBO (sudah ada dari list2VBO)
//    → setup semua glVertexAttribPointer SEKALI
//    → bind IBO jika ada indices
//    → simpan vao_id + fingerprint
//    → draw
//
//  Frame 2+ (list sudah punya VAO, tidak berubah):
//    list->vao_cfg_fingerprint == fp_baru  (tidak berubah)
//    → glBindVertexArray(list->vao_id)     ← SATU instruksi saja
//    → draw
//    → total overhead per frame: ~2 GL calls vs ~20+ di path lama
//
//  Frame setelah VBO update (jarang, terjadi saat list di-compile ulang):
//    fingerprint berubah
//    → invalidate VAO lama, buat baru
//
//  Benefit untuk Minecraft 1.12.2:
//  - Chunk display list dipanggil ratusan-ribuan kali per frame
//  - Setiap chunk: dari ~25 GL calls → 2 GL calls
//  - Estimasi saving: 90%+ overhead per display list draw call
// =============================================================================

#include "list_vao.h"

#include <stdlib.h>
#include <string.h>

#include "fpe.h"
#include "gl4es.h"
#include "glstate.h"
#include "render.h"
#include "loader.h"
#include "debug.h"
#include "logs.h"
#include "../glx/hardext.h"
#include "../gl/init.h"

//#define DEBUG_LIST_VAO
#ifdef DEBUG_LIST_VAO
#define LVDBG(a) a
#else
#define LVDBG(a)
#endif

// =============================================================================
//  list_vao_invalidate — Hapus native VAO milik satu renderlist
// =============================================================================
void list_vao_invalidate(renderlist_t *list) {
    if (!list) return;
    if (list->vao_id) {
        LOAD_GLES2(glDeleteVertexArrays);
        if (gles_glDeleteVertexArrays)
            gles_glDeleteVertexArrays(1, &list->vao_id);
        list->vao_id = 0;
        LVDBG(printf("list_vao: invalidated vao for list %p\n", list);)
    }
    if (list->ibo_id) {
        LOAD_GLES(glDeleteBuffers);
        if (gles_glDeleteBuffers)
            gles_glDeleteBuffers(1, &list->ibo_id);
        list->ibo_id = 0;
    }
    list->vao_cfg_fingerprint = 0;
}

// =============================================================================
//  list_vao_cleanup_all — Traverse linked list dan invalidate semua VAO
// =============================================================================
void list_vao_cleanup_all(void) {
    // Dipanggil dari close_gl4es() — context destroy sudah handle ini
    // tapi kita cleanup pointer untuk konsistensi
    // Tidak perlu traversal global karena GL context destroy melepas semua resources
    SHUT_LOGD("list_vao_cleanup_all: context destroy akan membersihkan GPU resources\n");
}

// =============================================================================
//  list_vao_create — Buat native VAO baru untuk renderlist
//
//  Dipanggil ketika:
//  - list->vao_id == 0 (belum ada)
//  - fingerprint berubah (konfigurasi berubah)
//
//  Mengembalikan vao_id baru (> 0) atau 0 jika gagal.
//
//  CATATAN PENTING tentang pointer semantik:
//  Setelah list2VBO, list->vbo_vert dst. adalah POINTER (uintptr_t) yang
//  merepresentasikan OFFSET dalam VBO (bukan alamat CPU).
//  Dalam konteks VBO bound, nilai pointer ini adalah byte offset dari
//  awal VBO → langsung bisa dipakai sebagai 5th arg glVertexAttribPointer.
// =============================================================================
static GLuint list_vao_create(renderlist_t *list) {
    LOAD_GLES2(glGenVertexArrays);
    LOAD_GLES2(glBindVertexArray);
    LOAD_GLES2(glVertexAttribPointer);
    LOAD_GLES2(glEnableVertexAttribArray);
    LOAD_GLES2(glDisableVertexAttribArray);
    LOAD_GLES(glBindBuffer);
    LOAD_GLES(glGetError);

    if (!gles_glGenVertexArrays || !gles_glBindVertexArray ||
        !gles_glVertexAttribPointer || !gles_glBindBuffer)
        return 0;

    // Hapus VAO lama jika ada
    if (list->vao_id) {
        LOAD_GLES2(glDeleteVertexArrays);
        if (gles_glDeleteVertexArrays)
            gles_glDeleteVertexArrays(1, &list->vao_id);
        list->vao_id = 0;
    }

    // Buat native VAO baru
    GLuint vao_id = 0;
    gles_glGenVertexArrays(1, &vao_id);
    if (!vao_id || gles_glGetError() != GL_NO_ERROR) return 0;

    // Bind VAO — semua state di bawah ini akan direkam dalam VAO
    gles_glBindVertexArray(vao_id);

    // Bind VBO sumber data
    // list->vbo_array sudah dibuat oleh list2VBO() dengan glGenBuffers + glBufferData
    gles_glBindBuffer(GL_ARRAY_BUFFER, list->vbo_array);

    // ── Setup vertex attrib pointers ────────────────────────────────────────
    // Semua pointer di bawah adalah OFFSET dalam VBO (bukan pointer CPU)
    // karena VBO sedang di-bind ke GL_ARRAY_BUFFER
    //
    // Layout standar gl4es (lihat array.h ATT_* constants):
    //   ATT_VERTEX = 0, size=4 float (xyzw)
    //   ATT_COLOR  = 3, size=4 float (rgba)
    //   ATT_NORMAL = 2, size=3 float
    //   ATT_SECONDARY = 4, size=4 float
    //   ATT_FOGCOORD  = 5, size=1 float
    //   ATT_MULTITEXCOORD0..N = 8..8+N, size=4 float

    if (list->vert) {
        int stride = list->vert_stride ? list->vert_stride : (4 * sizeof(GLfloat));
        gles_glEnableVertexAttribArray(ATT_VERTEX);
        gles_glVertexAttribPointer(ATT_VERTEX, 4, GL_FLOAT, GL_FALSE, stride,
                                    list->vbo_vert);
        LVDBG(printf("  att_vertex: stride=%d offset=%p\n", stride, list->vbo_vert);)
    } else {
        if (gles_glDisableVertexAttribArray)
            gles_glDisableVertexAttribArray(ATT_VERTEX);
    }

    if (list->color) {
        int stride = list->color_stride ? list->color_stride : (4 * sizeof(GLfloat));
        gles_glEnableVertexAttribArray(ATT_COLOR);
        gles_glVertexAttribPointer(ATT_COLOR, 4, GL_FLOAT, GL_FALSE, stride,
                                    list->vbo_color);
    } else {
        if (gles_glDisableVertexAttribArray)
            gles_glDisableVertexAttribArray(ATT_COLOR);
    }

    if (list->normal) {
        int stride = list->normal_stride ? list->normal_stride : (3 * sizeof(GLfloat));
        gles_glEnableVertexAttribArray(ATT_NORMAL);
        gles_glVertexAttribPointer(ATT_NORMAL, 3, GL_FLOAT, GL_FALSE, stride,
                                    list->vbo_normal);
    } else {
        if (gles_glDisableVertexAttribArray)
            gles_glDisableVertexAttribArray(ATT_NORMAL);
    }

    if (list->secondary) {
        int stride = list->secondary_stride ? list->secondary_stride : (4 * sizeof(GLfloat));
        gles_glEnableVertexAttribArray(ATT_SECONDARY);
        gles_glVertexAttribPointer(ATT_SECONDARY, 4, GL_FLOAT, GL_FALSE, stride,
                                    list->vbo_secondary);
    } else {
        if (gles_glDisableVertexAttribArray)
            gles_glDisableVertexAttribArray(ATT_SECONDARY);
    }

    if (list->fogcoord) {
        int stride = list->fogcoord_stride ? list->fogcoord_stride : (1 * sizeof(GLfloat));
        gles_glEnableVertexAttribArray(ATT_FOGCOORD);
        gles_glVertexAttribPointer(ATT_FOGCOORD, 1, GL_FLOAT, GL_FALSE, stride,
                                    list->vbo_fogcoord);
    } else {
        if (gles_glDisableVertexAttribArray)
            gles_glDisableVertexAttribArray(ATT_FOGCOORD);
    }

    for (int i = 0; i < list->maxtex && i < hardext.maxtex; i++) {
        if (list->tex[i]) {
            int stride = list->tex_stride[i] ? list->tex_stride[i] : (4 * sizeof(GLfloat));
            int att = ATT_MULTITEXCOORD0 + i;
            gles_glEnableVertexAttribArray(att);
            gles_glVertexAttribPointer(att, 4, GL_FLOAT, GL_FALSE, stride,
                                        list->vbo_tex[i]);
        } else {
            if (gles_glDisableVertexAttribArray)
                gles_glDisableVertexAttribArray(ATT_MULTITEXCOORD0 + i);
        }
    }

    // ── Bind IBO (indices) ────────────────────────────────────────────────────
    // Jika list punya indices, kita perlu EBO (Element Buffer Object).
    // Cek apakah vbo_indices sudah ada (set oleh list2VBO jika ada indices)
    if (list->vbo_indices) {
        // EBO sudah ada dari list2VBO — langsung bind
        gles_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, list->vbo_indices);
        LVDBG(printf("  ibo: using existing vbo_indices=%u\n", list->vbo_indices);)
    } else if (list->indices && list->ilen > 0 && !list->ibo_id) {
        // Indices ada tapi belum di-upload ke GPU (list2VBO tidak selalu buat IBO)
        // Buat EBO baru dan upload indices sekarang (sekali saja)
        LOAD_GLES(glGenBuffers);
        LOAD_GLES(glBufferData);
        if (gles_glGenBuffers && gles_glBufferData) {
            GLuint ibo = 0;
            gles_glGenBuffers(1, &ibo);
            if (ibo) {
                gles_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
                gles_glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                                  list->ilen * sizeof(GLushort),
                                  list->indices,
                                  GL_STATIC_DRAW);
                if (gles_glGetError() == GL_NO_ERROR) {
                    list->ibo_id = ibo;
                    LVDBG(printf("  ibo: created new ibo=%u, ilen=%lu\n",
                                  ibo, list->ilen);)
                } else {
                    gles_glDeleteBuffers(1, &ibo);
                    // Gagal buat IBO → unbind VAO, abort
                    gles_glBindVertexArray(0);
                    LOAD_GLES2(glDeleteVertexArrays);
                    if (gles_glDeleteVertexArrays)
                        gles_glDeleteVertexArrays(1, &vao_id);
                    return 0;
                }
            }
        }
    } else if (list->ibo_id) {
        // IBO sudah ada dari sebelumnya (re-creation VAO karena fingerprint berubah)
        gles_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, list->ibo_id);
    }

    // Unbind VAO → rekaman konfigurasi selesai
    gles_glBindVertexArray(0);
    // Unbind buffer agar tidak polusi state global
    gles_glBindBuffer(GL_ARRAY_BUFFER, 0);
    gles_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    LVDBG(printf("list_vao_create: created vao=%u for list %p (vert=%p color=%p)\n",
                  vao_id, list, list->vert, list->color);)
    return vao_id;
}

// =============================================================================
//  list_vao_draw_pure — Main entry point
//
//  Dipanggil dari LIST_VAO_FAST_PATH macro di listdraw.c.
//  Return 1 jika draw berhasil via native VAO.
//  Return 0 jika harus fallback ke path lama.
// =============================================================================
int list_vao_draw_pure(renderlist_t *list)
{
    // ── Guard conditions ──────────────────────────────────────────────────────
    if (!globals4es.use_native_vao)    return 0;
    if (hardext.esversion < 3)         return 0;
    if (!ispurerender_renderlist(list)) return 0;
    if (list->use_vbo_array != 2)      return 0;  // VBO belum terbuat
    if (!list->vert)                   return 0;  // Tidak ada vertex data
    if (!list->vbo_array)              return 0;  // VBO array belum ada

    // ── Hitung fingerprint ────────────────────────────────────────────────────
    uint32_t fp = list_vao_fingerprint(list);

    // ── Buat / validasi VAO ───────────────────────────────────────────────────
    if (list->vao_id == 0 || list->vao_cfg_fingerprint != fp) {
        LVDBG(printf("list_vao_draw_pure: (re)creating VAO for list %p "
                     "(old_id=%u fp_old=%u fp_new=%u)\n",
                     list, list->vao_id, list->vao_cfg_fingerprint, fp);)

        GLuint new_id = list_vao_create(list);
        if (!new_id) {
            LVDBG(printf("list_vao_draw_pure: VAO creation failed, fallback\n");)
            return 0;
        }
        list->vao_id              = new_id;
        list->vao_cfg_fingerprint = fp;
    }

    // ── Bind VAO → satu instruksi menggantikan ~20 GL calls ───────────────────
    LOAD_GLES2(glBindVertexArray);
    LOAD_GLES_FPE(glDrawElements);
    LOAD_GLES_FPE(glDrawArrays);
    if (!gles_glBindVertexArray) return 0;

    gles_glBindVertexArray(list->vao_id);

    // ── Realize textures (FPE state — wajib sebelum draw) ─────────────────────
    realize_textures(1);

    // ── Draw ──────────────────────────────────────────────────────────────────
    if (list->ilen > 0) {
        // Draw dengan indices (EBO terikat dalam VAO)
        gles_glDrawElements(list->mode,
                             (GLsizei)list->ilen,
                             GL_UNSIGNED_SHORT,
                             (const void*)0);   // offset 0 dari IBO
    } else {
        // Draw tanpa indices (glDrawArrays)
        gles_glDrawArrays(list->mode, 0, (GLsizei)list->len);
    }

    // ── Post-draw: unbind VAO ─────────────────────────────────────────────────
    // Mali driver: unbind setelah draw untuk hindari state pollution antar draw calls
    if (hardext.vendor & VEND_ARM) {
        gles_glBindVertexArray(0);
    }

    LVDBG(printf("list_vao_draw_pure: SUCCESS vao=%u mode=%X len=%lu ilen=%lu\n",
                  list->vao_id, list->mode, list->len, list->ilen);)
    return 1;
}
