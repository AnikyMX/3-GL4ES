// =============================================================================
//  hardext.c — Hardware Capability Detection (GLES 3.2 Edition)
//
//  Mendeteksi seluruh kapabilitas backend GLES secara runtime dengan:
//  1. Membuat EGL PBuffer context ES 3.2 → ES 3.0 → ES 2.0 (fallback chain)
//  2. Membaca GL_VERSION untuk esversion/esminor yang tepat
//  3. Iterasi ekstensi via glGetStringi() (GLES 3.0+) atau glGetString()
//  4. Compile shader uji untuk konfirmasi versi GLSL yang benar-benar jalan
//
//  Target: OpenGL ES 3.2, GLSL ES 3.20 (arm64-v8a, Zalith Launcher)
// =============================================================================

#include "hardext.h"

#include "../gl/debug.h"
#include "../gl/gl4es.h"
#include "../gl/init.h"
#include "../gl/logs.h"
#include "../gl/loader.h"
#ifndef ANDROID
#include "rpi.h"
#endif
#include "glx_gbm.h"

#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR  0x31D7
#endif

// EGL_CONTEXT_MINOR_VERSION sudah didefinisikan di egl.h (0x30FB),
// tapi kita pastikan via fallback jika header lama
#ifndef EGL_CONTEXT_MINOR_VERSION
#define EGL_CONTEXT_MINOR_VERSION  0x30FB
#endif

// ES3-bit untuk eglChooseConfig — dari EGL 1.5 / KHR_create_context
#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT  0x00000040
#endif

// ── State guard ───────────────────────────────────────────────────────────────
static int tested = 0;

// ── Global instance ───────────────────────────────────────────────────────────
hardext_t hardext = {0};

char* gl4es_original_vendor   = NULL;
char* gl4es_original_renderer = NULL;

// =============================================================================
//  Helpers internal
// =============================================================================

// -----------------------------------------------------------------------------
//  hasExtension — cek satu ekstensi dari string besar (glGetString path)
//  Pastikan nama ekstensi diikuti spasi atau '\0' agar tidak false-positive
//  (misalnya "GL_EXT_draw_buffers" vs "GL_EXT_draw_buffers_indexed")
// -----------------------------------------------------------------------------
static int hasExtension(const char* exts, const char* name) {
    if (!exts || !name) return 0;
    const char* p = exts;
    size_t nlen = strlen(name);
    while ((p = strstr(p, name)) != NULL) {
        char after = p[nlen];
        if (after == ' ' || after == '\0')
            return 1;
        p += nlen;
    }
    return 0;
}

// -----------------------------------------------------------------------------
//  hasExtensionI — cek ekstensi via glGetStringi (GLES 3.0+, lebih akurat)
//  Menggunakan exact string comparison — tidak perlu khawatir false-positive
// -----------------------------------------------------------------------------
static int hasExtensionI(const char* name) {
    LOAD_GLES2(glGetStringi);
    LOAD_GLES(glGetIntegerv);
    if (!gles_glGetStringi) return 0;

    GLint n = 0;
    gles_glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; i++) {
        const char* ext = (const char*)gles_glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (ext && strcmp(ext, name) == 0)
            return 1;
    }
    return 0;
}

// Macro pembantu: cek ekstensi, set field, dan log
// Gunakan glGetStringi jika GLES3+, fallback ke glGetString
#define EXT_CHECK_I(NAME, FIELD)                                            \
    do {                                                                    \
        if (hardext.esversion >= 3) {                                       \
            if (hasExtensionI(NAME)) {                                      \
                hardext.FIELD = 1;                                          \
                SHUT_LOGD("Extension %s detected and used\n", NAME);        \
            }                                                               \
        } else {                                                            \
            if (hasExtension(Exts, NAME " ")) {                             \
                hardext.FIELD = 1;                                          \
                SHUT_LOGD("Extension %s detected and used\n", NAME);        \
            }                                                               \
        }                                                                   \
    } while(0)

// Versi tanpa log "and used" — untuk ekstensi yang hanya dicatat
#define EXT_DETECT(NAME, FIELD)                                             \
    do {                                                                    \
        if (hardext.esversion >= 3) {                                       \
            if (hasExtensionI(NAME)) {                                      \
                hardext.FIELD = 1;                                          \
                SHUT_LOGD("Extension %s detected\n", NAME);                 \
            }                                                               \
        } else {                                                            \
            if (hasExtension(Exts, NAME " ")) {                             \
                hardext.FIELD = 1;                                          \
                SHUT_LOGD("Extension %s detected\n", NAME);                 \
            }                                                               \
        }                                                                   \
    } while(0)

// =============================================================================
//  testGLSL — Compile shader uji untuk konfirmasi versi GLSL yang berjalan
//  Mengembalikan 1 jika berhasil compile, 0 jika gagal.
//
//  Shader yang diuji sengaja sederhana (hanya transform MVP) agar cepat.
//  Untuk GLSL ES 3.20 kita wajib pakai 'in' bukan 'attribute', dan
//  texture() bukan texture2D() — inilah yang kita verifikasi.
// =============================================================================
static int testGLSL(const char* version_str, int use_in_qualifier) {
    LOAD_GLES2(glCreateShader);
    LOAD_GLES2(glShaderSource);
    LOAD_GLES2(glCompileShader);
    LOAD_GLES2(glGetShaderiv);
    LOAD_GLES2(glDeleteShader);
    LOAD_GLES(glGetError);

    if (!gles_glCreateShader) return 0;

    GLuint shad = gles_glCreateShader(GL_VERTEX_SHADER);
    if (!shad) return 0;

    // Pilih qualifier berdasarkan versi:
    // GLSL ES 1.00 / GLSL 1.20  → "attribute"
    // GLSL ES 3.00+              → "in"
    const char* qualifier = use_in_qualifier ? "in" : "attribute";

    // Buffer untuk baris qualifier agar tidak ada alokasi heap
    char qual_line[64];
    snprintf(qual_line, sizeof(qual_line), "%s vec4 vecPos;\n", qualifier);

    const char* parts[4] = {
        version_str,
        "\n",
        qual_line,
        "uniform mat4 matMVP;\n"
        "void main() {\n"
        "    gl_Position = matMVP * vecPos;\n"
        "}\n"
    };
    gles_glShaderSource(shad, 4, parts, NULL);
    gles_glCompileShader(shad);

    GLint compiled = GL_FALSE;
    gles_glGetShaderiv(shad, GL_COMPILE_STATUS, &compiled);
    gles_glDeleteShader(shad);
    gles_glGetError();  // bersihkan error state

    return (compiled == GL_TRUE) ? 1 : 0;
}

// Uji khusus: apakah textureCubeLod bekerja tanpa suffix EXT di fragment shader?
// Beberapa driver PVR lama membutuhkan nama tanpa EXT.
static int testTextureCubeLod(void) {
    LOAD_GLES2(glCreateShader);
    LOAD_GLES2(glShaderSource);
    LOAD_GLES2(glCompileShader);
    LOAD_GLES2(glGetShaderiv);
    LOAD_GLES2(glDeleteShader);
    LOAD_GLES(glGetError);

    if (!gles_glCreateShader) return 0;

    GLuint shad = gles_glCreateShader(GL_FRAGMENT_SHADER);
    if (!shad) return 0;

    const char* src =
        "#version 100\n"
        "#extension GL_EXT_shader_texture_lod : enable\n"
        "uniform samplerCube samCube;\n"
        "varying mediump vec3 coordCube;\n"
        "void main() {\n"
        "    gl_FragColor = textureCubeLod(samCube, coordCube, 0.0);\n"
        "}\n";

    gles_glShaderSource(shad, 1, &src, NULL);
    gles_glCompileShader(shad);

    GLint compiled = GL_FALSE;
    gles_glGetShaderiv(shad, GL_COMPILE_STATUS, &compiled);
    gles_glDeleteShader(shad);
    gles_glGetError();

    return (compiled == GL_TRUE) ? 1 : 0;
}

// =============================================================================
//  parseESVersion — parse string "OpenGL ES X.Y ..." dari glGetString(GL_VERSION)
//  Output: *major, *minor
// =============================================================================
static void parseESVersion(const char* verstr, int* major, int* minor) {
    *major = 2; *minor = 0;  // default aman
    if (!verstr) return;

    // Format: "OpenGL ES X.Y ..."
    const char* p = strstr(verstr, "OpenGL ES ");
    if (p) {
        p += 10;  // lewati "OpenGL ES "
        *major = (int)(*p - '0');
        if (p[1] == '.' && p[2] >= '0')
            *minor = (int)(p[2] - '0');
        return;
    }
    // Fallback: coba parse langsung "X.Y"
    if (verstr[0] >= '1' && verstr[0] <= '3') {
        *major = (int)(verstr[0] - '0');
        if (verstr[1] == '.' && verstr[2] >= '0')
            *minor = (int)(verstr[2] - '0');
    }
}

// =============================================================================
//  detectVendor — deteksi GPU vendor dari GL_VENDOR string
// =============================================================================
static int detectVendor(const char* vendor) {
    if (!vendor) return VEND_UNKNOWN;
    if (strstr(vendor, "ARM") || strstr(vendor, "Mali"))
        return VEND_ARM;
    if (strstr(vendor, "Imagination") || strstr(vendor, "PowerVR"))
        return VEND_IMGTEC;
    if (strstr(vendor, "Qualcomm") || strstr(vendor, "Adreno"))
        return VEND_QUALCOMM;
    if (strstr(vendor, "Samsung") || strstr(vendor, "Xclipse"))
        return VEND_SAMSUNG;
    if (strstr(vendor, "NVIDIA") || strstr(vendor, "Tegra"))
        return VEND_NVIDIA;
    if (strstr(vendor, "Broadcom") || strstr(vendor, "VideoCore"))
        return VEND_BROADCOM;
    return VEND_UNKNOWN;
}

// =============================================================================
//  tryCreateContext — coba buat EGL context dengan versi major.minor tertentu
//  Mengembalikan context yang valid, atau EGL_NO_CONTEXT jika gagal.
//
//  Menggunakan EGL_CONTEXT_MAJOR_VERSION + EGL_CONTEXT_MINOR_VERSION
//  (EGL 1.5 / KHR_create_context) agar bisa request tepat GLES 3.2.
// =============================================================================
static EGLContext tryCreateContext(EGLDisplay dpy, EGLConfig cfg,
                                   int major, int minor)
{
    // EGL_CONTEXT_MAJOR_VERSION = 0x3098 (alias EGL_CONTEXT_CLIENT_VERSION)
    // EGL_CONTEXT_MINOR_VERSION = 0x30FB (EGL 1.5)
    const EGLint attribs[] = {
        0x3098,                  // EGL_CONTEXT_MAJOR_VERSION
        (EGLint)major,
        EGL_CONTEXT_MINOR_VERSION,
        (EGLint)minor,
        EGL_NONE
    };
    LOAD_EGL(eglCreateContext);
    return egl_eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, attribs);
}

// =============================================================================
//  GetHardwareExtensions — entry point utama
//
//  notest=1 → mode minimal/offline: isi default tanpa probe hardware.
//              Dipakai ketika context belum tersedia atau saat testing.
//  notest=0 → probe penuh: buat EGL PBuffer, detect semua kapabilitas.
// =============================================================================
EXPORT
void GetHardwareExtensions(int notest)
{
    if (tested) return;

    // ── Nilai default yang selalu aman ──────────────────────────────────────
    hardext.maxtex         = 2;
    hardext.maxsize        = 2048;
    hardext.maxlights      = 8;
    hardext.maxplanes      = 6;
    hardext.maxdrawbuffers = 1;
    hardext.maxcolorattach = 1;
    hardext.maxsamples     = 0;

    hardext.esversion = globals4es.es;
    hardext.esminor   = 0;

    // ── Mode minimal (notest=1) ─────────────────────────────────────────────
    if (notest) {
        SHUT_LOGD("Hardware test disabled, using safe defaults\n");
        if (hardext.esversion >= 2) {
            hardext.maxteximage    = 8;
            hardext.maxvarying     = 8;
            hardext.maxtex         = 8;
            hardext.maxvattrib     = 16;
            hardext.npot           = 1;
            hardext.fbo            = 1;
            hardext.blendcolor     = 1;
            hardext.blendsub       = 1;
            hardext.blendfunc      = 1;
            hardext.blendeq        = 1;
            hardext.mirrored       = 1;
            hardext.pointsprite    = 1;
            hardext.pointsize      = 1;
            hardext.cubemap        = 1;
            hardext.maxdrawbuffers = 1;
            hardext.elementuint    = 1;
            hardext.glsl300es      = 0; // tidak diasumsi tanpa probe
        }
        if (hardext.esversion >= 3) {
            hardext.esminor        = 0; // ES 3.0 minimal
            hardext.pbo            = 1;
            hardext.ubo            = 1;
            hardext.vao_native     = 1;
            hardext.instanced      = 1;
            hardext.drawbuffers    = 1;
            hardext.derivatives    = 1;
            hardext.fragdepth      = 1;
            hardext.blendminmax    = 1;
            hardext.etc2           = 1;
            hardext.texture_storage= 1;
            hardext.sync_obj       = 1;
            hardext.glsl300es      = 1;
        }
        return;
    }

    // ── RPi / BCM init (non-Android only) ──────────────────────────────────
#if defined(BCMHOST) && !defined(ANDROID)
    rpi_init();
#endif

    // ── EGL Setup ──────────────────────────────────────────────────────────
#ifndef NOEGL
    LOAD_EGL(eglBindAPI);
    LOAD_EGL(eglInitialize);
    LOAD_EGL(eglGetDisplay);
    LOAD_EGL(eglCreatePbufferSurface);
    LOAD_EGL(eglDestroySurface);
    LOAD_EGL(eglDestroyContext);
    LOAD_EGL(eglMakeCurrent);
    LOAD_EGL(eglChooseConfig);
    LOAD_EGL(eglCreateContext);
    LOAD_EGL(eglQueryString);
    LOAD_EGL(eglTerminate);

    EGLDisplay eglDisplay;
    EGLSurface eglSurface  = EGL_NO_SURFACE;
    EGLContext eglContext  = EGL_NO_CONTEXT;

    // ── Display ────────────────────────────────────────────────────────────
#ifndef NO_GBM
    if (globals4es.usegbm) {
        LoadGBMFunctions();
        eglDisplay = OpenGBMDisplay(EGL_DEFAULT_DISPLAY);
    } else
#endif
        eglDisplay = egl_eglGetDisplay(EGL_DEFAULT_DISPLAY);

    egl_eglBindAPI(EGL_OPENGL_ES_API);
    if (egl_eglInitialize(eglDisplay, NULL, NULL) != EGL_TRUE) {
        LOGE("GetHardwareExtensions: eglInitialize failed (%s), "
             "falling back to safe defaults\n", PrintEGLError(0));
        egl_eglTerminate(eglDisplay);
        return;
    }

    // ── GBM / EGL extension check (sebelum context) ────────────────────────
#ifndef NO_GBM
    {
        const char* eglExts = egl_eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
        if (eglExts && hasExtension(eglExts, "EGL_KHR_platform_gbm")) {
            SHUT_LOGD("EGL: GBM platform supported%s\n",
                      globals4es.usegbm ? " and used" : "");
            hardext.gbm = 1;
        }
    }
#endif

    // ── Config selection ───────────────────────────────────────────────────
    // Kita coba dulu dengan EGL_OPENGL_ES3_BIT untuk GLES 3.x
    // Jika gagal, fallback ke ES2_BIT
    EGLint configAttribs_es3[] = {
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_NONE
    };
    EGLint configAttribs_es2[] = {
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };

    EGLConfig  pbufConfig  = NULL;
    EGLint     configsFound = 0;
    int        using_es3_config = 0;

    // Coba ES3 config dulu
    egl_eglChooseConfig(eglDisplay, configAttribs_es3, &pbufConfig, 1, &configsFound);
    if (configsFound > 0) {
        using_es3_config = 1;
        SHUT_LOGD("EGL: ES3 config tersedia\n");
    } else {
        // Fallback ke ES2 config
        egl_eglChooseConfig(eglDisplay, configAttribs_es2, &pbufConfig, 1, &configsFound);
        if (configsFound > 0) {
            SHUT_LOGD("EGL: ES3 config tidak ada, fallback ke ES2 config\n");
        }
    }

    // Coba tanpa alpha jika masih belum dapat config
    if (!configsFound) {
        configAttribs_es3[3*2+1] = 0; // EGL_ALPHA_SIZE = 0
        egl_eglChooseConfig(eglDisplay, configAttribs_es3, &pbufConfig, 1, &configsFound);
        if (configsFound) {
            using_es3_config = 1;
            hardext.eglnoalpha = 1;
            SHUT_LOGD("EGL: No-alpha ES3 config\n");
        }
    }
    if (!configsFound) {
        configAttribs_es2[3*2+1] = 0;
        egl_eglChooseConfig(eglDisplay, configAttribs_es2, &pbufConfig, 1, &configsFound);
        if (configsFound) {
            hardext.eglnoalpha = 1;
            SHUT_LOGD("EGL: No-alpha ES2 config\n");
        }
    }

    if (!configsFound) {
        LOGE("GetHardwareExtensions: eglChooseConfig gagal (%s)\n",
             PrintEGLError(0));
        egl_eglTerminate(eglDisplay);
        return;
    }

    // ── Context creation: coba GLES 3.2 → 3.1 → 3.0 → 2.0 ───────────────
    int actual_major = 2, actual_minor = 0;

    if (using_es3_config) {
        // Coba GLES 3.2
        eglContext = tryCreateContext(eglDisplay, pbufConfig, 3, 2);
        if (eglContext != EGL_NO_CONTEXT) {
            actual_major = 3; actual_minor = 2;
            SHUT_LOGD("EGL: Context GLES 3.2 berhasil dibuat\n");
        }
        // Coba GLES 3.1
        if (eglContext == EGL_NO_CONTEXT) {
            eglContext = tryCreateContext(eglDisplay, pbufConfig, 3, 1);
            if (eglContext != EGL_NO_CONTEXT) {
                actual_major = 3; actual_minor = 1;
                SHUT_LOGD("EGL: Context GLES 3.1 berhasil dibuat\n");
            }
        }
        // Coba GLES 3.0
        if (eglContext == EGL_NO_CONTEXT) {
            eglContext = tryCreateContext(eglDisplay, pbufConfig, 3, 0);
            if (eglContext != EGL_NO_CONTEXT) {
                actual_major = 3; actual_minor = 0;
                SHUT_LOGD("EGL: Context GLES 3.0 berhasil dibuat\n");
            }
        }
    }
    // Fallback GLES 2.0
    if (eglContext == EGL_NO_CONTEXT) {
        eglContext = tryCreateContext(eglDisplay, pbufConfig, 2, 0);
        if (eglContext != EGL_NO_CONTEXT) {
            actual_major = 2; actual_minor = 0;
            SHUT_LOGD("EGL: Context GLES 2.0 (fallback)\n");
        }
    }

    if (eglContext == EGL_NO_CONTEXT) {
        LOGE("GetHardwareExtensions: Tidak bisa buat context EGL (%s)\n",
             PrintEGLError(0));
        egl_eglTerminate(eglDisplay);
        return;
    }

    // ── PBuffer surface ────────────────────────────────────────────────────
    const EGLint pbuf_attribs[] = {
        EGL_WIDTH,  32,
        EGL_HEIGHT, 32,
        EGL_NONE
    };
    eglSurface = egl_eglCreatePbufferSurface(eglDisplay, pbufConfig, pbuf_attribs);
    if (eglSurface == EGL_NO_SURFACE) {
        LOGE("GetHardwareExtensions: eglCreatePbufferSurface gagal (%s)\n",
             PrintEGLError(0));
        egl_eglDestroyContext(eglDisplay, eglContext);
        egl_eglTerminate(eglDisplay);
        return;
    }

    egl_eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext);
#endif  // !NOEGL

    // ─────────────────────────────────────────────────────────────────────────
    //  Context aktif sekarang — mulai probe
    // ─────────────────────────────────────────────────────────────────────────
    tested = 1;

    LOAD_GLES(glGetString);
    LOAD_GLES(glGetIntegerv);
    LOAD_GLES(glGetError);
    LOAD_GLES2(glGetStringi);

    // ── Parse versi ES yang sesungguhnya dari GL_VERSION ───────────────────
    // Versi dari context creation bisa berbeda dengan yang dilaporkan driver.
    // glGetString(GL_VERSION) adalah sumber kebenaran.
    {
        const char* verstr = (const char*)gles_glGetString(GL_VERSION);
        int real_major, real_minor;
        parseESVersion(verstr, &real_major, &real_minor);
        SHUT_LOGD("GL_VERSION: %s → ES %d.%d\n",
                  verstr ? verstr : "NULL", real_major, real_minor);

        // Gunakan nilai yang lebih konservatif antara context attrib dan
        // GL_VERSION yang dilaporkan
        if (real_major < actual_major ||
           (real_major == actual_major && real_minor < actual_minor)) {
            actual_major = real_major;
            actual_minor = real_minor;
        }
        hardext.esversion = actual_major;
        hardext.esminor   = actual_minor;
    }

    SHUT_LOGD("Backend: OpenGL ES %d.%d\n", hardext.esversion, hardext.esminor);

    // ── Ambil string ekstensi (untuk ES < 3.0, satu string panjang) ────────
    const char* Exts = NULL;
    if (hardext.esversion < 3) {
        Exts = (const char*)gles_glGetString(GL_EXTENSIONS);
        if (!Exts) Exts = "";
    }
    // Untuk ES 3.0+, kita gunakan glGetStringi via EXT_CHECK_I/hasExtensionI

    // ── Vendor & Renderer ──────────────────────────────────────────────────
    {
        const char* vendor = (const char*)gles_glGetString(GL_VENDOR);
        SHUT_LOGD("GL_VENDOR: %s\n", vendor ? vendor : "NULL");
        if (!gl4es_original_vendor && vendor)
            gl4es_original_vendor = strdup(vendor);
        hardext.vendor = detectVendor(vendor ? vendor : "");

        const char* renderer = (const char*)gles_glGetString(GL_RENDERER);
        SHUT_LOGD("GL_RENDERER: %s\n", renderer ? renderer : "NULL");
        if (!gl4es_original_renderer && renderer)
            gl4es_original_renderer = strdup(renderer);
    }

    // =========================================================================
    //  Fitur yang SELALU tersedia di ES 2.0+ (tidak perlu cek ekstensi)
    // =========================================================================
    if (hardext.esversion >= 2) {
        hardext.fbo         = 1;  SHUT_LOGD("FBO: core ES2+\n");
        hardext.pointsprite = 1;  SHUT_LOGD("PointSprite: core ES2+\n");
        hardext.pointsize   = 1;
        hardext.cubemap     = 1;  SHUT_LOGD("CubeMap: core ES2+\n");
        hardext.blendcolor  = 1;  SHUT_LOGD("BlendColor: core ES2+\n");
        hardext.blendsub    = 1;
        hardext.blendfunc   = 1;
        hardext.blendeq     = 1;
        hardext.mirrored    = 1;
        hardext.elementuint = 1;
        hardext.maxlights   = 8;   // Disimulasi FPE
        hardext.maxplanes   = 6;   // Disimulasi FPE
    }

    // =========================================================================
    //  Fitur yang SELALU tersedia di ES 3.0+ (tidak perlu cek ekstensi)
    // =========================================================================
    if (hardext.esversion >= 3) {
        hardext.drawbuffers     = 1;  SHUT_LOGD("DrawBuffers: core ES3+\n");
        hardext.blendminmax     = 1;
        hardext.derivatives     = 1;
        hardext.fragdepth       = 1;
        hardext.highp           = 2;  // Selalu tersedia di ES 3.0+
        hardext.pbo             = 1;  SHUT_LOGD("PBO: core ES3+\n");
        hardext.ubo             = 1;  SHUT_LOGD("UBO: core ES3+\n");
        hardext.vao_native      = 1;  SHUT_LOGD("VAO native: core ES3+\n");
        hardext.instanced       = 1;  SHUT_LOGD("Instanced draw: core ES3+\n");
        hardext.transform_feedback = 1;
        hardext.sync_obj        = 1;
        hardext.texture_storage = 1;
        hardext.etc2            = 1;  SHUT_LOGD("ETC2/EAC: core ES3+\n");
        hardext.depthstencil    = 1;
        hardext.depth24         = 1;
        hardext.rgba8           = 1;
        hardext.depthtex        = 1;
        hardext.npot            = 3;  SHUT_LOGD("Full NPOT: core ES3+\n");
    }

    // =========================================================================
    //  Fitur yang SELALU tersedia di ES 3.1+ (tidak perlu cek ekstensi)
    // =========================================================================
    if (hardext.esversion == 3 && hardext.esminor >= 1) {
        hardext.ssbo         = 1;  SHUT_LOGD("SSBO: core ES3.1+\n");
        hardext.compute_shader = 1;
        hardext.indirect_draw  = 1;
    }

    // =========================================================================
    //  Fitur yang SELALU tersedia di ES 3.2 (tidak perlu cek ekstensi)
    // =========================================================================
    if (hardext_is_gles32()) {
        hardext.geometry_shader    = 1;  SHUT_LOGD("Geometry shader: core ES3.2\n");
        hardext.tessellation_shader= 1;  SHUT_LOGD("Tessellation: core ES3.2\n");
        hardext.sample_shading     = 1;
        hardext.debug              = 1;
        hardext.blend_eq_advanced  = 1;
        hardext.astc               = 1;  SHUT_LOGD("ASTC LDR: core ES3.2\n");
        hardext.floatfbo           = 1;
        SHUT_LOGD("Backend confirmed: OpenGL ES 3.2\n");
    }

    // =========================================================================
    //  Ekstensi opsional — perlu dicek terlepas dari versi
    // =========================================================================

    // NPOT (override jika belum ES3)
    if (hardext.esversion < 3) {
        hardext.npot = 0;
        if (hasExtension(Exts, "GL_ARB_texture_non_power_of_two ") ||
            hasExtension(Exts, "GL_OES_texture_npot "))
            hardext.npot = 3;
        else if (hasExtension(Exts, "GL_IMG_texture_npot "))
            hardext.npot = 1;
        else if (hasExtension(Exts, "GL_APPLE_texture_2D_limited_npot "))
            hardext.npot = 1;
        else if (hardext.esversion >= 2)
            hardext.npot = 1;
        SHUT_LOGD("NPOT: level=%d\n", hardext.npot);
    }

    // Tekstur float/half-float (ES 3.0 tidak membuatnya wajib untuk FBO)
    if (globals4es.floattex) {
        EXT_CHECK_I("GL_OES_texture_float",          floattex);
        EXT_CHECK_I("GL_OES_texture_half_float",     halffloattex);
        if (hardext.esversion < 3) {
            EXT_CHECK_I("GL_EXT_color_buffer_float",     floatfbo);
            EXT_CHECK_I("GL_EXT_color_buffer_half_float",halffloatfbo);
        }
    }
    if (hardext_is_gles32()) {
        // float FBO adalah core di ES 3.2
        if (!hardext.floatfbo)
            hardext.floatfbo = 1;
    }

    // BGRA
    if (!globals4es.nobgra)
        EXT_CHECK_I("GL_EXT_texture_format_BGRA8888", bgra8888);

    // S3TC/DXT (populer di Adreno & tegra, dibutuhkan beberapa shader MC)
    EXT_DETECT("GL_EXT_texture_compression_s3tc",    s3tc);
    if (!hardext.s3tc)
        EXT_DETECT("GL_EXT_texture_compression_dxt1", s3tc);

    // Depth & stencil textures (jika tidak sudah di-set oleh ES3+ core)
    if (!globals4es.nodepthtex) {
        if (!hardext.depthtex)
            EXT_CHECK_I("GL_OES_depth_texture",      depthtex);
        EXT_CHECK_I("GL_OES_texture_stencil8",       stenciltex);
    }

    // RG texture
    EXT_CHECK_I("GL_EXT_texture_rg",                 rgtex);

    // Anisotropic filtering
    EXT_CHECK_I("GL_EXT_texture_filter_anisotropic", aniso);
    if (hardext.aniso) {
        gles_glGetIntegerv(0x84FF /* GL_MAX_TEXTURE_MAX_ANISOTROPY */, &hardext.aniso);
        if (gles_glGetError() != GL_NO_ERROR) hardext.aniso = 0;
        SHUT_LOGD("Anisotropic max: %d\n", hardext.aniso);
    }

    // Draw texture (ES1 only)
    EXT_DETECT("GL_OES_draw_texture",                drawtex);

    // Multi draw (batching, berguna untuk chunk MC)
    EXT_DETECT("GL_EXT_multi_draw_arrays",           multidraw);

    // Map buffer
    EXT_DETECT("GL_OES_mapbuffer",                   mapbuffer);

    // Program binary
    EXT_DETECT("GL_OES_get_program_binary",          prgbinary);
    if (!hardext.prgbinary)
        EXT_DETECT("GL_OES_get_program",             prgbinary);
    if (hardext.prgbinary) {
        gles_glGetIntegerv(0x8741 /* GL_NUM_PROGRAM_BINARY_FORMATS_OES */,
                           &hardext.prgbin_n);
        SHUT_LOGD("Program binary formats: %d\n", hardext.prgbin_n);
    }

    // ARM shader framebuffer fetch
    EXT_DETECT("GL_ARM_shader_framebuffer_fetch",    shader_fbfetch);

    // Shader LOD (texture lod di fragment)
    if (!globals4es.noshaderlod) {
        EXT_CHECK_I("GL_EXT_shader_texture_lod",     shaderlod);
        if (hardext.shaderlod) {
            if (testTextureCubeLod()) {
                hardext.cubelod = 1;
                SHUT_LOGD("textureCubeLod: tanpa suffix EXT\n");
            }
        }
    }

    // Fragment precision high (ES2 legacy)
    if (hardext.esversion == 2 && !globals4es.nohighp) {
        EXT_CHECK_I("GL_OES_fragment_precision_high", highp);
        if (!hardext.highp) {
            LOAD_GLES2(glGetShaderPrecisionFormat);
            if (gles_glGetShaderPrecisionFormat) {
                GLint range[2] = {0}, precision = 0;
                gles_glGetShaderPrecisionFormat(GL_FRAGMENT_SHADER,
                    0x8DF5 /* GL_HIGH_FLOAT */, range, &precision);
                if (!(range[0] == 0 && range[1] == 0 && precision == 0)) {
                    hardext.highp = 2;
                    SHUT_LOGD("highp float fragment: tersedia (native)\n");
                }
            }
        }
    }

    // ES1-only extensions
    if (hardext.esversion < 2) {
        if (hasExtension(Exts, "GL_OES_framebuffer_object ")) hardext.fbo = 1;
        if (hasExtension(Exts, "GL_OES_point_sprite "))       hardext.pointsprite = 1;
        if (hasExtension(Exts, "GL_OES_texture_cube_map "))   hardext.cubemap = 1;
        if (hasExtension(Exts, "GL_EXT_blend_color "))        hardext.blendcolor = 1;
        if (hasExtension(Exts, "GL_OES_blend_subtract "))     hardext.blendsub = 1;
        if (hasExtension(Exts, "GL_OES_blend_func_separate "))hardext.blendfunc = 1;
        if (hasExtension(Exts, "GL_OES_blend_equation_separate ")) hardext.blendeq = 1;
        if (hasExtension(Exts, "GL_OES_texture_mirrored_repeat ")) hardext.mirrored = 1;
        if (hasExtension(Exts, "GL_OES_element_index_uint ")) hardext.elementuint = 1;
        if (hasExtension(Exts, "GL_OES_packed_depth_stencil "))hardext.depthstencil = 1;
        if (hasExtension(Exts, "GL_OES_depth24 "))             hardext.depth24 = 1;
        if (hasExtension(Exts, "GL_OES_rgb8_rgba8 "))          hardext.rgba8 = 1;
        if (hasExtension(Exts, "GL_EXT_blend_minmax "))        hardext.blendminmax = 1;
        // AmigaOS4
        if (hasExtension(Exts, "GL_AOS4_texture_format_RGB332"))     hardext.rgb332 = 1;
        if (hasExtension(Exts, "GL_AOS4_texture_format_RGB332REV"))  hardext.rgb332rev = 1;
        if (hasExtension(Exts, "GL_AOS4_texture_format_RGBA1555REV"))hardext.rgba1555rev = 1;
        if (hasExtension(Exts, "GL_AOS4_texture_format_RGBA8888"))   hardext.rgba8888 = 1;
        if (hasExtension(Exts, "GL_AOS4_texture_format_RGBA8888REV"))hardext.rgba8888rev = 1;
    }

    // =========================================================================
    //  Batas hardware — query GL integers
    // =========================================================================
    gles_glGetIntegerv(GL_MAX_TEXTURE_SIZE, &hardext.maxsize);
    SHUT_LOGD("Max texture size: %d\n", hardext.maxsize);

    if (hardext.esversion == 1) {
        gles_glGetIntegerv(GL_MAX_TEXTURE_UNITS, &hardext.maxtex);
        gles_glGetIntegerv(GL_MAX_LIGHTS,        &hardext.maxlights);
        gles_glGetIntegerv(GL_MAX_CLIP_PLANES,   &hardext.maxplanes);
        hardext.maxteximage = hardext.maxtex;
    } else {
        gles_glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &hardext.maxteximage);
        gles_glGetIntegerv(GL_MAX_VERTEX_ATTRIBS,      &hardext.maxvattrib);
        gles_glGetIntegerv(GL_MAX_VARYING_VECTORS,     &hardext.maxvarying);
        hardext.maxtex = hardext.maxteximage;
        SHUT_LOGD("MaxVAttrib=%d MaxVaryVec=%d MaxTexImg=%d\n",
                  hardext.maxvattrib, hardext.maxvarying, hardext.maxteximage);
        // Heuristik: GPU dengan < 16 attrib biasanya tidak bisa tangani
        // lebih dari 4 texture unit secara efisien di shader generated FPE
        if (hardext.maxvattrib < 16 && hardext.maxtex > 4)
            hardext.maxtex = 4;
    }

    // MSAA max samples (ES 3.0+)
    if (hardext.esversion >= 3) {
        gles_glGetIntegerv(0x8D57 /* GL_MAX_SAMPLES */, &hardext.maxsamples);
        SHUT_LOGD("Max MSAA samples: %d\n", hardext.maxsamples);
    }

    // Draw buffers limits
    if (hardext.drawbuffers) {
        gles_glGetIntegerv(0x8CE0 /* GL_MAX_COLOR_ATTACHMENTS */,
                           &hardext.maxcolorattach);
        gles_glGetIntegerv(0x8824 /* GL_MAX_DRAW_BUFFERS */,
                           &hardext.maxdrawbuffers);
    }

    // Sanitize & cap semua batas
    {
        int hardmaxtex = hardext.maxtex;
        if (hardext.maxtex      > MAX_TEX)         hardext.maxtex      = MAX_TEX;
        if (hardext.maxteximage > MAX_TEX)          hardext.maxteximage = MAX_TEX;
        if (hardext.maxlights   > MAX_LIGHT)        hardext.maxlights   = MAX_LIGHT;
        if (hardext.maxplanes   > MAX_CLIP_PLANES)  hardext.maxplanes   = MAX_CLIP_PLANES;
        if (hardext.maxcolorattach < 1)             hardext.maxcolorattach = 1;
        if (hardext.maxcolorattach > MAX_DRAW_BUFFERS) hardext.maxcolorattach = MAX_DRAW_BUFFERS;
        if (hardext.maxdrawbuffers < 1)             hardext.maxdrawbuffers = 1;
        if (hardext.maxdrawbuffers > MAX_DRAW_BUFFERS) hardext.maxdrawbuffers = MAX_DRAW_BUFFERS;
        SHUT_LOGD("TexUnits: %d/%d (hw:%d) Lights:%d Planes:%d ColorAttach:%d DrawBuf:%d\n",
                  hardext.maxtex, hardext.maxteximage, hardmaxtex,
                  hardext.maxlights, hardext.maxplanes,
                  hardext.maxcolorattach, hardext.maxdrawbuffers);
    }

    // =========================================================================
    //  GLSL version probe — compile shader uji
    //  Urutan: 320 es → 310 es → 300 es → 120 (desktop compat)
    //
    //  Catatan penting:
    //  - GLSL ES 3.00+ wajib pakai 'in' (bukan 'attribute')
    //  - GLSL ES 1.00 / GLSL 1.20 pakai 'attribute'
    //  - Kita hanya probe versi yang masuk akal untuk backend yang ada
    // =========================================================================
    if (hardext.esversion >= 2) {
        if (hardext_is_gles32()) {
            if (testGLSL("#version 320 es", 1)) {
                hardext.glsl320es = 1;
                SHUT_LOGD("GLSL ES 3.20: TERSEDIA ✓ (target utama)\n");
            } else {
                SHUT_LOGD("GLSL ES 3.20: tidak dikompilasi (driver tidak mendukung)\n");
            }
        }
        if (hardext.esversion == 3 && hardext.esminor >= 1 && !hardext.glsl320es) {
            if (testGLSL("#version 310 es", 1)) {
                hardext.glsl310es = 1;
                SHUT_LOGD("GLSL ES 3.10: tersedia\n");
            }
        }
        if (hardext.esversion >= 3 && !hardext.glsl320es && !hardext.glsl310es) {
            if (testGLSL("#version 300 es", 1)) {
                hardext.glsl300es = 1;
                SHUT_LOGD("GLSL ES 3.00: tersedia\n");
            }
        }
        // GLSL 1.20 desktop compat (dipakai shaderconv untuk konversi)
        if (testGLSL("#version 120", 0)) {
            hardext.glsl120 = 1;
            SHUT_LOGD("GLSL 1.20: tersedia\n");
        }
    }

    // Ringkasan GLSL
    SHUT_LOGD("GLSL summary: 120=%d 300es=%d 310es=%d 320es=%d\n",
              hardext.glsl120, hardext.glsl300es,
              hardext.glsl310es, hardext.glsl320es);

    // =========================================================================
    //  EGL extension check (setelah context aktif)
    // =========================================================================
#ifndef NOEGL
    {
        const char* eglExts = egl_eglQueryString(eglDisplay, EGL_EXTENSIONS);
        if (eglExts) {
            if (hasExtension(eglExts, "EGL_KHR_gl_colorspace")) {
                hardext.srgb = 1;
                SHUT_LOGD("EGL: sRGB surface support\n");
            }
            if (hasExtension(eglExts, "EGL_KHR_image_pixmap")) {
                hardext.khr_pixmap = 1;
                SHUT_LOGD("EGL: EGLImage from Pixmap\n");
            }
            if (hasExtension(eglExts, "EGL_KHR_gl_texture_2D_image")) {
                hardext.khr_texture_2d = 1;
                SHUT_LOGD("EGL: EGLImage to Texture2D\n");
            }
            if (hasExtension(eglExts, "EGL_KHR_gl_renderbuffer_image")) {
                hardext.khr_renderbuffer = 1;
                SHUT_LOGD("EGL: EGLImage to RenderBuffer\n");
            }
        }
    }

    // ── Cleanup ─────────────────────────────────────────────────────────────
    egl_eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    egl_eglDestroySurface(eglDisplay, eglSurface);
    egl_eglDestroyContext(eglDisplay, eglContext);
    egl_eglTerminate(eglDisplay);
#endif  // !NOEGL

    SHUT_LOGD("GetHardwareExtensions selesai (ES %d.%d, vendor=%d)\n",
              hardext.esversion, hardext.esminor, hardext.vendor);
}
