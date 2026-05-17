#ifndef _GLX_HARDEXT_H_
#define _GLX_HARDEXT_H_

// =============================================================================
//  hardext.h — Hardware Capability Descriptor (GLES 3.2 Edition)
//
//  Mendeskripsikan seluruh kapabilitas hardware backend yang terdeteksi saat
//  runtime. Digunakan oleh seluruh subsystem gl4es untuk mengambil keputusan
//  path terbaik (shader generation, texture format, draw path, dll.).
//
//  Target: OpenGL ES 3.2 (Android arm64-v8a, Zalith Launcher)
//          Fallback: GLES 3.0 → GLES 2.0
// =============================================================================

#include "../gl/attributes.h"

// ── Vendor IDs ────────────────────────────────────────────────────────────────
// Digunakan untuk workaround bug driver per-vendor
#define VEND_UNKNOWN    0x0000
#define VEND_IMGTEC     0x0100   // Imagination Technologies (PowerVR)
#define VEND_ARM        0x0200   // ARM (Mali)
#define VEND_QUALCOMM   0x0400   // Qualcomm (Adreno)
#define VEND_SAMSUNG    0x0800   // Samsung (Xclipse / RDNA-mobile)
#define VEND_NVIDIA     0x1000   // NVIDIA (Shield / Tegra)
#define VEND_BROADCOM   0x2000   // Broadcom (RPi VideoCore)

// ── Konstanta ─────────────────────────────────────────────────────────────────
#define MAX_PRGBIN      10       // Maks format program binary yang ditampung

// =============================================================================
//  Struct hardext_t
//  Setiap field = 0 berarti fitur TIDAK tersedia / dinonaktifkan.
//  Nilai positif = tersedia, nilainya bisa berarti level/count.
// =============================================================================
typedef struct _hardext {

    // ── Versi Backend ─────────────────────────────────────────────────────
    // esversion: 1 = ES 1.1 | 2 = ES 2.0 | 3 = ES 3.x
    // esminor  : sub-versi dari ES 3.x (0=3.0, 1=3.1, 2=3.2)
    int esversion;
    int esminor;

    // ── Batas Hardware ────────────────────────────────────────────────────
    int maxsize;            // GL_MAX_TEXTURE_SIZE
    int maxtex;             // Unit tekstur aktif (capped ke MAX_TEX)
    int maxteximage;        // GL_MAX_TEXTURE_IMAGE_UNITS (fragment stage)
    int maxlights;          // Lampu simulasi (via FPE), maks 8
    int maxplanes;          // Clip plane simulasi (via FPE), maks 6
    int maxvattrib;         // GL_MAX_VERTEX_ATTRIBS
    int maxvarying;         // GL_MAX_VARYING_VECTORS
    int maxcolorattach;     // GL_MAX_COLOR_ATTACHMENTS
    int maxdrawbuffers;     // GL_MAX_DRAW_BUFFERS
    int maxsamples;         // GL_MAX_SAMPLES — untuk MSAA (ES 3.0+ core)

    // ── Tekstur ───────────────────────────────────────────────────────────
    // npot: 0=tidak ada, 1=terbatas, 2=terbatas+mipmap, 3=penuh
    int npot;
    int bgra8888;           // GL_EXT_texture_format_BGRA8888
    int rgtex;              // GL_EXT_texture_rg — format R/RG
    int floattex;           // GL_OES_texture_float
    int halffloattex;       // GL_OES_texture_half_float
    int floatfbo;           // GL_EXT_color_buffer_float
    int halffloatfbo;       // GL_EXT_color_buffer_half_float
    int mirrored;           // GL_OES_texture_mirrored_repeat / ES2+ core
    int aniso;              // GL_EXT_texture_filter_anisotropic (0=off, N=max)
    int depthtex;           // GL_OES_depth_texture
    int stenciltex;         // GL_OES_texture_stencil8
    int cubemap;            // Cube map textures
    int drawtex;            // GL_OES_draw_texture (ES1 hanya)
    int texture_storage;    // glTexStorage* — core ES 3.0+
    int astc;               // GL_KHR_texture_compression_astc_ldr (ES 3.2 core)
    int s3tc;               // GL_EXT_texture_compression_s3tc / GL_EXT_texture_compression_dxt1
    int etc2;               // ETC2/EAC — core ES 3.0+
    int srgb;               // EGL_KHR_gl_colorspace

    // AmigaOS4-specific (dipertahankan untuk kompatibilitas)
    int rgb332;
    int rgb332rev;
    int rgba1555rev;
    int rgba8888;
    int rgba8888rev;

    // ── Blending ──────────────────────────────────────────────────────────
    int blendsub;           // GL_OES_blend_subtract / ES2+ core
    int blendfunc;          // GL_OES_blend_func_separate / ES2+ core
    int blendeq;            // GL_OES_blend_equation_separate / ES2+ core
    int blendminmax;        // GL_EXT_blend_minmax / ES3+ core
    int blendcolor;         // GL_EXT_blend_color / ES2+ core
    int blend_eq_advanced;  // GL_KHR_blend_equation_advanced (ES 3.2 core)

    // ── Framebuffer ───────────────────────────────────────────────────────
    int fbo;                // Framebuffer Objects
    int depthstencil;       // GL_OES_packed_depth_stencil
    int depth24;            // GL_OES_depth24
    int rgba8;              // GL_OES_rgb8_rgba8
    int drawbuffers;        // GL_EXT_draw_buffers / ES3+ core

    // ── Shader & GLSL ─────────────────────────────────────────────────────
    // highp: 0=tidak ada | 1=via extension | 2=native (tidak perlu deklarasi)
    int highp;
    int fragdepth;          // GL_EXT_frag_depth / ES3+ core
    int derivatives;        // GL_OES_standard_derivatives / ES3+ core
    int shaderlod;          // GL_EXT_shader_texture_lod
    int cubelod;            // textureCubeLod tanpa suffix EXT (bug driver PVR)
    int shader_fbfetch;     // GL_ARM_shader_framebuffer_fetch
    int glsl120;            // GLSL 1.20 diterima (desktop compat via conversion)
    int glsl300es;          // GLSL ES 3.00
    int glsl310es;          // GLSL ES 3.10
    int glsl320es;          // GLSL ES 3.20 ← TARGET UTAMA

    // ── Kapabilitas ES 3.2 Core ───────────────────────────────────────────
    int geometry_shader;    // Geometry shader (ES 3.2 core)
    int tessellation_shader;// Tessellation shader (ES 3.2 core)
    int sample_shading;     // Per-sample shading / GL_OES_sample_shading (ES 3.2 core)
    int debug;              // GL_KHR_debug (ES 3.2 core)

    // ── Kapabilitas ES 3.0+ Core ──────────────────────────────────────────
    int pbo;                // Pixel Buffer Objects — glReadPixels async, texture upload
    int ubo;                // Uniform Buffer Objects
    int vao_native;         // Native VAO (bukan emulasi) — core ES 3.0+
    int instanced;          // glDrawArraysInstanced / glDrawElementsInstanced
    int transform_feedback; // Transform feedback
    int sync_obj;           // Fence/sync objects — glFenceSync, glWaitSync

    // ── Kapabilitas ES 3.1+ Core ──────────────────────────────────────────
    int ssbo;               // Shader Storage Buffer Objects
    int compute_shader;     // Compute shaders
    int indirect_draw;      // glDrawArraysIndirect / glDrawElementsIndirect

    // ── ES 1.1 Specific ───────────────────────────────────────────────────
    int pointsprite;
    int pointsize;

    // ── Misc ──────────────────────────────────────────────────────────────
    int elementuint;        // GL_OES_element_index_uint / ES3+ core
    int multidraw;          // GL_EXT_multi_draw_arrays
    int mapbuffer;          // GL_OES_mapbuffer
    int prgbinary;          // GL_OES_get_program_binary
    int prgbin_n;           // Jumlah format binary program yang didukung
    int gbm;                // EGL_KHR_platform_gbm
    int khr_pixmap;         // EGL_KHR_image_pixmap
    int khr_texture_2d;     // EGL_KHR_gl_texture_2D_image
    int khr_renderbuffer;   // EGL_KHR_gl_renderbuffer_image
    int vendor;             // Vendor GPU (VEND_*)
    int eglnoalpha;         // EGL surface tanpa channel alpha (auto-detect)

} hardext_t;

// ── Global instance & string info ─────────────────────────────────────────────
EXPORT extern hardext_t hardext;
EXPORT extern char* gl4es_original_vendor;
EXPORT extern char* gl4es_original_renderer;

// ── Entry point utama ─────────────────────────────────────────────────────────
// notest=1 → skip hardware probe, gunakan nilai default minimal
EXPORT void GetHardwareExtensions(int notest);

// ── Inline helpers ────────────────────────────────────────────────────────────
// Gunakan fungsi ini daripada mengakses esversion/esminor langsung
// agar lebih mudah diubah nantinya

// Backend adalah GLES 2.0 atau lebih tinggi?
static inline int hardext_is_gles2plus(void) {
    return hardext.esversion >= 2;
}

// Backend adalah GLES 3.0 atau lebih tinggi?
static inline int hardext_is_gles3plus(void) {
    return hardext.esversion >= 3;
}

// Backend adalah tepat GLES 3.1 atau lebih tinggi?
static inline int hardext_is_gles31plus(void) {
    return (hardext.esversion == 3 && hardext.esminor >= 1);
}

// Backend adalah tepat GLES 3.2? ← Ini yang kita target untuk Minecraft 1.12.2
static inline int hardext_is_gles32(void) {
    return (hardext.esversion == 3 && hardext.esminor >= 2);
}

#endif // _GLX_HARDEXT_H_
