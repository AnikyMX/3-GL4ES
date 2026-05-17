#ifndef _GL4ES_INIT_H_
#define _GL4ES_INIT_H_

// =============================================================================
//  init.h — Global Runtime Configuration (GLES 3.2 Edition)
//
//  ATURAN PENTING:
//  - Seluruh field original DIPERTAHANKAN dengan nama yang IDENTIK agar
//    kompatibel dengan seluruh subsystem yang sudah ada (drawing, texture, dll)
//  - Field baru untuk ES 3.x ditandai dengan komentar "// [ES3.x NEW]"
//  - Setelah GetHardwareExtensions(), gunakan hardext.* untuk kapabilitas GPU
//    dan globals4es.* untuk konfigurasi user (env var override, tuning, dll)
// =============================================================================

#if defined(PANDORA)
#define USE_FBIO 1
#endif

typedef struct _globals4es {
    // ── Original Fields ── (JANGAN UBAH NAMA / URUTAN) ───────────────────
    int nobanner;
    int mergelist;
    int xrefresh;
    int stacktrace;
    int usefb;
    int usegbm;
    int usefbo;
    int recyclefbo;
    int usepbuffer;
    int showfps;
    int vsync;
    int automipmap;
    int texcopydata;
    int tested_env;
    int texshrink;
    int texdump;
    int alphahack;
    int texstream;
    int nolumalpha;
    int blendhack;
    int blendcolor;
    int noerror;
    int npot;
    int defaultwrap;
    int notexrect;
    int queries;
    int silentstub;
    int glx_surface_srgb;
    int nodownsampling;
    int vabgra;
    int nobgra;
    int potframebuffer;
    float gamma;
    int texmat;
    int novaocache;
    int beginend;
    int avoid16bits;
    int avoid24bits;
    int force16bits;
    int nohighp;
    int minbatch;
    int maxbatch;
    // es : 1=ES1.1 | 2=ES2.0 | 3=ES3.x  (dikontrol via LIBGL_ES)
    int es;
    // gl : versi OpenGL yang di-expose ke aplikasi (misal 21=GL2.1)
    int gl;
    int usevbo;
    int comments;
    int forcenpot;
    int fbomakecurrent;
    int fbounbind;
    int fboforcetex;
    int blitfullscreen;
    int notexarray;
    int nodepthtex;
    int logshader;
    int shadernogles;
    int floattex;
    int glxrecycle;
    int noclean;
    int dbgshaderconv;
    int nopsa;
    int noes2;
    int nointovlhack;
    int noshaderlod;
    int fbo_noalpha;
    int noarbprogram;
    int glxnative;
    int normalize;
    int blitfb0;
    int skiptexcopies;
    int shaderblend;
    int deepbind;
    float fbtexscale;
#ifndef NO_GBM
    char drmcard[50];
#endif
    char version[50];

    // ── ES 3.x Feature Flags ── [ES3.x NEW] ──────────────────────────────
    // Diset SETELAH GetHardwareExtensions() berhasil.
    // Subsystem lain (drawing, texture, buffers) cek flag ini — bukan hardext
    // langsung — agar ada satu titik kontrol + override via env var.

    // esminor: sub-versi ES 3.x yang aktif (dari hardext): 0=3.0|1=3.1|2=3.2
    int esminor;

    // use_native_vao: gunakan VAO native GLES 3.0+ (bukan VAO cache software)
    int use_native_vao;

    // use_pbo: gunakan PBO untuk texture upload/download async (GLES 3.0+)
    //   Mengurangi CPU-stall pada glTexImage2D dan glReadPixels di MC 1.12.2
    int use_pbo;

    // use_instanced: gunakan glDraw*Instanced (GLES 3.0+)
    int use_instanced;

    // use_ubo: gunakan Uniform Buffer Objects (GLES 3.0+)
    int use_ubo;

    // use_sync: gunakan fence/sync objects (GLES 3.0+)
    int use_sync;

    // use_mapbuffer_range: gunakan glMapBufferRange (GLES 3.0+)
    //   Lebih efisien dari glMapBuffer OES untuk partial VBO update
    int use_mapbuffer_range;

    // glsl_target: versi GLSL ES target untuk FPE & shaderconv
    //   Nilai: 120 | 300 | 310 | 320
    //   Default otomatis: 320 jika ES3.2, 300 jika ES3.0, 120 jika ES2
    int glsl_target;

} globals4es_t;

extern globals4es_t globals4es;

#endif // _GL4ES_INIT_H_
