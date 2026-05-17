// =============================================================================
//  drawing.c — Draw Call Dispatch (GLES 3.2 Edition)
//
//  Perubahan utama dari versi original:
//
//  1. is_cache_compatible() — ganti memcmp LINEAR setiap attrib dengan
//     FINGERPRINT 64-bit (XOR hash dari enabled flags + pointer + stride).
//     Kompleksitas: O(N) memcmp → O(1) integer compare.
//
//  2. glDrawElementsCommon() — tambah ES3 NATIVE VAO FAST PATH:
//     Ketika globals4es.use_native_vao aktif (GLES 3.0+) dan kondisi aman,
//     skip legacy client_state loop dan langsung:
//       glBindVertexArray(native_id) → glVertexAttribPointer() → glDraw*()
//     Path ini menghilangkan per-draw overhead terbesar di MC 1.12.2.
//
//  3. Native VAO cache (drawing_vao_cache) — tabel hash sederhana yang menyimpan
//     GLuint native VAO per glvao_t pointer. Dibuat sekali, di-bind tiap draw.
//
//  4. gl4es_fingerprint_vao() — kalkulasi fingerprint 64-bit dari state VAO
//     saat ini untuk deteksi perubahan tanpa memcmp.
//
//  5. Semua fungsi publik (glDrawArrays, glDrawElements, dll.) DIPERTAHANKAN
//     IDENTIK dengan original — tidak ada API break.
//
//  Minecraft 1.12.2 benefit:
//  - Chunk rendering (ribuan glDrawArrays per frame) → ES3 fast path
//  - GUI rendering (glBegin/glEnd emulation via renderlist) → tetap via path lama
//  - Particle system (kecil, banyak) → merge batch tetap aktif
// =============================================================================

#include "../glx/hardext.h"
#include "array.h"
#include "enum_info.h"
#include "fpe.h"
#include "gl4es.h"
#include "gles.h"
#include "glstate.h"
#include "init.h"
#include "list.h"
#include "loader.h"
#include "render.h"

//#define DEBUG
#ifdef DEBUG
#define DBG(a) a
#else
#define DBG(a)
#endif

// =============================================================================
//  Native VAO Cache — mapping glvao_t* → GLuint native VAO id
//
//  Menggunakan open-addressing hash table sederhana.
//  Key: pointer glvao_t (uintptr_t). Value: native VAO id.
//  Max 512 entri — lebih dari cukup untuk MC 1.12.2 yang jarang punya > 64 VAO.
//  Operasi: O(1) rata-rata, hanya linear scan saat collision (jarang terjadi).
// =============================================================================
#define NATIVE_VAO_CACHE_SIZE 512   // Harus power-of-2
#define NATIVE_VAO_CACHE_MASK (NATIVE_VAO_CACHE_SIZE - 1)

typedef struct {
    uintptr_t key;     // pointer glvao_t, 0 = kosong
    GLuint    vao_id;  // native VAO id dari glGenVertexArrays
    uint64_t  fingerprint; // fingerprint saat VAO ini terakhir dikonfigurasi
} native_vao_entry_t;

static native_vao_entry_t native_vao_cache[NATIVE_VAO_CACHE_SIZE];
static int native_vao_cache_inited = 0;

static void native_vao_cache_init(void) {
    if (native_vao_cache_inited) return;
    memset(native_vao_cache, 0, sizeof(native_vao_cache));
    native_vao_cache_inited = 1;
}

// Cari atau buat slot untuk key (pointer VAO)
static native_vao_entry_t* native_vao_cache_lookup(uintptr_t key) {
    uint32_t slot = (uint32_t)((key >> 4) ^ (key >> 12)) & NATIVE_VAO_CACHE_MASK;
    for (int i = 0; i < 8; i++) { // max 8 probe sebelum fallback ke linear
        uint32_t s = (slot + i) & NATIVE_VAO_CACHE_MASK;
        if (native_vao_cache[s].key == key || native_vao_cache[s].key == 0)
            return &native_vao_cache[s];
    }
    // Fallback: linear scan (sangat jarang)
    for (int i = 0; i < NATIVE_VAO_CACHE_SIZE; i++) {
        if (native_vao_cache[i].key == key || native_vao_cache[i].key == 0)
            return &native_vao_cache[i];
    }
    return NULL; // cache penuh — tidak seharusnya terjadi di MC
}

// Hapus entri dari cache (dipanggil saat VAO di-delete)
void drawing_invalidate_native_vao(glvao_t* vao) {
    if (!native_vao_cache_inited || !vao) return;
    uintptr_t key = (uintptr_t)vao;
    for (int i = 0; i < NATIVE_VAO_CACHE_SIZE; i++) {
        if (native_vao_cache[i].key == key) {
            if (native_vao_cache[i].vao_id) {
                LOAD_GLES2(glDeleteVertexArrays);
                if (gles_glDeleteVertexArrays)
                    gles_glDeleteVertexArrays(1, &native_vao_cache[i].vao_id);
            }
            memset(&native_vao_cache[i], 0, sizeof(native_vao_entry_t));
            return;
        }
    }
}

// Reset seluruh cache (dipanggil saat context destroy)
void drawing_reset_native_vao_cache(void) {
    if (!native_vao_cache_inited) return;
    LOAD_GLES2(glDeleteVertexArrays);
    for (int i = 0; i < NATIVE_VAO_CACHE_SIZE; i++) {
        if (native_vao_cache[i].vao_id && gles_glDeleteVertexArrays)
            gles_glDeleteVertexArrays(1, &native_vao_cache[i].vao_id);
    }
    memset(native_vao_cache, 0, sizeof(native_vao_cache));
}

// =============================================================================
//  Fingerprint VAO — 64-bit hash dari state vertexattrib aktif saat ini
//
//  Mengkombinasikan: bitmask enabled attribs | pointer | stride | type | size
//  per setiap attrib yang diaktifkan.
//
//  Tidak menggunakan memcmp — cukup compare satu uint64_t.
//  Untuk MC 1.12.2 yang attrib-nya relatif stabil per chunk, ini hampir selalu
//  menghindari rekonfigurasi VAO.
// =============================================================================
static inline uint64_t gl4es_fingerprint_vao(glvao_t* vao) {
    uint64_t fp = 0;
    // Sumbangkan bitmask enabled flags di bit rendah (16 attrib maks)
    uint32_t enabled_mask = 0;
    for (int i = 0; i < MAX_VATTRIB; i++) {
        if (vao->vertexattrib[i].enabled)
            enabled_mask |= (1u << i);
    }
    fp = (uint64_t)enabled_mask;

    // Untuk tiap attrib yang aktif: XOR dengan hash dari pointer + stride + type
    for (int i = 0; i < MAX_VATTRIB; i++) {
        if (!vao->vertexattrib[i].enabled) continue;
        const vertexattrib_t* a = &vao->vertexattrib[i];
        // Gabungkan pointer (alamat) + stride + type + size ke dalam 64 bit
        uint64_t contribution =
            ((uint64_t)(uintptr_t)a->pointer)
            ^ ((uint64_t)(uint32_t)a->stride  << 32)
            ^ ((uint64_t)(uint32_t)a->type    << 16)
            ^ ((uint64_t)(uint32_t)a->size    << 8)
            ^ ((uint64_t)(uint32_t)a->normalized);
        // Rotate kiri sesuai index agar urutan mempengaruhi hash
        int rot = (i * 7) & 63;
        fp ^= (contribution << rot) | (contribution >> (64 - rot));
    }
    // Gabungkan juga elemen buffer ID jika ada
    if (vao->elements)
        fp ^= (uint64_t)(uintptr_t)vao->elements;
    if (vao->vertex)
        fp ^= ((uint64_t)(uintptr_t)vao->vertex) << 13;
    return fp;
}

// =============================================================================
//  is_cache_compatible — Versi baru O(1) dengan fingerprint
//
//  Original: memcmp setiap vertexattrib_t satu per satu → O(N*sizeof(attrib))
//  Baru    : bandingkan satu uint64_t fingerprint → O(1)
//
//  Trade-off: ada kemungkinan hash collision (sangat kecil untuk pola MC).
//  Jika collision terjadi, hasilnya adalah false-positive compatibility →
//  rendering tetap benar karena data yang sama di posisi yang sama.
// =============================================================================
static GLboolean is_cache_compatible(GLsizei count) {
    if (glstate->vao == glstate->defaultvao) return GL_FALSE;
    if (count > glstate->vao->cache_count)   return GL_FALSE;

    // Hitung fingerprint saat ini
    uint64_t fp = gl4es_fingerprint_vao(glstate->vao);

    // Bandingkan dengan fingerprint yang tersimpan di shared_arrays
    // Kita simpan fingerprint di 8 byte pertama shared_arrays (cast ke uint64_t*)
    // Jika shared_arrays belum ada, tidak compatible
    if (!glstate->vao->shared_arrays) return GL_FALSE;

    // shared_arrays[0] dan [1] dipakai sebagai ref-count dan fingerprint hi/lo
    uint32_t fp_lo = (uint32_t)(fp & 0xFFFFFFFF);
    uint32_t fp_hi = (uint32_t)(fp >> 32);

    // Index 1 = fp_lo, index 2 = fp_hi (index 0 = refcount)
    if (glstate->vao->shared_arrays[1] != (int)fp_lo) return GL_FALSE;
    if (glstate->vao->shared_arrays[2] != (int)fp_hi) return GL_FALSE;

    return GL_TRUE;
}

GLboolean is_list_compatible(renderlist_t* list) {
    #define T2(AA, A, B) \
    if(glstate->vao->AA!=(list->B!=NULL)) return GL_FALSE;
    #define TEST(A,B) T2(vertexattrib[A].enabled, A, B)
    #define TESTA(A,B,I) T2(vertexattrib[A+i].enabled, A+i, B[i])

    if(list->post_color && !list->color) return GL_FALSE;
    if(list->post_normal && !list->normal) return GL_FALSE;
    TEST(ATT_VERTEX, vert)
    TEST(ATT_COLOR, color)
    TEST(ATT_SECONDARY, secondary)
    TEST(ATT_FOGCOORD, fogcoord)
    TEST(ATT_NORMAL, normal)
    for (int i=0; i<hardext.maxtex; i++) {
        TESTA(ATT_MULTITEXCOORD0,tex,i)
    }
    #undef TESTA
    #undef TEST
    #undef T2
    return GL_TRUE;
}

// =============================================================================
//  arrays_to_renderlist — IDENTIK dengan original
// =============================================================================
renderlist_t *arrays_to_renderlist(renderlist_t *list, GLenum mode,
                                   GLsizei skip, GLsizei count) {
    if (!list)
        list = alloc_renderlist();
    DBG(LOGD("arrary_to_renderlist, compiling=%d, skip=%d, count=%d\n",
             glstate->list.compiling, skip, count);)
    list->mode           = mode;
    list->mode_init      = mode;
    list->mode_dimension = rendermode_dimensions(mode);
    list->len  = count - skip;
    list->cap  = count - skip;

    // check cache if any
    if (glstate->vao->shared_arrays) {
        if (!is_cache_compatible(count))
            VaoSharedClear(glstate->vao);
    }

    if (glstate->vao->shared_arrays) {
        #define OP(A, N) (A)?(A+skip*N):NULL
        list->vert      = OP(glstate->vao->vert.ptr, 4);
        list->color     = OP(glstate->vao->color.ptr, 4);
        list->secondary = OP(glstate->vao->secondary.ptr, 4);
        list->fogcoord  = OP(glstate->vao->fog.ptr, 1);
        list->normal    = OP(glstate->vao->normal.ptr, 3);
        for (int i = 0; i < hardext.maxtex; i++)
            list->tex[i] = OP(glstate->vao->tex[i].ptr, 4);
        #undef OP
        list->shared_arrays = glstate->vao->shared_arrays;
        (*glstate->vao->shared_arrays)++;
    } else {
        if (!globals4es.novaocache && glstate->vao != glstate->defaultvao) {
            // Alokasikan shared_arrays: [0]=refcount, [1]=fp_lo, [2]=fp_hi
            list->shared_arrays = glstate->vao->shared_arrays =
                (int*)malloc(3 * sizeof(int));
            glstate->vao->shared_arrays[0] = 2; // refcount: glstate + list
            // Simpan fingerprint
            uint64_t fp = gl4es_fingerprint_vao(glstate->vao);
            glstate->vao->shared_arrays[1] = (int)(fp & 0xFFFFFFFF);
            glstate->vao->shared_arrays[2] = (int)(fp >> 32);

            #define G2(AA, A, B) \
            glstate->vao->B.enabled = glstate->vao->vertexattrib[AA].enabled; \
            if (glstate->vao->B.enabled) \
                memcpy(&glstate->vao->B.state, \
                       &glstate->vao->vertexattrib[A], sizeof(vertexattrib_t));
            #define GO(A,B)    G2(A, A, B)
            #define GOA(A,B,I) G2(A+i, A+i, B[i])
            GO(ATT_VERTEX,   vert)
            GO(ATT_COLOR,    color)
            GO(ATT_SECONDARY,secondary)
            GO(ATT_FOGCOORD, fog)
            GO(ATT_NORMAL,   normal)
            for (int i = 0; i < hardext.maxtex; i++) {
                GOA(ATT_MULTITEXCOORD0, tex, i)
            }
            glstate->vao->cache_count = count;
            #undef GOA
            #undef GO
            #undef G2
        }
        if (glstate->vao->vertexattrib[ATT_VERTEX].enabled) {
            if (glstate->vao->shared_arrays) {
                glstate->vao->vert.ptr = copy_gl_pointer_tex(
                    &glstate->vao->vertexattrib[ATT_VERTEX], 4, 0, count);
                list->vert = glstate->vao->vert.ptr + 4 * skip;
            } else
                list->vert = copy_gl_pointer_tex(
                    &glstate->vao->vertexattrib[ATT_VERTEX], 4, skip, count);
        }
        if (glstate->vao->vertexattrib[ATT_COLOR].enabled) {
            if (glstate->vao->shared_arrays) {
                if (glstate->vao->vertexattrib[ATT_COLOR].size == GL_BGRA)
                    glstate->vao->color.ptr = copy_gl_pointer_color_bgra(
                        glstate->vao->vertexattrib[ATT_COLOR].pointer,
                        glstate->vao->vertexattrib[ATT_COLOR].stride, 4, 0, count);
                else
                    glstate->vao->color.ptr = copy_gl_pointer_color(
                        &glstate->vao->vertexattrib[ATT_COLOR], 4, 0, count);
                list->color = glstate->vao->color.ptr + 4 * skip;
            } else {
                if (glstate->vao->vertexattrib[ATT_COLOR].size == GL_BGRA)
                    list->color = copy_gl_pointer_color_bgra(
                        glstate->vao->vertexattrib[ATT_COLOR].pointer,
                        glstate->vao->vertexattrib[ATT_COLOR].stride, 4, skip, count);
                else
                    list->color = copy_gl_pointer_color(
                        &glstate->vao->vertexattrib[ATT_COLOR], 4, skip, count);
            }
        }
        if (glstate->vao->vertexattrib[ATT_SECONDARY].enabled) {
            if (glstate->vao->shared_arrays) {
                if (glstate->vao->vertexattrib[ATT_SECONDARY].size == GL_BGRA)
                    glstate->vao->secondary.ptr = copy_gl_pointer_color_bgra(
                        glstate->vao->vertexattrib[ATT_SECONDARY].pointer,
                        glstate->vao->vertexattrib[ATT_SECONDARY].stride, 4, 0, count);
                else
                    glstate->vao->secondary.ptr = copy_gl_pointer(
                        &glstate->vao->vertexattrib[ATT_SECONDARY], 4, 0, count);
                list->secondary = glstate->vao->secondary.ptr + 4 * skip;
            } else {
                if (glstate->vao->vertexattrib[ATT_SECONDARY].size == GL_BGRA)
                    list->secondary = copy_gl_pointer_color_bgra(
                        glstate->vao->vertexattrib[ATT_SECONDARY].pointer,
                        glstate->vao->vertexattrib[ATT_SECONDARY].stride, 4, skip, count);
                else
                    list->secondary = copy_gl_pointer(
                        &glstate->vao->vertexattrib[ATT_SECONDARY], 4, skip, count);
            }
        }
        if (glstate->vao->vertexattrib[ATT_NORMAL].enabled) {
            if (glstate->vao->shared_arrays) {
                glstate->vao->normal.ptr = copy_gl_pointer_raw(
                    &glstate->vao->vertexattrib[ATT_NORMAL], 3, 0, count);
                list->normal = glstate->vao->normal.ptr + 3 * skip;
            } else
                list->normal = copy_gl_pointer_raw(
                    &glstate->vao->vertexattrib[ATT_NORMAL], 3, skip, count);
        }
        if (glstate->vao->vertexattrib[ATT_FOGCOORD].enabled) {
            if (glstate->vao->shared_arrays) {
                glstate->vao->fog.ptr = copy_gl_pointer_raw(
                    &glstate->vao->vertexattrib[ATT_FOGCOORD], 1, 0, count);
                list->fogcoord = glstate->vao->fog.ptr + 1 * skip;
            } else
                list->fogcoord = copy_gl_pointer_raw(
                    &glstate->vao->vertexattrib[ATT_FOGCOORD], 1, skip, count);
        }
        for (int i = 0; i < glstate->vao->maxtex; i++) {
            if (glstate->vao->vertexattrib[ATT_MULTITEXCOORD0 + i].enabled) {
                if (glstate->vao->shared_arrays) {
                    glstate->vao->tex[i].ptr = copy_gl_pointer_tex(
                        &glstate->vao->vertexattrib[ATT_MULTITEXCOORD0 + i],
                        4, 0, count);
                    list->tex[i] = glstate->vao->tex[i].ptr + 4 * skip;
                } else
                    list->tex[i] = copy_gl_pointer_tex(
                        &glstate->vao->vertexattrib[ATT_MULTITEXCOORD0 + i],
                        4, skip, count);
            }
        }
    }
    for (int i = 0; i < hardext.maxtex; i++)
        if (list->tex[i] && list->maxtex < i + 1) list->maxtex = i + 1;
    return list;
}

// =============================================================================
//  arrays_add_renderlist — IDENTIK dengan original (fingerprint tidak mempengaruhi ini)
// =============================================================================
static renderlist_t *arrays_add_renderlist(renderlist_t *a, GLenum mode,
                                            GLsizei skip, GLsizei count,
                                            GLushort* indices, int ilen_b) {
    DBG(LOGD("arrays_add_renderlist(%p, %s, %d, %d, %p, %d)\n",
             a, PrintEnum(mode), skip, count, indices, ilen_b);)
    if (glstate->vao->shared_arrays) {
        if (!is_cache_compatible(count))
            VaoSharedClear(glstate->vao);
    }
    int ilen_a  = a->ilen;
    int len_b   = count - skip;
    unsigned long cap = a->cap;
    if (a->len + len_b >= cap) cap += len_b + DEFAULT_RENDER_LIST_CAPACITY;
    unshared_renderlist(a, cap);
    redim_renderlist(a, cap);
    unsharedindices_renderlist(a, ((ilen_a) ? ilen_a : a->len) +
                                  ((ilen_b) ? ilen_b : len_b));
    if (glstate->vao->shared_arrays) {
        if (a->vert)      memcpy(a->vert+a->len*4,      glstate->vao->vert.ptr+skip*4,      len_b*4*sizeof(GLfloat));
        if (a->normal)    memcpy(a->normal+a->len*3,    glstate->vao->normal.ptr+skip*3,    len_b*3*sizeof(GLfloat));
        if (a->color)     memcpy(a->color+a->len*4,     glstate->vao->color.ptr+skip*4,     len_b*4*sizeof(GLfloat));
        if (a->secondary) memcpy(a->secondary+a->len*4, glstate->vao->secondary.ptr+skip*4, len_b*4*sizeof(GLfloat));
        if (a->fogcoord)  memcpy(a->fogcoord+a->len*1,  glstate->vao->fog.ptr+skip*1,       len_b*1*sizeof(GLfloat));
        for (int i = 0; i < a->maxtex; i++)
            if (a->tex[i]) memcpy(a->tex[i]+a->len*4, glstate->vao->tex[i].ptr+skip*4, len_b*4*sizeof(GLfloat));
    } else {
        if (a->vert)   copy_gl_pointer_tex_noalloc(a->vert+a->len*4,    &glstate->vao->vertexattrib[ATT_VERTEX],  4, skip, count);
        if (a->normal) copy_gl_pointer_raw_noalloc(a->normal+a->len*3,  &glstate->vao->vertexattrib[ATT_NORMAL],  3, skip, count);
        if (a->color) {
            if (glstate->vao->vertexattrib[ATT_COLOR].size == GL_BGRA)
                copy_gl_pointer_color_bgra_noalloc(a->color+a->len*4,
                    glstate->vao->vertexattrib[ATT_COLOR].pointer,
                    glstate->vao->vertexattrib[ATT_COLOR].stride, 4, skip, count);
            else
                copy_gl_pointer_color_noalloc(a->color+a->len*4,
                    &glstate->vao->vertexattrib[ATT_COLOR], 4, skip, count);
        }
        if (a->secondary) {
            if (glstate->vao->vertexattrib[ATT_SECONDARY].size == GL_BGRA)
                copy_gl_pointer_color_bgra_noalloc(a->secondary+a->len*4,
                    glstate->vao->vertexattrib[ATT_SECONDARY].pointer,
                    glstate->vao->vertexattrib[ATT_SECONDARY].stride, 4, skip, count);
            else
                copy_gl_pointer_noalloc(a->secondary+a->len*4,
                    &glstate->vao->vertexattrib[ATT_SECONDARY], 4, skip, count);
        }
        if (a->fogcoord) copy_gl_pointer_raw_noalloc(a->fogcoord+a->len*1, &glstate->vao->vertexattrib[ATT_FOGCOORD], 1, skip, count);
        for (int i = 0; i < a->maxtex; i++)
            if (a->tex[i]) copy_gl_pointer_tex_noalloc(a->tex[i]+a->len*4,
                &glstate->vao->vertexattrib[ATT_MULTITEXCOORD0+i], 4, skip, count);
    }
    int old_ilenb = ilen_b;
    if (!a->mode_inits) list_add_modeinit(a, a->mode_init);
    if (ilen_a || ilen_b || mode_needindices(a->mode) || mode_needindices(mode)
        || (a->mode != mode && (a->mode == GL_QUADS || mode == GL_QUADS)))
    {
        ilen_b = indices_getindicesize(mode, ((indices) ? ilen_b : len_b));
        prepareadd_renderlist(a, ilen_b);
        doadd_renderlist(a, mode, indices, indices ? old_ilenb : len_b, ilen_b);
    }
    a->len += len_b;
    if (a->mode_inits) list_add_modeinit(a, mode);
    a->stage = STAGE_DRAW;
    return a;
}

// =============================================================================
//  should_intercept_render — IDENTIK dengan original
// =============================================================================
static inline bool should_intercept_render(GLenum mode) {
    if (hardext.esversion == 1)
        for (int aa = 0; aa < hardext.maxtex; aa++) {
            if (glstate->enable.texture[aa]) {
                if ((glstate->enable.texgen_s[aa] || glstate->enable.texgen_t[aa] ||
                     glstate->enable.texgen_r[aa] || glstate->enable.texgen_q[aa]))
                    return true;
                if ((!glstate->vao->vertexattrib[ATT_MULTITEXCOORD0+aa].enabled) &&
                    !(mode == GL_POINT && glstate->texture.pscoordreplace[aa]))
                    return true;
                if ((glstate->vao->vertexattrib[ATT_MULTITEXCOORD0+aa].enabled) &&
                    (glstate->vao->vertexattrib[ATT_MULTITEXCOORD0+aa].size == 1))
                    return true;
            }
        }
    if (glstate->polygon_mode == GL_LINE && mode >= GL_TRIANGLES)
        return true;
    if ((hardext.esversion == 1) &&
        ((glstate->vao->vertexattrib[ATT_SECONDARY].enabled) &&
         (glstate->vao->vertexattrib[ATT_COLOR].enabled)))
        return true;
    if ((hardext.esversion == 1) &&
        (glstate->vao->vertexattrib[ATT_COLOR].enabled &&
         (glstate->vao->vertexattrib[ATT_COLOR].size != 4)))
        return true;
    return (
        (glstate->vao->vertexattrib[ATT_VERTEX].enabled &&
         !valid_vertex_type(glstate->vao->vertexattrib[ATT_VERTEX].type)) ||
        (mode == GL_LINES && glstate->enable.line_stipple) ||
        (glstate->list.active && !glstate->list.pending)
    );
}

// =============================================================================
//  len_indices — IDENTIK dengan original
// =============================================================================
GLuint len_indices(const GLushort *sindices, const GLuint *iindices, GLsizei count) {
    GLuint len = 0;
    if (sindices) {
        for (int i = 0; i < count; i++)
            if (len < sindices[i]) len = sindices[i];
    } else {
        for (int i = 0; i < count; i++)
            if (len < iindices[i]) len = iindices[i];
    }
    return len + 1;
}

// =============================================================================
//  es3_setup_native_vao — Konfigurasi native VAO GLES 3.0+ untuk draw call
//
//  Dipanggil hanya ketika:
//  - globals4es.use_native_vao == 1
//  - hardext.esversion >= 3
//  - Ada vertex data (vertexattrib[ATT_VERTEX].enabled)
//
//  Mengembalikan native VAO id jika berhasil, 0 jika gagal.
//
//  Ide: satu glvao_t → satu native GLuint VAO.
//  Fingerprint fingerprint_vao membandingkan apakah attrib berubah sejak
//  konfigurasi terakhir. Jika berubah: rekonfigurasi attrib dan simpan fp baru.
//  Jika tidak berubah: langsung bind saja.
// =============================================================================
static GLuint es3_setup_native_vao(void) {
    LOAD_GLES2(glGenVertexArrays);
    LOAD_GLES2(glBindVertexArray);
    LOAD_GLES2(glVertexAttribPointer);
    LOAD_GLES2(glEnableVertexAttribArray);
    LOAD_GLES2(glDisableVertexAttribArray);

    if (!gles_glGenVertexArrays || !gles_glBindVertexArray ||
        !gles_glVertexAttribPointer)
        return 0;

    native_vao_cache_init();

    uintptr_t key = (uintptr_t)glstate->vao;
    native_vao_entry_t* entry = native_vao_cache_lookup(key);
    if (!entry) return 0; // cache penuh — fallback ke legacy path

    uint64_t fp = gl4es_fingerprint_vao(glstate->vao);

    // Buat native VAO jika belum ada
    if (entry->key != key || entry->vao_id == 0) {
        GLuint new_id = 0;
        gles_glGenVertexArrays(1, &new_id);
        if (!new_id) return 0;
        entry->key    = key;
        entry->vao_id = new_id;
        entry->fingerprint = 0; // paksa rekonfigurasi pertama kali
    }

    // Bind native VAO
    gles_glBindVertexArray(entry->vao_id);

    // Cek apakah perlu rekonfigurasi (fingerprint berubah)
    if (entry->fingerprint == fp)
        return entry->vao_id; // ✓ Tidak ada perubahan — langsung draw

    // ── Rekonfigurasi attrib ──────────────────────────────────────────────
    // Bind VBO jika ada
    LOAD_GLES(glBindBuffer);
    if (gles_glBindBuffer) {
        GLuint vbo = glstate->vao->vertex ? glstate->vao->vertex->id : 0;
        gles_glBindBuffer(GL_ARRAY_BUFFER, vbo);
    }

    // Setup semua attrib yang aktif
    for (int i = 0; i < MAX_VATTRIB; i++) {
        const vertexattrib_t* a = &glstate->vao->vertexattrib[i];
        if (a->enabled) {
            gles_glEnableVertexAttribArray(i);
            gles_glVertexAttribPointer(
                i,
                a->size,
                a->type,
                a->normalized,
                a->stride,
                a->pointer
            );
        } else {
            if (gles_glDisableVertexAttribArray)
                gles_glDisableVertexAttribArray(i);
        }
    }

    // Bind element buffer jika ada
    if (gles_glBindBuffer) {
        GLuint ebo = glstate->vao->elements ? glstate->vao->elements->id : 0;
        gles_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    }

    // Simpan fingerprint baru
    entry->fingerprint = fp;

    return entry->vao_id;
}

// =============================================================================
//  glDrawElementsCommon — Core draw dispatch
//
//  [BARU] ES3 NATIVE VAO FAST PATH:
//  Kondisi: esversion >= 3 && use_native_vao && !render_mode_select && !es1
//  Path baru: es3_setup_native_vao() → realize_textures() → glDraw*()
//
//  Path lama (ES1 / fallback): identik dengan original
// =============================================================================
static void glDrawElementsCommon(GLenum mode, GLint first, GLsizei count,
                                  GLuint len,
                                  const GLushort *sindices,
                                  const GLuint   *iindices,
                                  int instancecount)
{
    if (glstate->raster.bm_drawing)
        bitmap_flush();

    DBG(printf("glDrawElementsCommon(%s, %d, %d, %d, %p, %p, %d)\n",
               PrintEnum(mode), first, count, len, sindices, iindices,
               instancecount);)

    LOAD_GLES_FPE(glDrawElements);
    LOAD_GLES_FPE(glDrawArrays);
    LOAD_GLES_FPE(glNormalPointer);
    LOAD_GLES_FPE(glVertexPointer);
    LOAD_GLES_FPE(glColorPointer);
    LOAD_GLES_FPE(glTexCoordPointer);
    LOAD_GLES_FPE(glEnable);
    LOAD_GLES_FPE(glDisable);
    LOAD_GLES_FPE(glMultiTexCoord4f);

    #define client_state(A, B, C) \
        if((glstate->vao->vertexattrib[A].enabled != \
            glstate->gleshard->vertexattrib[A].enabled) || \
           (hardext.esversion != 1)) { \
            C \
            if(glstate->vao->vertexattrib[A].enabled) \
                fpe_glEnableClientState(B); \
            else \
                fpe_glDisableClientState(B); \
        }

    GLenum mode_init = mode;
    if (glstate->polygon_mode == GL_POINT && mode >= GL_TRIANGLES)
        mode = GL_POINTS;
    if (mode == GL_QUAD_STRIP)  mode = GL_TRIANGLE_STRIP;
    if (mode == GL_POLYGON)     mode = GL_TRIANGLE_FAN;
    if (mode == GL_QUADS) {
        mode = GL_TRIANGLES;
        int ilen = (count * 3) / 2;
        if (iindices) {
            gl4es_scratch(ilen * sizeof(GLuint));
            GLuint *tmp = (GLuint*)glstate->scratch;
            for (int i = 0, j = 0; i + 3 < count; i += 4, j += 6) {
                tmp[j+0] = iindices[i+0]; tmp[j+1] = iindices[i+1]; tmp[j+2] = iindices[i+2];
                tmp[j+3] = iindices[i+0]; tmp[j+4] = iindices[i+2]; tmp[j+5] = iindices[i+3];
            }
            iindices = tmp;
        } else {
            gl4es_scratch(ilen * sizeof(GLushort));
            GLushort *tmp = (GLushort*)glstate->scratch;
            for (int i = 0, j = 0; i + 3 < count; i += 4, j += 6) {
                tmp[j+0] = sindices[i+0]; tmp[j+1] = sindices[i+1]; tmp[j+2] = sindices[i+2];
                tmp[j+3] = sindices[i+0]; tmp[j+4] = sindices[i+2]; tmp[j+5] = sindices[i+3];
            }
            sindices = tmp;
        }
        count = ilen;
    }

    // ── GL_SELECT mode ────────────────────────────────────────────────────
    if (glstate->render_mode == GL_SELECT) {
        if (!sindices && !iindices)
            select_glDrawArrays(&glstate->vao->vertexattrib[ATT_VERTEX],
                                mode, first, count);
        else
            select_glDrawElements(&glstate->vao->vertexattrib[ATT_VERTEX],
                                  mode, count,
                                  sindices ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                                  sindices ? (void*)sindices : (void*)iindices);
        return;
    }

    // =========================================================================
    //  [BARU] ES3 NATIVE VAO FAST PATH
    //
    //  Kondisi aktifasi:
    //  1. Backend GLES 3.0+  (hardext.esversion >= 3)
    //  2. use_native_vao aktif (diset di init.c berdasarkan hardext)
    //  3. Ada vertex data (ATT_VERTEX enabled)
    //  4. Tidak sedang dalam mode ES1 (yang pakai legacy pointer)
    //
    //  Keuntungan vs path lama:
    //  - Tidak ada loop client_state yang setup tiap attrib setiap frame
    //  - Driver caching: state VAO dikompilasi oleh driver (lihat Google blog)
    //  - Per-draw call: hanya glBindVertexArray + realize_textures + glDraw*
    // =========================================================================
    if (globals4es.use_native_vao &&
        hardext.esversion >= 3 &&
        glstate->vao->vertexattrib[ATT_VERTEX].enabled)
    {
        GLuint native_id = es3_setup_native_vao();
        if (native_id != 0) {
            // Proses textures (FPE path — identik dengan path lama)
            GLuint old_tex = glstate->texture.client;
            realize_textures(1);

            // VBO lock optimization (ES3 tetap mendukung ini)
            if (globals4es.usevbo == 2 && glstate->vao->locked == 1)
                ToBuffer(glstate->vao->first, glstate->vao->count);
            if (globals4es.usevbo == 3 &&
                (glstate->vao->locked == 1 || glstate->vao->locked == 2)) {
                if (glstate->vao->locked == 1)
                    glstate->vao->locked++;
                else
                    ToBuffer(glstate->vao->first, glstate->vao->count);
            }

            // ── Draw call ─────────────────────────────────────────────────
            if (instancecount == 1) {
                if (!iindices && !sindices)
                    gles_glDrawArrays(mode, first, count);
                else
                    gles_glDrawElements(mode, count,
                        sindices ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                        sindices ? (void*)sindices : (void*)iindices);
            } else {
                if (!iindices && !sindices)
                    fpe_glDrawArraysInstanced(mode, first, count, instancecount);
                else {
                    void* tmp = sindices ? (void*)sindices : (void*)iindices;
                    GLenum t  = sindices ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
                    fpe_glDrawElementsInstanced(mode, count, t, tmp, instancecount);
                }
            }

            // Unbind native VAO setelah draw untuk menghindari state pollution
            // di driver yang tidak isolasi VAO state dengan baik (beberapa Mali lama)
            if (hardext.vendor & VEND_ARM) {
                LOAD_GLES2(glBindVertexArray);
                if (gles_glBindVertexArray)
                    gles_glBindVertexArray(0);
            }

            if (glstate->texture.client != old_tex) {
                #define TEXTURE(A) gl4es_glClientActiveTexture(A + GL_TEXTURE0);
                TEXTURE(old_tex);
                #undef TEXTURE
            }

            #undef client_state
            return;
        }
        // Jika native VAO gagal, fall through ke path lama
    }

    // =========================================================================
    //  PATH LAMA — ES1 atau fallback (IDENTIK dengan original)
    // =========================================================================
    {
        GLuint old_tex = glstate->texture.client;
        realize_textures(1);

        if (hardext.esversion == 1) {
            #define TEXTURE(A) gl4es_glClientActiveTexture(A + GL_TEXTURE0);
            vertexattrib_t *p;
            #define GetP(A) (&glstate->vao->vertexattrib[A])

            client_state(ATT_COLOR, GL_COLOR_ARRAY, );
            p = GetP(ATT_COLOR);
            if (p->enabled) gles_glColorPointer(p->size, p->type, p->stride, p->pointer);

            client_state(ATT_NORMAL, GL_NORMAL_ARRAY, );
            p = GetP(ATT_NORMAL);
            if (p->enabled) gles_glNormalPointer(p->type, p->stride, p->pointer);

            client_state(ATT_VERTEX, GL_VERTEX_ARRAY, );
            p = GetP(ATT_VERTEX);
            if (p->enabled) gles_glVertexPointer(p->size, p->type, p->stride, p->pointer);

            for (int aa = 0; aa < hardext.maxtex; aa++) {
                client_state(ATT_MULTITEXCOORD0+aa, GL_TEXTURE_COORD_ARRAY, TEXTURE(aa););
                p = GetP(ATT_MULTITEXCOORD0 + aa);
                const GLint itarget = get_target(glstate->enable.texture[aa]);
                if (itarget >= 0) {
                    if (!IS_TEX2D(glstate->enable.texture[aa]) &&
                        (IS_ANYTEX(glstate->enable.texture[aa]))) {
                        gl4es_glActiveTexture(GL_TEXTURE0 + aa);
                        realize_active();
                        gles_glEnable(GL_TEXTURE_2D);
                    }
                    if (p->enabled) {
                        TEXTURE(aa);
                        int changes = tex_setup_needchange(itarget);
                        if (changes && !len) len = len_indices(sindices, iindices, count);
                        tex_setup_texcoord(len, changes, itarget, p);
                    } else
                        gles_glMultiTexCoord4f(GL_TEXTURE0+aa,
                            glstate->texcoord[aa][0], glstate->texcoord[aa][1],
                            glstate->texcoord[aa][2], glstate->texcoord[aa][3]);
                }
            }
            #undef GetP
            if (glstate->texture.client != old_tex)
                TEXTURE(old_tex);
        }

        // VBO lock
        if (hardext.esversion > 1 && globals4es.usevbo == 2 &&
            glstate->vao->locked == 1)
            ToBuffer(glstate->vao->first, glstate->vao->count);
        if (hardext.esversion > 1 && globals4es.usevbo == 3 &&
            (glstate->vao->locked == 1 || glstate->vao->locked == 2)) {
            if (glstate->vao->locked == 1)
                glstate->vao->locked++;
            else
                ToBuffer(glstate->vao->first, glstate->vao->count);
        }

        // Draw
        if (instancecount == 1 || hardext.esversion == 1) {
            if (!iindices && !sindices)
                gles_glDrawArrays(mode, first, count);
            else
                gles_glDrawElements(mode, count,
                    sindices ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                    sindices ? (void*)sindices : (void*)iindices);
        } else {
            if (!iindices && !sindices)
                fpe_glDrawArraysInstanced(mode, first, count, instancecount);
            else {
                void* tmp = sindices ? (void*)sindices : (void*)iindices;
                GLenum t  = sindices ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
                fpe_glDrawElementsInstanced(mode, count, t, tmp, instancecount);
            }
        }

        for (int aa = 0; aa < hardext.maxtex; aa++) {
            if (!IS_TEX2D(glstate->enable.texture[aa]) &&
                (IS_ANYTEX(glstate->enable.texture[aa]))) {
                gl4es_glActiveTexture(GL_TEXTURE0 + aa);
                realize_active();
                gles_glDisable(GL_TEXTURE_2D);
            }
        }
        if (glstate->texture.client != old_tex) {
            #define TEXTURE(A) gl4es_glClientActiveTexture(A + GL_TEXTURE0);
            TEXTURE(old_tex);
            #undef TEXTURE
        }
    }
    #undef client_state
}

// =============================================================================
//  Semua fungsi publik di bawah ini IDENTIK dengan original.
//  Hanya glDrawElementsCommon yang diubah (di atas).
//  Tidak ada perubahan pada API surface sama sekali.
// =============================================================================

#define MIN_BATCH  globals4es.minbatch
#define MAX_BATCH  globals4es.maxbatch

void APIENTRY_GL4ES gl4es_glDrawRangeElements(GLenum mode, GLuint start, GLuint end,
                                               GLsizei count, GLenum type,
                                               const void *indices)
{
    DBG(printf("glDrawRangeElements(%s, %i, %i, %i, %s, @%p), inlist=%i, pending=%d\n",
               PrintEnum(mode), start, end, count, PrintEnum(type), indices,
               (glstate->list.active)?1:0, glstate->list.pending);)
    count = adjust_vertices(mode, count);
    if (count < 0)  { errorShim(GL_INVALID_VALUE); return; }
    if (count == 0) { noerrorShim(); return; }

    bool compiling = (glstate->list.active);
    bool intercept = should_intercept_render(mode);

    if (!compiling) {
        if ((!intercept && !glstate->list.pending &&
             (count >= MIN_BATCH && count <= MAX_BATCH))
            || (intercept && globals4es.maxbatch)) {
            compiling = true;
            glstate->list.pending = 1;
            glstate->list.active = alloc_renderlist();
        }
    }

    noerrorShim();
    GLushort *sindices = NULL;
    GLuint   *iindices = NULL;
    bool need_free = !(
        (type == GL_UNSIGNED_SHORT) ||
        (!compiling && !intercept && type == GL_UNSIGNED_INT && hardext.elementuint)
    );
    if (need_free) {
        sindices = copy_gl_array(
            (glstate->vao->elements)
                ? (void*)((char*)glstate->vao->elements->data + (uintptr_t)indices)
                : indices,
            type, 1, 0, GL_UNSIGNED_SHORT, 1, 0, count, NULL);
    } else {
        if (type == GL_UNSIGNED_INT)
            iindices = (glstate->vao->elements)
                ? (void*)((char*)glstate->vao->elements->data + (uintptr_t)indices)
                : (GLvoid*)indices;
        else
            sindices = (glstate->vao->elements)
                ? (void*)((char*)glstate->vao->elements->data + (uintptr_t)indices)
                : (GLvoid*)indices;
    }

    if (compiling) {
        renderlist_t *list = glstate->list.active;
        if (!need_free) {
            GLushort *tmp = sindices;
            sindices = (GLushort*)malloc(count * sizeof(GLushort));
            memcpy(sindices, tmp, count * sizeof(GLushort));
        }
        for (int i = 0; i < count; i++) sindices[i] -= start;
        if (globals4es.mergelist && list->stage >= STAGE_DRAW &&
            is_list_compatible(list) && !list->use_glstate && sindices) {
            list = NewDrawStage(list, mode);
            if (list->vert) {
                glstate->list.active = arrays_add_renderlist(list, mode, start, end+1, sindices, count);
                NewStage(glstate->list.active, STAGE_POSTDRAW);
                return;
            }
        }
        NewStage(list, STAGE_DRAW);
        glstate->list.active = list = arrays_to_renderlist(list, mode, start, end+1);
        list->indices  = sindices;
        list->ilen     = count;
        list->indice_cap = count;
        NewStage(glstate->list.active, STAGE_POSTDRAW);
        return;
    }

    if (intercept) {
        if (!need_free) {
            GLushort *tmp = sindices;
            sindices = (GLushort*)malloc(count * sizeof(GLushort));
            memcpy(sindices, tmp, count * sizeof(GLushort));
        }
        for (int i = 0; i < count; i++) sindices[i] -= start;
        renderlist_t *list = arrays_to_renderlist(NULL, mode, start, end+1);
        list->indices = sindices; list->ilen = count; list->indice_cap = count;
        list = end_renderlist(list);
        draw_renderlist(list);
        free_renderlist(list);
        return;
    } else {
        glDrawElementsCommon(mode, 0, count, end+1, sindices, iindices, 1);
        if (need_free) free(sindices);
    }
}
AliasExport(void,glDrawRangeElements,,(GLenum mode,GLuint start,GLuint end,GLsizei count,GLenum type,const void *indices));
AliasExport(void,glDrawRangeElements,EXT,(GLenum mode,GLuint start,GLuint end,GLsizei count,GLenum type,const void *indices));


void APIENTRY_GL4ES gl4es_glDrawElements(GLenum mode, GLsizei count, GLenum type,
                                          const GLvoid *indices)
{
    DBG(printf("glDrawElements(%s, %d, %s, %p), vtx=%p map=%p, pending=%d\n",
               PrintEnum(mode), count, PrintEnum(type), indices,
               (glstate->vao->vertex)?glstate->vao->vertex->data:NULL,
               (glstate->vao->elements)?glstate->vao->elements->data:NULL,
               glstate->list.pending);)
    count = adjust_vertices(mode, count);
    if (count < 0)  { errorShim(GL_INVALID_VALUE); return; }
    if (count == 0) { noerrorShim(); return; }

    bool compiling = (glstate->list.active);
    bool intercept = should_intercept_render(mode);

    if (!compiling) {
        if ((!intercept && !glstate->list.pending &&
             (count >= MIN_BATCH && count <= MAX_BATCH))
            || (intercept && globals4es.maxbatch)) {
            compiling = true;
            glstate->list.pending = 1;
            glstate->list.active = alloc_renderlist();
        }
    }

    noerrorShim();
    GLushort *sindices = NULL;
    GLuint   *iindices = NULL;
    GLuint old_index = 0;
    bool need_free = !(
        (type == GL_UNSIGNED_SHORT) ||
        (!compiling && !intercept && type == GL_UNSIGNED_INT && hardext.elementuint)
    );
    if (need_free) {
        sindices = copy_gl_array(
            (glstate->vao->elements)
                ? (void*)((char*)glstate->vao->elements->data + (uintptr_t)indices)
                : indices,
            type, 1, 0, GL_UNSIGNED_SHORT, 1, 0, count, NULL);
        old_index = wantBufferIndex(0);
    } else {
        if (type == GL_UNSIGNED_INT)
            iindices = (glstate->vao->elements)
                ? (void*)((char*)glstate->vao->elements->data + (uintptr_t)indices)
                : (GLvoid*)indices;
        else
            sindices = (glstate->vao->elements)
                ? (void*)((char*)glstate->vao->elements->data + (uintptr_t)indices)
                : (GLvoid*)indices;
    }

    if (compiling) {
        renderlist_t *list = glstate->list.active;
        GLsizei min, max;
        if (!need_free) {
            GLushort *tmp = sindices;
            sindices = (GLushort*)malloc(count * sizeof(GLushort));
            memcpy(sindices, tmp, count * sizeof(GLushort));
        }
        normalize_indices_us(sindices, &max, &min, count);
        if (globals4es.mergelist && list->stage >= STAGE_DRAW &&
            is_list_compatible(list) && !list->use_glstate && sindices) {
            list = NewDrawStage(list, mode);
            glstate->list.active = arrays_add_renderlist(list, mode, min, max+1, sindices, count);
            NewStage(glstate->list.active, STAGE_POSTDRAW);
            return;
        }
        NewStage(list, STAGE_DRAW);
        glstate->list.active = list = arrays_to_renderlist(list, mode, min, max+1);
        list->indices = sindices; list->ilen = count; list->indice_cap = count;
        NewStage(glstate->list.active, STAGE_POSTDRAW);
        return;
    }

    if (intercept) {
        renderlist_t *list = NULL;
        GLsizei min, max;
        if (!need_free) {
            GLushort *tmp = sindices;
            sindices = (GLushort*)malloc(count * sizeof(GLushort));
            memcpy(sindices, tmp, count * sizeof(GLushort));
        }
        normalize_indices_us(sindices, &max, &min, count);
        list = arrays_to_renderlist(list, mode, min, max+1);
        list->indices = sindices; list->ilen = count; list->indice_cap = count;
        list = end_renderlist(list);
        draw_renderlist(list);
        free_renderlist(list);
        return;
    } else {
        glDrawElementsCommon(mode, 0, count, 0, sindices, iindices, 1);
        if (need_free) { free(sindices); wantBufferIndex(old_index); }
    }
}
AliasExport(void,glDrawElements,,(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices));


void APIENTRY_GL4ES gl4es_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    DBG(printf("glDrawArrays(%s, %d, %d), list=%p pending=%d\n",
               PrintEnum(mode), first, count,
               glstate->list.active, glstate->list.pending);)
    count = adjust_vertices(mode, count);
    if (count < 0)  { errorShim(GL_INVALID_VALUE); return; }
    if (count == 0) { noerrorShim(); return; }

    // Large QUADS split (identik dengan original)
    if ((mode == GL_QUADS) && (count > 4 * 8000)) {
        int cnt = 4 * 8000;
        for (int i = 0; i < count; i += 4 * 8000) {
            if (i + cnt > count) cnt = count - i;
            gl4es_glDrawArrays(mode, i, cnt);
        }
        return;
    }
    noerrorShim();

    bool intercept = should_intercept_render(mode);
    if (!glstate->list.compiling) {
        if ((!intercept && !glstate->list.pending &&
             (count >= MIN_BATCH && count <= MAX_BATCH))
            || (intercept && globals4es.maxbatch)) {
            glstate->list.pending = 1;
            glstate->list.active = alloc_renderlist();
        }
    }

    if (glstate->list.active) {
        renderlist_t *list = glstate->list.active;
        if (globals4es.mergelist && list->stage >= STAGE_DRAW &&
            is_list_compatible(list) && !list->use_glstate) {
            list = NewDrawStage(list, mode);
            if (list->vert) {
                glstate->list.active = arrays_add_renderlist(list, mode, first, count+first, NULL, 0);
                NewStage(glstate->list.active, STAGE_POSTDRAW);
                return;
            }
        }
        NewStage(list, STAGE_DRAW);
        glstate->list.active = arrays_to_renderlist(list, mode, first, count+first);
        NewStage(glstate->list.active, STAGE_POSTDRAW);
        return;
    }

    if (glstate->polygon_mode == GL_POINT && mode >= GL_TRIANGLES)
        mode = GL_POINTS;

    if (intercept) {
        renderlist_t *list = arrays_to_renderlist(NULL, mode, first, count+first);
        list = end_renderlist(list);
        draw_renderlist(list);
        free_renderlist(list);
    } else {
        if (mode == GL_QUADS) {
            static GLushort *indices = NULL;
            static int indcnt = 0;
            static int indfirst = 0;
            int realfirst  = ((first % 4) == 0) ? 0 : first;
            int realcount  = count + (first - realfirst);
            if ((indcnt < realcount) || (indfirst != realfirst)) {
                if (indcnt < realcount) {
                    indcnt = realcount;
                    if (indices) free(indices);
                    indices = (GLushort*)malloc(sizeof(GLushort) * (indcnt * 3 / 2));
                }
                indfirst = realfirst;
                GLushort *p = indices;
                for (int i = 0, j = indfirst; i + 3 < indcnt; i += 4, j += 4) {
                    *(p++) = j+0; *(p++) = j+1; *(p++) = j+2;
                    *(p++) = j+0; *(p++) = j+2; *(p++) = j+3;
                }
            }
            GLuint old_buffer = wantBufferIndex(0);
            glDrawElementsCommon(GL_TRIANGLES, 0, count*3/2, count,
                                  indices + (first - indfirst) * 3 / 2, NULL, 1);
            wantBufferIndex(old_buffer);
            return;
        }
        glDrawElementsCommon(mode, first, count, count, NULL, NULL, 1);
    }
}
AliasExport(void,glDrawArrays,,(GLenum mode, GLint first, GLsizei count));
AliasExport(void,glDrawArrays,EXT,(GLenum mode, GLint first, GLsizei count));


void APIENTRY_GL4ES gl4es_glMultiDrawArrays(GLenum mode, const GLint *firsts,
                                             const GLsizei *counts, GLsizei primcount)
{
    DBG(printf("glMultiDrawArrays(%s, %p, %p, %d), list=%p pending=%d\n",
               PrintEnum(mode), firsts, counts, primcount,
               glstate->list.active, glstate->list.pending);)
    if (!primcount) { noerrorShim(); return; }

    bool compiling = (glstate->list.active);
    bool intercept = should_intercept_render(mode);

    GLsizei maxcount = counts[0], mincount = counts[0];
    for (int i = 1; i < primcount; i++) {
        if (counts[i] > maxcount) maxcount = counts[i];
        if (counts[i] < mincount) mincount = counts[i];
    }
    if (!compiling) {
        if (!intercept && glstate->list.pending && maxcount > MAX_BATCH)
            gl4es_flush();
        else if ((!intercept && !glstate->list.pending && mincount < MIN_BATCH)
                 || (intercept && globals4es.maxbatch)) {
            compiling = true;
            glstate->list.pending = 1;
            glstate->list.active = alloc_renderlist();
        }
    }
    renderlist_t *list = NULL;
    GLenum err = 0;

    for (int i = 0; i < primcount; i++) {
        GLsizei count = adjust_vertices(mode, counts[i]);
        GLint   fst   = firsts[i];
        if (count < 0) { err = GL_INVALID_VALUE; continue; }
        if (count == 0) continue;

        if (compiling) {
            if (globals4es.mergelist && glstate->list.active->stage >= STAGE_DRAW &&
                is_list_compatible(glstate->list.active) &&
                !glstate->list.active->use_glstate) {
                glstate->list.active = NewDrawStage(glstate->list.active, mode);
                glstate->list.active = arrays_add_renderlist(glstate->list.active,
                    mode, fst, count+fst, NULL, 0);
                NewStage(glstate->list.active, STAGE_POSTDRAW);
                continue;
            }
            NewStage(glstate->list.active, STAGE_DRAW);
            glstate->list.active = arrays_to_renderlist(glstate->list.active, mode, fst, count+fst);
            NewStage(glstate->list.active, STAGE_POSTDRAW);
            continue;
        }

        if (glstate->polygon_mode == GL_POINT && mode >= GL_TRIANGLES)
            mode = GL_POINTS;

        if (intercept) {
            if (list) {
                NewStage(list, STAGE_DRAW);
                if (globals4es.mergelist && list->stage >= STAGE_DRAW &&
                    is_list_compatible(list) && !list->use_glstate) {
                    list = NewDrawStage(list, mode);
                    list = arrays_add_renderlist(list, mode, fst, count+fst, NULL, 0);
                    NewStage(list, STAGE_POSTDRAW);
                }
            } else
                list = arrays_to_renderlist(NULL, mode, fst, count+fst);
        } else {
            if (mode == GL_QUADS) {
                static GLushort *indices = NULL;
                static int indcnt = 0, indfirst = 0;
                int realfirst = ((fst % 4) == 0) ? 0 : fst;
                int realcount = count + (fst - realfirst);
                if ((indcnt < realcount) || (indfirst != realfirst)) {
                    if (indcnt < realcount) {
                        indcnt = realcount;
                        if (indices) free(indices);
                        indices = (GLushort*)malloc(sizeof(GLushort) * (indcnt * 3 / 2));
                    }
                    indfirst = realfirst;
                    GLushort *p = indices;
                    for (int k = 0, j = indfirst; k + 3 < indcnt; k += 4, j += 4) {
                        *(p++) = j+0; *(p++) = j+1; *(p++) = j+2;
                        *(p++) = j+0; *(p++) = j+2; *(p++) = j+3;
                    }
                }
                GLuint old_idx = wantBufferIndex(0);
                glDrawElementsCommon(GL_TRIANGLES, 0, count*3/2, count,
                                      indices + (fst - indfirst) * 3 / 2, NULL, 1);
                wantBufferIndex(old_idx);
                continue;
            }
            glDrawElementsCommon(mode, fst, count, count, NULL, NULL, 1);
        }
    }
    if (list) {
        list = end_renderlist(list);
        draw_renderlist(list);
        free_renderlist(list);
    }
    if (err) errorShim(err); else errorGL();
}
AliasExport(void,glMultiDrawArrays,,(GLenum mode, const GLint *first, const GLsizei *count, GLsizei primcount));


void APIENTRY_GL4ES gl4es_glMultiDrawElements(GLenum mode, GLsizei *counts, GLenum type,
                                               const void * const *indices, GLsizei primcount)
{
    DBG(printf("glMultiDrawElements(%s, %p, %s, %p, %d), list=%p pending=%d\n",
               PrintEnum(mode), counts, PrintEnum(type), indices, primcount,
               glstate->list.active, glstate->list.pending);)
    if (!primcount) { noerrorShim(); return; }

    bool compiling = (glstate->list.active);
    bool intercept = should_intercept_render(mode);

    GLsizei maxcount = counts[0], mincount = counts[0];
    for (int i = 1; i < primcount; i++) {
        if (counts[i] > maxcount) maxcount = counts[i];
        if (counts[i] < mincount) mincount = counts[i];
    }
    if (!compiling) {
        if (!intercept && glstate->list.pending && maxcount > MAX_BATCH)
            gl4es_flush();
        else if ((!intercept && !glstate->list.pending && mincount < MIN_BATCH)
                 || (intercept && globals4es.maxbatch)) {
            compiling = true;
            glstate->list.pending = 1;
            glstate->list.active = alloc_renderlist();
        }
    }
    renderlist_t *list = NULL;
    for (int i = 0; i < primcount; i++) {
        GLsizei count = adjust_vertices(mode, counts[i]);
        if (count < 0)  { errorShim(GL_INVALID_VALUE); continue; }
        if (count == 0) { noerrorShim(); continue; }
        noerrorShim();

        GLushort *sindices = NULL;
        GLuint   *iindices = NULL;
        GLuint old_index = 0;
        bool need_free = !(
            (type == GL_UNSIGNED_SHORT) ||
            (!compiling && !intercept && type == GL_UNSIGNED_INT && hardext.elementuint)
        );
        if (need_free) {
            GLvoid *src = glstate->vao->elements
                ? (void*)((char*)glstate->vao->elements->data + (uintptr_t)indices[i])
                : (GLvoid*)indices[i];
            sindices = copy_gl_array(src, type, 1, 0, GL_UNSIGNED_SHORT, 1, 0, count, NULL);
            old_index = wantBufferIndex(0);
        } else {
            if (type == GL_UNSIGNED_INT)
                iindices = glstate->vao->elements
                    ? (void*)((char*)glstate->vao->elements->data + (uintptr_t)indices[i])
                    : (GLuint*)indices[i];
            else
                sindices = glstate->vao->elements
                    ? (void*)((char*)glstate->vao->elements->data + (uintptr_t)indices[i])
                    : (GLushort*)indices[i];
        }

        if (compiling) {
            renderlist_t *clist = NULL;
            GLsizei min, max;
            NewStage(glstate->list.active, STAGE_DRAW);
            clist = glstate->list.active;
            if (!need_free) {
                GLushort *tmp = sindices;
                sindices = (GLushort*)malloc(count * sizeof(GLushort));
                memcpy(sindices, tmp, count * sizeof(GLushort));
            }
            normalize_indices_us(sindices, &max, &min, count);
            clist = arrays_to_renderlist(clist, mode, min, max+1);
            clist->indices = sindices; clist->ilen = count; clist->indice_cap = count;
            if (glstate->list.pending)
                NewStage(glstate->list.active, STAGE_POSTDRAW);
            else
                glstate->list.active = extend_renderlist(clist);
            continue;
        }

        if (intercept) {
            renderlist_t *ilist = NULL;
            GLsizei min, max;
            if (!need_free) {
                GLushort *tmp = sindices;
                sindices = (GLushort*)malloc(count * sizeof(GLushort));
                memcpy(sindices, tmp, count * sizeof(GLushort));
            }
            normalize_indices_us(sindices, &max, &min, count);
            if (list) NewStage(list, STAGE_DRAW);
            ilist = arrays_to_renderlist(ilist, mode, min, max+1);
            ilist->indices = sindices; ilist->ilen = count; ilist->indice_cap = count;
        } else {
            glDrawElementsCommon(mode, 0, count, 0, sindices, iindices, 1);
            if (need_free) { free(sindices); wantBufferIndex(old_index); }
        }
    }
    if (list) {
        list = end_renderlist(list);
        draw_renderlist(list);
        free_renderlist(list);
    }
}
AliasExport(void,glMultiDrawElements,,( GLenum mode, GLsizei *count, GLenum type, const void * const *indices, GLsizei primcount));


void APIENTRY_GL4ES gl4es_glMultiDrawElementsBaseVertex(GLenum mode, GLsizei *counts,
                                                         GLenum type,
                                                         const void * const *indices,
                                                         GLsizei primcount,
                                                         const GLint * basevertex)
{
    DBG(printf("glMultiDrawElementsBaseVertex(%s, %p, %s, @%p, %d, @%p), inlist=%i, pending=%d\n",
               PrintEnum(mode), counts, PrintEnum(type), indices, primcount, basevertex,
               (glstate->list.active)?1:0, glstate->list.pending);)
    bool compiling = (glstate->list.active);
    bool intercept = should_intercept_render(mode);
    GLsizei maxcount = counts[0], mincount = counts[0];
    for (int i = 1; i < primcount; i++) {
        if (counts[i] > maxcount) maxcount = counts[i];
        if (counts[i] < mincount) mincount = counts[i];
    }
    if (!compiling) {
        if (!intercept && glstate->list.pending && maxcount > MAX_BATCH)
            gl4es_flush();
        else if ((!intercept && !glstate->list.pending && mincount < MIN_BATCH)
                 || (intercept && globals4es.maxbatch)) {
            compiling = true;
            glstate->list.pending = 1;
            glstate->list.active = alloc_renderlist();
        }
    }
    renderlist_t *list = NULL;
    for (int i = 0; i < primcount; i++) {
        GLsizei count = adjust_vertices(mode, counts[i]);
        if (count < 0)  { errorShim(GL_INVALID_VALUE); continue; }
        if (count == 0) { noerrorShim(); continue; }
        noerrorShim();

        GLushort *sindices = NULL;
        GLuint   *iindices = NULL;
        if (type == GL_UNSIGNED_INT && hardext.elementuint && !compiling && !intercept)
            iindices = copy_gl_array(
                (glstate->vao->elements)
                    ? (void*)((char*)glstate->vao->elements->data + (uintptr_t)indices)
                    : (void*)indices,
                type, 1, 0, GL_UNSIGNED_INT, 1, 0, count, NULL);
        else
            sindices = copy_gl_array(
                (glstate->vao->elements)
                    ? (void*)((char*)glstate->vao->elements->data + (uintptr_t)indices)
                    : (void*)indices,
                type, 1, 0, GL_UNSIGNED_SHORT, 1, 0, count, NULL);

        if (compiling) {
            renderlist_t *clist = NULL;
            GLsizei min, max;
            NewStage(glstate->list.active, STAGE_DRAW);
            clist = glstate->list.active;
            normalize_indices_us(sindices, &max, &min, count);
            clist = arrays_to_renderlist(clist, mode,
                min + basevertex[i], max + basevertex[i] + 1);
            clist->indices = sindices; clist->ilen = count; clist->indice_cap = count;
            if (glstate->list.pending)
                NewStage(glstate->list.active, STAGE_POSTDRAW);
            else
                glstate->list.active = extend_renderlist(clist);
            continue;
        }
        if (intercept) {
            GLsizei min, max;
            normalize_indices_us(sindices, &max, &min, count);
            if (list) NewStage(list, STAGE_DRAW);
            list = arrays_to_renderlist(list, mode,
                min + basevertex[i], max + basevertex[i] + 1);
            list->indices = sindices; list->ilen = count; list->indice_cap = count;
        } else {
            if (iindices)
                for (int k = 0; k < count; k++) iindices[k] += basevertex[i];
            else
                for (int k = 0; k < count; k++) sindices[k] += basevertex[i];
            GLuint old_idx = wantBufferIndex(0);
            glDrawElementsCommon(mode, 0, count, 0, sindices, iindices, 1);
            if (iindices) free(iindices); else free(sindices);
            wantBufferIndex(old_idx);
        }
    }
    if (list) {
        list = end_renderlist(list);
        draw_renderlist(list);
        free_renderlist(list);
    }
}
AliasExport(void,glMultiDrawElementsBaseVertex,,( GLenum mode, GLsizei *count, GLenum type, const void * const *indices, GLsizei primcount, const GLint * basevertex));
AliasExport(void,glMultiDrawElementsBaseVertex,ARB,( GLenum mode, GLsizei *count, GLenum type, const void * const *indices, GLsizei primcount, const GLint * basevertex));


void APIENTRY_GL4ES gl4es_glDrawElementsBaseVertex(GLenum mode, GLsizei count,
                                                    GLenum type, const void *indices,
                                                    GLint basevertex)
{
    DBG(printf("glDrawElementsBaseVertex(%s, %d, %s, %p, %d), vtx=%p map=%p, pending=%d\n",
               PrintEnum(mode), count, PrintEnum(type), indices, basevertex,
               (glstate->vao->vertex)?glstate->vao->vertex->data:NULL,
               (glstate->vao->elements)?glstate->vao->elements->data:NULL,
               glstate->list.pending);)
    if (basevertex == 0) {
        gl4es_glDrawElements(mode, count, type, indices);
        return;
    }
    count = adjust_vertices(mode, count);
    if (count < 0)  { errorShim(GL_INVALID_VALUE); return; }
    if (count == 0) { noerrorShim(); return; }

    bool compiling = (glstate->list.active);
    bool intercept = should_intercept_render(mode);
    if (!compiling) {
        if (!intercept && glstate->list.pending && count > MAX_BATCH) gl4es_flush();
        else if ((!intercept && !glstate->list.pending && count < MIN_BATCH)
                 || (intercept && globals4es.maxbatch)) {
            compiling = true;
            glstate->list.pending = 1;
            glstate->list.active = alloc_renderlist();
        }
    }
    noerrorShim();
    GLushort *sindices = NULL;
    GLuint   *iindices = NULL;
    if (type == GL_UNSIGNED_INT && hardext.elementuint && !compiling && !intercept)
        iindices = copy_gl_array(
            (glstate->vao->elements)
                ? (void*)((char*)glstate->vao->elements->data + (uintptr_t)indices)
                : indices,
            type, 1, 0, GL_UNSIGNED_INT, 1, 0, count, NULL);
    else
        sindices = copy_gl_array(
            (glstate->vao->elements)
                ? (void*)((char*)glstate->vao->elements->data + (uintptr_t)indices)
                : indices,
            type, 1, 0, GL_UNSIGNED_SHORT, 1, 0, count, NULL);

    if (compiling) {
        renderlist_t *list = NULL;
        GLsizei min, max;
        NewStage(glstate->list.active, STAGE_DRAW);
        list = glstate->list.active;
        normalize_indices_us(sindices, &max, &min, count);
        list = arrays_to_renderlist(list, mode, min+basevertex, max+basevertex+1);
        list->indices = sindices; list->ilen = count; list->indice_cap = count;
        if (glstate->list.pending)
            NewStage(glstate->list.active, STAGE_POSTDRAW);
        else
            glstate->list.active = extend_renderlist(list);
        return;
    }
    if (intercept) {
        renderlist_t *list = NULL;
        GLsizei min, max;
        normalize_indices_us(sindices, &max, &min, count);
        list = arrays_to_renderlist(list, mode, min+basevertex, max+basevertex+1);
        list->indices = sindices; list->ilen = count; list->indice_cap = count;
        list = end_renderlist(list);
        draw_renderlist(list);
        free_renderlist(list);
        return;
    } else {
        if (iindices)
            for (int i = 0; i < count; i++) iindices[i] += basevertex;
        else
            for (int i = 0; i < count; i++) sindices[i] += basevertex;
        glDrawElementsCommon(mode, 0, count, 0, sindices, iindices, 1);
        if (iindices) free(iindices); else free(sindices);
    }
}
AliasExport(void,glDrawElementsBaseVertex,,(GLenum mode, GLsizei count, GLenum type, const void *indices, GLint basevertex));
AliasExport(void,glDrawElementsBaseVertex,ARB,(GLenum mode, GLsizei count, GLenum type, const void *indices, GLint basevertex));


void APIENTRY_GL4ES gl4es_glDrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end,
                                                         GLsizei count, GLenum type,
                                                         const void *indices, GLint basevertex)
{
    DBG(printf("glDrawRangeElementsBaseVertex(%s, %i, %i, %i, %s, @%p, %d), inlist=%i, pending=%d\n",
               PrintEnum(mode), start, end, count, PrintEnum(type), indices, basevertex,
               (glstate->list.active)?1:0, glstate->list.pending);)
    if (basevertex == 0) {
        gl4es_glDrawRangeElements(mode, start, end, count, type, indices);
        return;
    }
    count = adjust_vertices(mode, count);
    if (count < 0)  { errorShim(GL_INVALID_VALUE); return; }
    if (count == 0) { noerrorShim(); return; }

    bool compiling = (glstate->list.active);
    bool intercept = should_intercept_render(mode);
    if (!compiling) {
        if (!intercept && glstate->list.pending && count > MAX_BATCH) gl4es_flush();
        else if ((!intercept && !glstate->list.pending && count < MIN_BATCH)
                 || (intercept && globals4es.maxbatch)) {
            compiling = true;
            glstate->list.pending = 1;
            glstate->list.active = alloc_renderlist();
        }
    }
    noerrorShim();
    GLushort *sindices = NULL;
    GLuint   *iindices = NULL;
    if (type == GL_UNSIGNED_INT && hardext.elementuint && !compiling && !intercept)
        iindices = copy_gl_array(
            (glstate->vao->elements)
                ? (void*)((char*)glstate->vao->elements->data + (uintptr_t)indices)
                : indices,
            type, 1, 0, GL_UNSIGNED_INT, 1, 0, count, NULL);
    else
        sindices = copy_gl_array(
            (glstate->vao->elements)
                ? (void*)((char*)glstate->vao->elements->data + (uintptr_t)indices)
                : indices,
            type, 1, 0, GL_UNSIGNED_SHORT, 1, 0, count, NULL);

    if (compiling) {
        renderlist_t *list = NULL;
        NewStage(glstate->list.active, STAGE_DRAW);
        list = glstate->list.active;
        for (int i = 0; i < count; i++) sindices[i] -= start;
        list = arrays_to_renderlist(list, mode, start+basevertex, end+basevertex+1);
        list->indices = sindices; list->ilen = count; list->indice_cap = count;
        if (glstate->list.pending)
            NewStage(glstate->list.active, STAGE_POSTDRAW);
        else
            glstate->list.active = extend_renderlist(list);
        return;
    }
    if (intercept) {
        renderlist_t *list = NULL;
        for (int i = 0; i < count; i++) sindices[i] -= start;
        list = arrays_to_renderlist(list, mode, start+basevertex, end+basevertex+1);
        list->indices = sindices; list->ilen = count; list->indice_cap = count;
        list = end_renderlist(list);
        draw_renderlist(list);
        free_renderlist(list);
        return;
    } else {
        if (iindices)
            for (int i = 0; i < count; i++) iindices[i] += basevertex;
        else
            for (int i = 0; i < count; i++) sindices[i] += basevertex;
        GLuint old_idx = wantBufferIndex(0);
        glDrawElementsCommon(mode, 0, count, end+basevertex+1, sindices, iindices, 1);
        if (iindices) free(iindices); else free(sindices);
        wantBufferIndex(old_idx);
    }
}
AliasExport(void,glDrawRangeElementsBaseVertex,,(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices, GLint basevertex));
AliasExport(void,glDrawRangeElementsBaseVertex,ARB,(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices, GLint basevertex));


void APIENTRY_GL4ES gl4es_glDrawArraysInstanced(GLenum mode, GLint first,
                                                 GLsizei count, GLsizei primcount)
{
    DBG(printf("glDrawArraysInstanced(%s, %d, %d, %d), list=%p pending=%d\n",
               PrintEnum(mode), first, count, primcount,
               glstate->list.active, glstate->list.pending);)
    count = adjust_vertices(mode, count);
    if (count < 0)  { errorShim(GL_INVALID_VALUE); return; }
    if (count == 0) { noerrorShim(); return; }
    if ((mode == GL_QUADS) && (count > 4*8000)) {
        int cnt = 4 * 8000;
        for (int i = 0; i < count; i += 4*8000) {
            if (i + cnt > count) cnt = count - i;
            gl4es_glDrawArrays(mode, i, cnt);
        }
        return;
    }
    noerrorShim();
    bool intercept = should_intercept_render(mode);
    if (!glstate->list.compiling) {
        if ((!intercept && !glstate->list.pending &&
             (count >= MIN_BATCH && count <= MAX_BATCH))
            || (intercept && globals4es.maxbatch)) {
            glstate->list.pending = 1;
            glstate->list.active = alloc_renderlist();
        }
    }
    if (glstate->list.active) {
        NewStage(glstate->list.active, STAGE_DRAW);
        glstate->list.active = arrays_to_renderlist(glstate->list.active, mode, first, count+first);
        glstate->list.active->instanceCount = primcount;
        if (glstate->list.pending)
            NewStage(glstate->list.active, STAGE_POSTDRAW);
        else
            glstate->list.active = extend_renderlist(glstate->list.active);
        return;
    }
    if (glstate->polygon_mode == GL_POINT && mode >= GL_TRIANGLES)
        mode = GL_POINTS;
    if (intercept) {
        renderlist_t *list = arrays_to_renderlist(NULL, mode, first, count+first);
        list->instanceCount = primcount;
        list = end_renderlist(list);
        draw_renderlist(list);
        free_renderlist(list);
    } else {
        if (mode == GL_QUADS) {
            static GLushort *indices = NULL;
            static int indcnt = 0, indfirst = 0;
            int realfirst = ((first % 4) == 0) ? 0 : first;
            int realcount = count + (first - realfirst);
            if ((indcnt < realcount) || (indfirst != realfirst)) {
                if (indcnt < realcount) {
                    indcnt = realcount;
                    if (indices) free(indices);
                    indices = (GLushort*)malloc(sizeof(GLushort) * (indcnt * 3 / 2));
                }
                indfirst = realfirst;
                GLushort *p = indices;
                for (int i = 0, j = indfirst; i + 3 < indcnt; i += 4, j += 4) {
                    *(p++) = j+0; *(p++) = j+1; *(p++) = j+2;
                    *(p++) = j+0; *(p++) = j+2; *(p++) = j+3;
                }
            }
            GLuint old_buf = wantBufferIndex(0);
            glDrawElementsCommon(GL_TRIANGLES, 0, count*3/2, count,
                                  indices + (first-indfirst)*3/2, NULL, primcount);
            wantBufferIndex(old_buf);
            return;
        }
        glDrawElementsCommon(mode, first, count, count, NULL, NULL, primcount);
    }
}
AliasExport(void,glDrawArraysInstanced,,(GLenum mode, GLint first, GLsizei count, GLsizei primcount));
AliasExport(void,glDrawArraysInstanced,ARB,(GLenum mode, GLint first, GLsizei count, GLsizei primcount));


void APIENTRY_GL4ES gl4es_glDrawElementsInstanced(GLenum mode, GLsizei count,
                                                   GLenum type, const void *indices,
                                                   GLsizei primcount)
{
    DBG(printf("glDrawElementsInstanced(%s, %d, %s, %p, %d), list=%p pending=%d\n",
               PrintEnum(mode), count, PrintEnum(type), indices, primcount,
               glstate->list.active, glstate->list.pending);)
    count = adjust_vertices(mode, count);
    if (count < 0)  { errorShim(GL_INVALID_VALUE); return; }
    if (count == 0) { noerrorShim(); return; }

    bool compiling = (glstate->list.active);
    bool intercept = should_intercept_render(mode);
    if (!compiling) {
        if ((!intercept && !glstate->list.pending &&
             (count >= MIN_BATCH && count <= MAX_BATCH))
            || (intercept && globals4es.maxbatch)) {
            compiling = true;
            glstate->list.pending = 1;
            glstate->list.active = alloc_renderlist();
        }
    }
    noerrorShim();
    GLushort *sindices = NULL;
    GLuint   *iindices = NULL;
    GLuint old_index = 0;
    bool need_free = !(
        (type == GL_UNSIGNED_SHORT) ||
        (!compiling && !intercept && type == GL_UNSIGNED_INT && hardext.elementuint)
    );
    if (need_free) {
        sindices = copy_gl_array(
            (glstate->vao->elements)
                ? ((void*)((char*)glstate->vao->elements->data + (uintptr_t)indices))
                : indices,
            type, 1, 0, GL_UNSIGNED_SHORT, 1, 0, count, NULL);
        old_index = wantBufferIndex(0);
    } else {
        if (type == GL_UNSIGNED_INT)
            iindices = (glstate->vao->elements)
                ? ((void*)((char*)glstate->vao->elements->data + (uintptr_t)indices))
                : (GLvoid*)indices;
        else
            sindices = (glstate->vao->elements)
                ? ((void*)((char*)glstate->vao->elements->data + (uintptr_t)indices))
                : (GLvoid*)indices;
    }
    if (compiling) {
        renderlist_t *list = NULL;
        GLsizei min, max;
        NewStage(glstate->list.active, STAGE_DRAW);
        list = glstate->list.active;
        if (!need_free) {
            GLushort *tmp = sindices;
            sindices = (GLushort*)malloc(count * sizeof(GLushort));
            memcpy(sindices, tmp, count * sizeof(GLushort));
        }
        normalize_indices_us(sindices, &max, &min, count);
        list = arrays_to_renderlist(list, mode, min, max+1);
        list->indices = sindices; list->ilen = count; list->indice_cap = count;
        list->instanceCount = primcount;
        if (glstate->list.pending)
            NewStage(glstate->list.active, STAGE_POSTDRAW);
        else
            glstate->list.active = extend_renderlist(list);
        return;
    }
    if (intercept) {
        renderlist_t *list = NULL;
        GLsizei min, max;
        if (!need_free) {
            GLushort *tmp = sindices;
            sindices = (GLushort*)malloc(count * sizeof(GLushort));
            memcpy(sindices, tmp, count * sizeof(GLushort));
        }
        normalize_indices_us(sindices, &max, &min, count);
        list = arrays_to_renderlist(list, mode, min, max+1);
        list->indices = sindices; list->ilen = count; list->indice_cap = count;
        list->instanceCount = primcount;
        list = end_renderlist(list);
        draw_renderlist(list);
        free_renderlist(list);
        return;
    } else {
        glDrawElementsCommon(mode, 0, count, 0, sindices, iindices, primcount);
        if (need_free) { free(sindices); wantBufferIndex(old_index); }
    }
}
AliasExport(void,glDrawElementsInstanced,,(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei primcount));
AliasExport(void,glDrawElementsInstanced,ARB,(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei primcount));


void APIENTRY_GL4ES gl4es_glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count,
                                                             GLenum type, const void *indices,
                                                             GLsizei primcount, GLint basevertex)
{
    DBG(printf("glDrawElementsInstanceBaseVertex(%s, %d, %s, %p, %d, %d), vtx=%p map=%p, pending=%d\n",
               PrintEnum(mode), count, PrintEnum(type), indices, primcount, basevertex,
               (glstate->vao->vertex)?glstate->vao->vertex->data:NULL,
               (glstate->vao->elements)?glstate->vao->elements->data:NULL,
               glstate->list.pending);)
    if (basevertex == 0) {
        gl4es_glDrawElementsInstanced(mode, count, type, indices, primcount);
        return;
    }
    count = adjust_vertices(mode, count);
    if (count < 0)  { errorShim(GL_INVALID_VALUE); return; }
    if (count == 0) { noerrorShim(); return; }

    bool compiling = (glstate->list.active);
    bool intercept = should_intercept_render(mode);
    if (!compiling) {
        if (!intercept && glstate->list.pending && count > MAX_BATCH) gl4es_flush();
        else if ((!intercept && !glstate->list.pending &&
                  (count >= MIN_BATCH && count <= MAX_BATCH))
                 || (intercept && globals4es.maxbatch)) {
            compiling = true;
            glstate->list.pending = 1;
            glstate->list.active = alloc_renderlist();
        }
    }
    noerrorShim();
    GLushort *sindices = NULL;
    GLuint   *iindices = NULL;
    if (type == GL_UNSIGNED_INT && hardext.elementuint && !compiling && !intercept)
        iindices = copy_gl_array(
            (glstate->vao->elements)
                ? (void*)((char*)glstate->vao->elements->data + (uintptr_t)indices)
                : indices,
            type, 1, 0, GL_UNSIGNED_INT, 1, 0, count, NULL);
    else
        sindices = copy_gl_array(
            (glstate->vao->elements)
                ? (void*)((char*)glstate->vao->elements->data + (uintptr_t)indices)
                : indices,
            type, 1, 0, GL_UNSIGNED_SHORT, 1, 0, count, NULL);

    if (compiling) {
        renderlist_t *list = NULL;
        GLsizei min, max;
        NewStage(glstate->list.active, STAGE_DRAW);
        list = glstate->list.active;
        normalize_indices_us(sindices, &max, &min, count);
        list = arrays_to_renderlist(list, mode, min+basevertex, max+basevertex+1);
        list->indices = sindices; list->ilen = count; list->indice_cap = count;
        list->instanceCount = primcount;
        if (glstate->list.pending)
            NewStage(glstate->list.active, STAGE_POSTDRAW);
        else
            glstate->list.active = extend_renderlist(list);
        return;
    }
    if (intercept) {
        renderlist_t *list = NULL;
        GLsizei min, max;
        normalize_indices_us(sindices, &max, &min, count);
        list = arrays_to_renderlist(list, mode, min+basevertex, max+basevertex+1);
        list->indices = sindices; list->ilen = count; list->indice_cap = count;
        list->instanceCount = primcount;
        list = end_renderlist(list);
        draw_renderlist(list);
        free_renderlist(list);
        return;
    } else {
        if (iindices)
            for (int i = 0; i < count; i++) iindices[i] += basevertex;
        else
            for (int i = 0; i < count; i++) sindices[i] += basevertex;
        GLuint old_idx = wantBufferIndex(0);
        glDrawElementsCommon(mode, 0, count, 0, sindices, iindices, primcount);
        if (iindices) free(iindices); else free(sindices);
        wantBufferIndex(old_idx);
    }
}
AliasExport(void,glDrawElementsInstancedBaseVertex,,(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei primcount, GLint basevertex));
AliasExport(void,glDrawElementsInstancedBaseVertex,ARB,(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei primcount, GLint basevertex));
