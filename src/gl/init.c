// =============================================================================
//  init.c — Inisialisasi gl4es (GLES 3.2 Edition)
//
//  Perubahan utama dari versi original:
//  1. DEFAULT_ES = 3 pada Android (sebelumnya 1, kemudian 2)
//  2. LIBGL_ES menerima nilai 3 untuk ES 3.x backend
//  3. Setelah GetHardwareExtensions(), sinkronkan esversion/esminor dari
//     hardext ke globals4es, dan inisialisasi semua ES 3.x feature flags
//  4. GLSL target dipilih otomatis: 320es → 300es → 120 sesuai kapabilitas
//  5. Log backend menampilkan versi ES yang tepat (bukan hanya 1.1/2.0)
//  6. Default optimal untuk Android arm64 + MC 1.12.2 (non-core profile)
// =============================================================================

#include <stdio.h>
#ifndef _WIN32
#include <unistd.h>
#else
#include <stdio.h>
#include <direct.h>
#define getcwd(a,b) _getcwd(a,b)
#define snprintf    _snprintf
#endif
#include "../../version.h"
#include "../glx/glx_gbm.h"
#include "../glx/streaming.h"
#include "build_info.h"
#include "debug.h"
#include "loader.h"
#include "logs.h"
#include "fpe_cache.h"
#include "init.h"
#include "envvars.h"
#if defined(__EMSCRIPTEN__) || defined(__APPLE__)
#define NO_INIT_CONSTRUCTOR
#endif

void gl_init();
void gl_close();

#ifdef GL4ES_COMPILE_FOR_USE_IN_SHARED_LIB
#ifdef AMIGAOS4
void agl_reset_internals();
#endif
void fpe_shader_reset_internals();
#endif

globals4es_t globals4es = {0};

// ── FastMath untuk Cortex-A8 (Pandora/CHIP) ──────────────────────────────────
#if defined(PANDORA) || defined(CHIP) || (defined(GOA_CLONE) && !defined(__aarch64__))
static void fast_math() {
    int v = 0;
    __asm__ __volatile__ (
        "vmrs %0, fpscr\n"
        "orr  %0, #((1<<25)|(1<<24))\n"   // default NaN, flush-to-zero
        "vmsr fpscr, %0\n"
        : "=&r"(v));
}
#endif

// =============================================================================
//  DEFAULT_ES — Backend default ketika LIBGL_ES tidak di-set
//
//  Perubahan dari original:
//  - Android: 3 (ES 3.x) — sebelumnya 1, lalu 2
//    Justifikasi: Seluruh Android arm64 modern (API 26+) mendukung ES 3.2.
//    Jika hardware tidak mendukung, hardext akan fallback ke 3.0 atau 2.0,
//    dan kita sinkronkan kembali globals4es.es setelah GetHardwareExtensions.
//  - Pandora: 1 (ES 1.1) — tetap, hardware Pandora hanya punya SGX530
//  - Lainnya: 2 (ES 2.0) — default aman untuk non-Android non-Pandora
// =============================================================================
#ifndef DEFAULT_ES
#  if defined(ANDROID)
#    define DEFAULT_ES 3
#  elif defined(PANDORA)
#    define DEFAULT_ES 1
#  else
#    define DEFAULT_ES 2
#  endif
#endif

void load_libs();
void glx_init();

static int inited = 0;

// ── Helper: format string versi backend ES ───────────────────────────────────
static const char* esVersionStr(int major, int minor) {
    if (major == 1) return "1.1";
    if (major == 2) return "2.0";
    if (major == 3) {
        if (minor == 2) return "3.2";
        if (minor == 1) return "3.1";
        return "3.0";
    }
    return "?";
}

// ── set_getmainfbsize / set_getprocaddress ────────────────────────────────────
EXPORT
void set_getmainfbsize(void (APIENTRY_GL4ES *new_getMainFBSize)(int* w, int* h)) {
    gl4es_getMainFBSize = (void*)new_getMainFBSize;
}

EXPORT
void set_getprocaddress(void *(APIENTRY_GL4ES *new_proc_address)(const char *)) {
    gles_getProcAddress = new_proc_address;
}

// =============================================================================
//  initialize_gl4es — Entry point utama, dipanggil sekali via constructor
// =============================================================================
#ifdef NO_INIT_CONSTRUCTOR
EXPORT
#else
#if defined(_WIN32) || defined(__CYGWIN__)
#define BUILD_WINDOWS_DLL
static unsigned char dll_inited;
EXPORT
#endif
#if !defined(_MSC_VER) || defined(__clang__)
__attribute__((constructor(101)))
#endif
#endif
void initialize_gl4es() {
#ifdef BUILD_WINDOWS_DLL
    if (!dll_inited) {
        LOGE("Windows ES emulator cannot be initialized from DllMain\n");
        return;
    }
#endif
    if (inited++) return;

    // ── Reset seluruh struct ke nol, lalu set default ─────────────────────
    memset(&globals4es, 0, sizeof(globals4es));
    globals4es.mergelist = 1;
    globals4es.queries   = 1;
    globals4es.beginend  = 1;

    // ── deepbind ──────────────────────────────────────────────────────────
#ifdef PYRA
    GetEnvVarInt("LIBGL_DEEPBIND", &globals4es.deepbind, 0);
#else
    GetEnvVarInt("LIBGL_DEEPBIND", &globals4es.deepbind, 1);
#endif

    // ── Banner ────────────────────────────────────────────────────────────
#ifdef GL4ES_COMPILE_FOR_USE_IN_SHARED_LIB
    GetEnvVarInt("LIBGL_NOBANNER", &globals4es.nobanner, 1);
#else
    globals4es.nobanner = IsEnvVarTrue("LIBGL_NOBANNER");
#endif
    SHUT_LOGD("Initialising gl4es (GLES 3.2 Edition)\n");
    if (!globals4es.nobanner) print_build_infos();

    // ── Makro env helper ──────────────────────────────────────────────────
    #define env(name, global, message)      \
        if (IsEnvVarTrue(#name)) {          \
            SHUT_LOGD(message "\n");        \
            global = true;                  \
        }

    env(LIBGL_XREFRESH,    globals4es.xrefresh,    "xrefresh will be called on cleanup");
    env(LIBGL_STACKTRACE,  globals4es.stacktrace,  "stacktrace will be printed on crash");

    // ── Framebuffer mode ──────────────────────────────────────────────────
    const int LIBGL_FB_ENV_VAR =
#ifndef LIBGL_FB
        ReturnEnvVarInt("LIBGL_FB")
#else
        ReturnEnvVarIntDef("LIBGL_FB", LIBGL_FB)
#endif
    ;
    switch (LIBGL_FB_ENV_VAR) {
        case 1:
            SHUT_LOGD("framebuffer output enabled\n");
            globals4es.usefb = 1;
            break;
        case 2:
            SHUT_LOGD("using framebuffer + fbo\n");
            globals4es.usefb  = 1;
            globals4es.usefbo = 1;
            break;
#ifndef NOX11
        case 3:
            SHUT_LOGD("using pbuffer\n");
            globals4es.usefb       = 0;
            globals4es.usepbuffer  = 1;
            break;
#endif
        case 4:
#ifdef NO_GBM
            SHUT_LOGD("GBM not built, cannot use\n");
#else
            SHUT_LOGD("using GBM\n");
            globals4es.usefb  = 0;
            globals4es.usegbm = 1;
#endif
            break;
        default:
            break;
    }

    env(LIBGL_BLITFB0,  globals4es.blitfb0,  "Blit to FB 0 forces SwapBuffer");
    env(LIBGL_FPS,      globals4es.showfps,   "fps counter enabled");
#if defined(USE_FBIO) || defined(PYRA)
    env(LIBGL_VSYNC,    globals4es.vsync,     "vsync enabled");
#endif
#ifdef PANDORA
    if (GetEnvVarFloat("LIBGL_GAMMA", &globals4es.gamma, 0.0f))
        SHUT_LOGD("Set gamma to %.2f\n", globals4es.gamma);
#endif
    env(LIBGL_NOBGRA,     globals4es.nobgra,     "Ignore BGRA texture capability");
    env(LIBGL_NOTEXRECT,  globals4es.notexrect,  "Don't export Tex Rectangle extension");
    if (globals4es.usefbo)
        env(LIBGL_FBONOALPHA, globals4es.fbo_noalpha, "Main FBO has no alpha channel");

    // =========================================================================
    //  LIBGL_ES — Pilih backend ES
    //
    //  Nilai yang valid:
    //    1 → ES 1.1 (hanya untuk Pandora/hardware lama)
    //    2 → ES 2.0
    //    3 → ES 3.x (target kita: 3.2 dengan fallback ke 3.0/2.0)
    //    0 / default → DEFAULT_ES (3 pada Android)
    //
    //  Catatan: nilai aktual yang digunakan bisa berubah setelah
    //  GetHardwareExtensions() jika hardware tidak mendukung versi ini.
    // =========================================================================
    globals4es.es = ReturnEnvVarInt("LIBGL_ES");
    switch (globals4es.es) {
        case 1:
        case 2:
        case 3:
            break;
        default:
            globals4es.es = DEFAULT_ES;
            break;
    }

    // ── Versi OpenGL yang di-expose ke aplikasi ───────────────────────────
    // Minecraft 1.12.2 menggunakan fixed-function pipeline via LWJGL GL11/GL15
    // dan shader via GL20. Versi OpenGL 2.1 (gl=21) adalah yang paling cocok.
    // Untuk ES 3.x backend, kita tetap expose GL 2.1 (bukan 3.x) karena
    // MC 1.12.2 adalah non-core profile dan bergantung pada fixed-function.
    globals4es.gl = ReturnEnvVarInt("LIBGL_GL");
    switch (globals4es.gl) {
        case 10: case 11: case 12: case 13: case 14: case 15:
        case 20: case 21:
        case 30: case 31: case 32: case 33:
        case 40: case 41: case 42: case 43: case 44: case 45:
            break;
        default:
            // ES 3.x backend → expose GL 2.1 (optimal untuk MC 1.12.2)
            // ES 1.x backend → expose GL 1.5
            globals4es.gl = (globals4es.es == 1) ? 15 : 21;
            break;
    }

    SHUT_LOGD("Requested ES backend: %s | Exposing GL %d.%d\n",
              esVersionStr(globals4es.es, 0),
              globals4es.gl / 10, globals4es.gl % 10);

    env(LIBGL_NODEPTHTEX, globals4es.nodepthtex, "Disable Depth Textures");

    // ── DRM card untuk GBM ────────────────────────────────────────────────
    const char* env_drmcard = GetEnvVar("LIBGL_DRMCARD");
    if (env_drmcard) {
#ifdef NO_GBM
        SHUT_LOGD("Warning: GBM not compiled, LIBGL_DRMCARD ignored\n");
#else
        strncpy(globals4es.drmcard, env_drmcard, 50);
    } else {
        strcpy(globals4es.drmcard, "/dev/dri/card0");
#endif
    }

#if !defined(__EMSCRIPTEN__) && !defined(__APPLE__)
    load_libs();
#endif

#if (defined(NOEGL) && !defined(ANDROID) && !defined(__APPLE__)) || defined(__EMSCRIPTEN__)
    int gl4es_notest = !gles_getProcAddress;
#else
    int gl4es_notest = IsEnvVarTrue("LIBGL_NOTEST");
#endif

    env(LIBGL_NOHIGHP, globals4es.nohighp, "No HIGHP in fragment shader");

    globals4es.floattex = ReturnEnvVarIntDef("LIBGL_FLOAT", 1);
    switch (globals4es.floattex) {
        case 0: SHUT_LOGD("Float/Half-Float texture support disabled\n"); break;
        case 2: SHUT_LOGD("Float/Half-Float texture support forced\n");   break;
        default: globals4es.floattex = 1; break;
    }

    // =========================================================================
    //  GetHardwareExtensions — Probe hardware, hasilnya di hardext.*
    //  SETELAH ini kita sinkronkan globals4es dengan hasil probe.
    // =========================================================================
    GetHardwareExtensions(gl4es_notest);

    // ── Sinkronisasi: gunakan versi yang sesungguhnya dari hardext ─────────
    // Contoh: kita minta ES 3.2 (globals4es.es=3), tapi hardware hanya punya
    // ES 3.0 (hardext.esversion=3, hardext.esminor=0). Maka kita update
    // globals4es agar subsystem lain tahu versi yang benar-benar aktif.
    if (hardext.esversion > 0) {
        if (hardext.esversion < globals4es.es) {
            SHUT_LOGD("Backend downgraded: requested ES %s, got ES %s\n",
                      esVersionStr(globals4es.es, 0),
                      esVersionStr(hardext.esversion, hardext.esminor));
            globals4es.es = hardext.esversion;
        }
        globals4es.esminor = hardext.esminor;
    }

    SHUT_LOGD("Active ES backend: %s\n",
              esVersionStr(globals4es.es, globals4es.esminor));

    // =========================================================================
    //  Inisialisasi ES 3.x Feature Flags
    //
    //  Logika: hardware capability (hardext.*) AND tidak di-disable user (env)
    //  Setelah blok ini, subsystem lain cek globals4es.use_* bukan hardext.*
    // =========================================================================
    if (globals4es.es >= 3) {
        // Native VAO — menggantikan VAO cache software untuk MC 1.12.2
        // Default ON jika tersedia. User bisa matikan via LIBGL_NOVAOCACHE=1
        // tapi untuk ES3 itu akan tetap pakai native jika tersedia.
        globals4es.use_native_vao = hardext.vao_native &&
                                    !globals4es.novaocache;
        if (globals4es.use_native_vao)
            SHUT_LOGD("ES3: Native VAO enabled (GLES 3.0 core)\n");

        // PBO — async texture upload/download, mengurangi CPU stall
        // Sangat penting untuk MC 1.12.2 yang sering glTexImage2D chunk
        globals4es.use_pbo = hardext.pbo;
        if (globals4es.use_pbo)
            SHUT_LOGD("ES3: PBO enabled (async texture upload/download)\n");

        // Instanced draw — untuk MC batch rendering
        globals4es.use_instanced = hardext.instanced;

        // UBO — untuk FPE shader (uniform block alih-alih individual uniform)
        globals4es.use_ubo = hardext.ubo;
        if (globals4es.use_ubo)
            SHUT_LOGD("ES3: UBO enabled\n");

        // Fence sync — untuk sinkronisasi CPU/GPU
        globals4es.use_sync = hardext.sync_obj;

        // glMapBufferRange — lebih efisien dari OES mapbuffer untuk VBO update
        globals4es.use_mapbuffer_range = (globals4es.es >= 3);
        if (globals4es.use_mapbuffer_range)
            SHUT_LOGD("ES3: glMapBufferRange enabled\n");
    }

    // ── GLSL target selection ─────────────────────────────────────────────
    // Urutan prioritas: 320es → 310es → 300es → 120 (desktop compat)
    // Minecraft 1.12.2 tidak memerlukan GLSL ES 3.20 secara eksplisit,
    // tapi menggunakan backend 320es memberi shader generator path yang
    // lebih bersih (texture() vs texture2D(), in/out vs attribute/varying)
    if (hardext.glsl320es) {
        globals4es.glsl_target = 320;
        SHUT_LOGD("GLSL target: ES 3.20 (optimal)\n");
    } else if (hardext.glsl310es) {
        globals4es.glsl_target = 310;
        SHUT_LOGD("GLSL target: ES 3.10\n");
    } else if (hardext.glsl300es) {
        globals4es.glsl_target = 300;
        SHUT_LOGD("GLSL target: ES 3.00\n");
    } else {
        globals4es.glsl_target = 120;
        SHUT_LOGD("GLSL target: GLSL 1.20 (fallback)\n");
    }

    // Izinkan override manual via env (untuk debugging)
    {
        int env_glsl = ReturnEnvVarInt("LIBGL_GLSL_TARGET");
        if (env_glsl == 120 || env_glsl == 300 || env_glsl == 310 || env_glsl == 320) {
            globals4es.glsl_target = env_glsl;
            SHUT_LOGD("GLSL target overridden via LIBGL_GLSL_TARGET: %d\n", env_glsl);
        }
    }

    // ── GBM fallback ──────────────────────────────────────────────────────
#if !defined(NO_LOADER) && !defined(NO_GBM)
    if (globals4es.usegbm) LoadGBMFunctions();
    if (globals4es.usegbm && !(gbm && drm)) {
        SHUT_LOGD("Cannot use GBM (driver/device not found), disabling\n");
        globals4es.usegbm = 0;
    }
#else
    globals4es.usegbm = 0;
#endif

#if !defined(NOX11)
    glx_init();
#endif

    gl_init();

#ifdef GL4ES_COMPILE_FOR_USE_IN_SHARED_LIB
    fpe_shader_reset_internals();
#ifdef AMIGAOS4
    agl_reset_internals();
#endif
#endif

    env(LIBGL_RECYCLEFBO, globals4es.recyclefbo, "Recycling of FBO enabled");

    // ── MipMap ───────────────────────────────────────────────────────────
    globals4es.automipmap = ReturnEnvVarInt("LIBGL_MIPMAP");
    switch (globals4es.automipmap) {
        case 1: SHUT_LOGD("AutoMipMap forced\n");                                break;
        case 2: SHUT_LOGD("Guess AutoMipMap\n");                                 break;
        case 3: SHUT_LOGD("Ignore MipMap\n");                                    break;
        case 4: SHUT_LOGD("Ignore AutoMipMap on non-squared textures\n");        break;
        case 5: SHUT_LOGD("Calculate sub-mipmap if some are missing\n");         break;
        default: globals4es.automipmap = 0; break;
    }

    if (IsEnvVarTrue("LIBGL_TEXCOPY")) {
        globals4es.texcopydata = 1;
        SHUT_LOGD("Texture copy enabled\n");
    }

    // ── Texture shrink ────────────────────────────────────────────────────
    globals4es.texshrink = ReturnEnvVarInt("LIBGL_SHRINK");
    switch (globals4es.texshrink) {
        case 1:  SHUT_LOGD("Texture shrink mode 1 (all /2)\n");                  break;
        case 2:  SHUT_LOGD("Texture shrink mode 2 (>512 /2)\n");                 break;
        case 3:  SHUT_LOGD("Texture shrink mode 3 (>256 /2)\n");                 break;
        case 4:  SHUT_LOGD("Texture shrink mode 4 (>256 /2, >=1024 /4)\n");      break;
        case 5:  SHUT_LOGD("Texture shrink mode 5 (>256 → 256)\n");              break;
        case 6:  SHUT_LOGD("Texture shrink mode 6 (>128 /2, >=512 → 256)\n");    break;
        case 7:  SHUT_LOGD("Texture shrink mode 7 (>512 /2)\n");                 break;
        case 8:  SHUT_LOGD("Texture shrink mode 8 (>2048 → 2048, adv. 8192)\n"); break;
        case 9:  SHUT_LOGD("Texture shrink mode 9 (>4096 quad, >512 /2)\n");     break;
        case 10: SHUT_LOGD("Texture shrink mode 10 (>2048 quad, >512 /2)\n");    break;
        case 11: SHUT_LOGD("Texture shrink mode 11 (adv. max*2, shrink >max)\n");break;
        default: globals4es.texshrink = 0; break;
    }

    env(LIBGL_TEXDUMP,    globals4es.texdump,    "Texture dump enabled");
    env(LIBGL_ALPHAHACK,  globals4es.alphahack,  "Alpha Hack enabled");

#ifdef TEXSTREAM
    switch (ReturnEnvVarInt("LIBGL_STREAM")) {
        case 1:
            globals4es.texstream = InitStreamingCache();
            SHUT_LOGD("Streaming texture %s\n", globals4es.texstream ? "enabled" : "not available");
            break;
        case 2:
            globals4es.texstream = InitStreamingCache() ? 2 : 0;
            SHUT_LOGD("Streaming texture %s\n", globals4es.texstream ? "forced" : "not available");
            break;
        default:
            break;
    }
#endif

    env(LIBGL_NOLUMALPHA, globals4es.nolumalpha, "GL_LUMINANCE_ALPHA disabled");
    env(LIBGL_BLENDHACK,  globals4es.blendhack,  "Blend SRC_ALPHA,ONE → ONE,ONE");
    env(LIBGL_BLENDCOLOR, globals4es.blendcolor, "Export faked glBlendColor");
    env(LIBGL_NOERROR,    globals4es.noerror,    "glGetError() always GL_NO_ERROR");

    globals4es.silentstub = 1;
    if (IsEnvVarInt("LIBGL_SILENTSTUB", 0)) {
        globals4es.silentstub = 0;
        SHUT_LOGD("Stub/non-present functions printed\n");
    }

    env(LIBGL_VABGRA, globals4es.vabgra, "Export GL_ARB_vertex_array_bgra");

    // ── Version string ────────────────────────────────────────────────────
    const char* env_version = GetEnvVar("LIBGL_VERSION");
    if (env_version) {
        SHUT_LOGD("Override GL version string: \"%s\"\n", env_version);
        snprintf(globals4es.version, 49, "%s gl4es wrapper %d.%d.%d",
                 env_version, MAJOR, MINOR, REVISION);
    } else {
        snprintf(globals4es.version, 49, "%d.%d gl4es wrapper %d.%d.%d",
                 globals4es.gl / 10, globals4es.gl % 10,
                 MAJOR, MINOR, REVISION);
    }
    SHUT_LOGD("Exposing OpenGL %d.%d (%s)\n",
              globals4es.gl / 10, globals4es.gl % 10, globals4es.version);

    // ── sRGB ──────────────────────────────────────────────────────────────
    if (hardext.srgb && IsEnvVarTrue("LIBGL_SRGB")) {
        globals4es.glx_surface_srgb = 2;
        SHUT_LOGD("sRGB surface enabled\n");
    }

    // ── FastMath ──────────────────────────────────────────────────────────
    if (IsEnvVarTrue("LIBGL_FASTMATH")) {
#if defined(PANDORA) || defined(CHIP) || (defined(GOA_CLONE) && !defined(__aarch64__))
        SHUT_LOGD("Enable FastMath for cortex-a8\n");
        fast_math();
#else
        SHUT_LOGD("FastMath: platform tidak mendukung (arm64 sudah -Ofast)\n");
#endif
    }

    // ── NPOT ──────────────────────────────────────────────────────────────
    switch (hardext.npot) {
        case 0: globals4es.npot = 0; break;
        case 1:
        case 2: globals4es.npot = 1; break;
        case 3: globals4es.npot = 2; break;
    }
    switch (ReturnEnvVarInt("LIBGL_NPOT")) {
        case 1:
            if (globals4es.npot < 1) { globals4es.npot = 1; SHUT_LOGD("Expose limited NPOT\n"); }
            break;
        case 2:
            if (globals4es.npot < 2) { globals4es.npot = 2; SHUT_LOGD("Expose GL_ARB_texture_non_power_of_two\n"); }
            break;
    }

    // ── GL Queries ────────────────────────────────────────────────────────
    if (IsEnvVarFalse("LIBGL_GLQUERIES")) {
        globals4es.queries = 0;
        SHUT_LOGD("Fake glQueries disabled\n");
    }
    if (IsEnvVarTrue("LIBGL_NODOWNSAMPLING")) {
        globals4es.nodownsampling = 1;
        SHUT_LOGD("No downsampling of DXTc textures\n");
    }

    env(LIBGL_NOTEXMAT,    globals4es.texmat,        "Don't handle Texture Matrix internally");
    env(LIBGL_NOVAOCACHE,  globals4es.novaocache,    "Don't use VAO cache");

    // Jika user nonaktifkan VAO cache, nonaktifkan juga native VAO
    if (globals4es.novaocache)
        globals4es.use_native_vao = 0;

    if (IsEnvVarTrue("LIBGL_NOINTOVLHACK")) {
        globals4es.nointovlhack = 1;
        SHUT_LOGD("No int overload hack in shader converter\n");
    }
    if (IsEnvVarTrue("LIBGL_NOSHADERLOD")) {
        globals4es.noshaderlod = 1;
        SHUT_LOGD("GL_EXT_shader_texture_lod disabled\n");
        hardext.shaderlod = 0;
    }

    // ── BeginEnd merge ────────────────────────────────────────────────────
    {
        int env_begin_end;
        if (GetEnvVarInt("LIBGL_BEGINEND", &env_begin_end, 0)) {
            switch (env_begin_end) {
                case 0:
                    globals4es.beginend  = 0;
                    globals4es.mergelist = 0;
                    SHUT_LOGD("Don't merge glBegin/glEnd blocks\n");
                    break;
                case 1:
                case 2:
                    globals4es.beginend = 1;
                    SHUT_LOGD("Merge subsequent glBegin/glEnd blocks\n");
                    break;
            }
        }
    }

    // ── 16-bit texture avoidance ──────────────────────────────────────────
    // Default: hindari 16bit pada ES 3.x (hardware modern punya format lengkap)
    //          hindari 16bit juga pada non-ImgTec untuk kualitas terbaik
    {
        int default_avoid16 = (hardext.vendor & VEND_IMGTEC) ? 0 : 1;
        // Pada ES 3.x, driver punya format RGBA8 native, 16bit tidak efisien
        if (globals4es.es >= 3) default_avoid16 = 1;
        if (GetEnvVarBool("LIBGL_AVOID16BITS", &globals4es.avoid16bits, default_avoid16)) {
            SHUT_LOGD("Avoid 16bits textures: %s\n", globals4es.avoid16bits ? "yes" : "no");
        }
    }

    if (GetEnvVarInt("LIBGL_AVOID24BITS", &globals4es.avoid24bits, 0)) {
        switch (globals4es.avoid24bits) {
            case 0: SHUT_LOGD("Don't avoid 24bits textures\n");    break;
            case 1: globals4es.avoid24bits = 2;
                    SHUT_LOGD("Avoid 24bits textures\n");           break;
            default: globals4es.avoid24bits = 0;                    break;
        }
    }

    env(LIBGL_FORCE16BITS,    globals4es.force16bits,    "Force 16bits textures");
    env(LIBGL_POTFRAMEBUFFER, globals4es.potframebuffer, "Force POT framebuffers");

    // ── NPOT force ────────────────────────────────────────────────────────
    {
        int env_forcenpot = ReturnEnvVarIntDef("LIBGL_FORCENPOT", 0);
        if (env_forcenpot == 0 && globals4es.es >= 3 && hardext.npot == 3) {
            // ES 3.x dengan full NPOT: tidak perlu forcing
        } else if (env_forcenpot != 0 || (globals4es.es == 2 && hardext.npot <= 2)) {
            if (hardext.npot == 3) {
                SHUT_LOGD("NPOT handled in hardware (full)\n");
            } else if (hardext.npot == 1) {
                globals4es.forcenpot = 1;
                SHUT_LOGD("Forcing NPOT by disabling MIPMAP for NPOT textures\n");
            } else {
                SHUT_LOGD("WARNING: No NPOT support, force has no effect\n");
            }
        }
    }

    // ── Batch draw ────────────────────────────────────────────────────────
#if defined(GL4ES_COMPILE_FOR_USE_IN_SHARED_LIB) && defined(AMIGAOS4)
    globals4es.maxbatch = 40;
#else
    globals4es.maxbatch = 0;
#endif
    globals4es.minbatch = 0;
    {
        int tmp = 0, tmp2 = 0;
        switch (GetEnvVarFmt("LIBGL_BATCH", "%d-%d", &tmp, &tmp2)) {
            case 2:
                globals4es.maxbatch = tmp2;
                globals4es.minbatch = tmp;
                if (globals4es.minbatch > globals4es.maxbatch) {
                    globals4es.maxbatch = tmp;
                    globals4es.minbatch = tmp2;
                }
                break;
            case 1:
                globals4es.maxbatch = 10 * 10 * tmp;
                globals4es.minbatch = 0;
                break;
        }
    }
    if (globals4es.maxbatch == 0)
        SHUT_LOGD("Batch draw disabled\n");
    else
        SHUT_LOGD("Batch draw: %d–%d vertices\n", globals4es.minbatch, globals4es.maxbatch);

    // ── VBO ───────────────────────────────────────────────────────────────
    if (globals4es.es == 1) {
        globals4es.usevbo = 0;  // VBO di ES 1.1 terlalu berantakan
    } else {
        globals4es.usevbo = ReturnEnvVarIntDef("LIBGL_USEVBO", 1);
        switch (globals4es.usevbo) {
            case 0: SHUT_LOGD("VBO disabled\n");                              break;
            case 1: SHUT_LOGD("VBO enabled\n");                               break;
            case 2: SHUT_LOGD("VBO enabled (+glLockArrays)\n");               break;
            case 3: SHUT_LOGD("VBO enabled (idtech3 glLockArrays mode)\n");   break;
            default: globals4es.usevbo = 1;                                   break;
        }
    }

    // ── FBO workarounds ───────────────────────────────────────────────────
    // fbomakecurrent: bind/unbind FBO saat glXMakeCurrent
    // Perlu pada Mali (ARM) dan jika usefb aktif.
    // Pada ES 3.x + Adreno (Qualcomm) biasanya tidak diperlukan.
    globals4es.fbomakecurrent = 0;
    if ((hardext.vendor & VEND_ARM) || globals4es.usefb)
        globals4es.fbomakecurrent = 1;
    // Qualcomm Adreno pada ES 3.x: workaround ini lebih sering menyebabkan
    // masalah daripada membantu — disable secara default
    if ((hardext.vendor & VEND_QUALCOMM) && globals4es.es >= 3)
        globals4es.fbomakecurrent = 0;

    switch (ReturnEnvVarIntDef("LIBGL_FBOMAKECURRENT", -1)) {
        case 0:
            globals4es.fbomakecurrent = 0;
            SHUT_LOGD("FBO glXMakeCurrent workaround disabled\n");
            break;
        case 1:
            globals4es.fbomakecurrent = 1;
            break;
    }
    if (globals4es.fbomakecurrent)
        SHUT_LOGD("FBO glXMakeCurrent workaround enabled\n");

    // fbounbind: unbind FBO jika texture yang sedang digunakan juga terikat
    // Dibutuhkan pada Mali dan PVR. Pada Adreno ES 3.x tidak diperlukan.
    globals4es.fbounbind = 0;
    if ((hardext.vendor & VEND_ARM) || (hardext.vendor & VEND_IMGTEC))
        globals4es.fbounbind = 1;
    if ((hardext.vendor & VEND_QUALCOMM) && globals4es.es >= 3)
        globals4es.fbounbind = 0;

    switch (ReturnEnvVarIntDef("LIBGL_FBOUNBIND", -1)) {
        case 0: globals4es.fbounbind = 0; SHUT_LOGD("FBO unbind workaround disabled\n"); break;
        case 1: globals4es.fbounbind = 1; break;
    }
    if (globals4es.fbounbind)
        SHUT_LOGD("FBO unbind workaround enabled\n");

    globals4es.fboforcetex = 1;
    GetEnvVarInt("LIBGL_FBOFORCETEX", &globals4es.fboforcetex, globals4es.fboforcetex);
    if (globals4es.fboforcetex)
        SHUT_LOGD("Force texture for FBO Color0 attachment\n");

    globals4es.blitfullscreen = 1;
    GetEnvVarInt("LIBGL_BLITFULLSCREEN", &globals4es.blitfullscreen, globals4es.blitfullscreen);
    if (globals4es.blitfullscreen)
        SHUT_LOGD("Fullscreen blit on default FBO triggers SwapBuffers\n");

    env(LIBGL_COMMENTS,    globals4es.comments,    "Keep comments in converted shaders");
    env(LIBGL_NOARBPROGRAM,globals4es.noarbprogram,"ARB Program extensions disabled");

    // ── Default wrap mode ─────────────────────────────────────────────────
    // ES 3.x dengan full NPOT: default GL_REPEAT (lebih akurat untuk MC)
    // ES 2.x dengan limited NPOT: default GL_CLAMP_TO_EDGE (aman)
    globals4es.defaultwrap = (hardext.npot == 3) ? 0 : 1;
    if (GetEnvVarInt("LIBGL_DEFAULTWRAP", &globals4es.defaultwrap,
                     (hardext.npot == 3) ? 0 : 1)) {
        switch (globals4es.defaultwrap) {
            case 0: SHUT_LOGD("Default wrap: GL_REPEAT\n");                  break;
            case 1: SHUT_LOGD("Default wrap: GL_CLAMP_TO_EDGE\n");           break;
            case 2:
            default:
                globals4es.defaultwrap = 2;
                SHUT_LOGD("Default wrap: GL_CLAMP_TO_EDGE (enforced)\n");
                break;
        }
    }

    GetEnvVarBool("LIBGL_NOTEXARRAY", &globals4es.notexarray, 0);
    if (globals4es.notexarray)
        SHUT_LOGD("No Texture Array in shaders\n");

    env(LIBGL_LOGSHADERERROR, globals4es.logshader,    "Log shader compile errors");
    env(LIBGL_SHADERNOGLES,   globals4es.shadernogles, "Remove GLES part in shaders");
    env(LIBGL_NOES2COMPAT,    globals4es.noes2,        "Don't expose ES2 profile extension");
    env(LIBGL_NORMALIZE,      globals4es.normalize,    "Force normal normalization in FPE");

    // ── dbgshaderconv ─────────────────────────────────────────────────────
    globals4es.dbgshaderconv = ReturnEnvVarIntDef("LIBGL_DBGSHADERCONV", 0);
    if (globals4es.dbgshaderconv) {
        if (globals4es.dbgshaderconv == 1)     globals4es.dbgshaderconv = 15;
        if (!(globals4es.dbgshaderconv & 3))   globals4es.dbgshaderconv |= 3;
        if (!(globals4es.dbgshaderconv & 12))  globals4es.dbgshaderconv |= 12;
        SHUT_LOGD_NOPREFIX("Log shaders:");
        if (globals4es.dbgshaderconv & 4)  SHUT_LOGD_NOPREFIX(" Before");
        if (globals4es.dbgshaderconv & 8)  SHUT_LOGD_NOPREFIX(" After");
        if (globals4es.dbgshaderconv & 1)  SHUT_LOGD_NOPREFIX(" Vertex");
        if (globals4es.dbgshaderconv & 2)  SHUT_LOGD_NOPREFIX(" Fragment");
        SHUT_LOGD_NOPREFIX("\n");
    }

    env(LIBGL_NOCLEAN, globals4es.noclean, "Don't clean context on destroy");

    // ── EGL/GLX recycle ───────────────────────────────────────────────────
    globals4es.glxrecycle = 1;
#ifndef NOEGL
    if (globals4es.usepbuffer || globals4es.usefb)
        globals4es.glxrecycle = 0;

    int env_glxrecycle = ReturnEnvVarIntDef("LIBGL_GLXRECYCLE", -1);
    if (globals4es.glxrecycle && env_glxrecycle == 0
        && !(globals4es.usepbuffer || globals4es.usefb)) {
        globals4es.glxrecycle = 0;
        SHUT_LOGD("glX: Will NOT recycle EGL Surface\n");
    }
    if (env_glxrecycle == 1) globals4es.glxrecycle = 1;
    if (globals4es.glxrecycle)
        SHUT_LOGD("glX: Will recycle EGL Surface\n");

    env(LIBGL_GLXNATIVE, globals4es.glxnative, "Don't filter GLXConfig with NATIVE_TYPE");
#endif

    // ── CWD log ───────────────────────────────────────────────────────────
    {
        char cwd[4096];
        if (getcwd(cwd, sizeof(cwd)) != NULL)
            SHUT_LOGD("Current folder: %s\n", cwd);

        // ── Shader blend (ARM_shader_framebuffer_fetch) ──────────────────
        if (hardext.shader_fbfetch)
            env(LIBGL_SHADERBLEND, globals4es.shaderblend, "Blend in shaders (ARM fbfetch)");

        // ── PSA (Pre-compiled Shader Archive) ────────────────────────────
        if (hardext.prgbin_n > 0 && !globals4es.notexarray) {
            env(LIBGL_NOPSA, globals4es.nopsa, "Don't use Pre-compiled Shader Archive");
            if (globals4es.nopsa == 0) {
                cwd[0] = '\0';
                const char* custom_psa = GetEnvVar("LIBGL_PSA_FOLDER");
#ifdef __linux__
                const char* home = GetEnvVar("HOME");
                if (custom_psa)
                    strcpy(cwd, custom_psa);
                else if (home)
                    strcpy(cwd, home);
                if (strlen(cwd) && cwd[strlen(cwd)-1] != '/')
                    strcat(cwd, "/");
#elif defined(AMIGAOS4)
                if (custom_psa)
                    strcpy(cwd, custom_psa);
                else
                    strcpy(cwd, "PROGDIR:");
#endif
                if (strlen(cwd)) {
                    strcat(cwd, ".gl4es.psa");
                    fpe_InitPSA(cwd);
                    fpe_readPSA();
                }
            }
        } else {
            SHUT_LOGD("PSA not used (prgbin_n=%d, notexarray=%d)\n",
                      hardext.prgbin_n, globals4es.notexarray);
        }
    }

    env(LIBGL_SKIPTEXCOPIES, globals4es.skiptexcopies, "Texture Copies skipped");

    if (GetEnvVarFloat("LIBGL_FB_TEX_SCALE", &globals4es.fbtexscale, 0.0f))
        SHUT_LOGD("Framebuffer Textures scaled by %.2f\n", globals4es.fbtexscale);

    // ── Ringkasan akhir ───────────────────────────────────────────────────
    SHUT_LOGD("gl4es init done: ES %s | GL %d.%d | GLSL %d | "
              "VAO=%d PBO=%d Instanced=%d UBO=%d\n",
              esVersionStr(globals4es.es, globals4es.esminor),
              globals4es.gl / 10, globals4es.gl % 10,
              globals4es.glsl_target,
              globals4es.use_native_vao,
              globals4es.use_pbo,
              globals4es.use_instanced,
              globals4es.use_ubo);
}

// =============================================================================
//  close_gl4es — Cleanup saat library di-unload
// =============================================================================
#ifndef NOX11
void FreeFBVisual();
#endif

#ifdef NO_INIT_CONSTRUCTOR
EXPORT
#else
#ifdef BUILD_WINDOWS_DLL
EXPORT
#endif
#if !defined(_MSC_VER) || defined(__clang__)
__attribute__((destructor))
#endif
#endif
void close_gl4es() {
#ifdef GL4ES_COMPILE_FOR_USE_IN_SHARED_LIB
    SHUT_LOGD("Shutdown requested\n");
    if (--inited) return;
#endif
    SHUT_LOGD("Shutting down gl4es\n");
#ifndef NOX11
    FreeFBVisual();
#endif
    gl_close();
    fpe_writePSA();
    fpe_FreePSA();
#if defined(GL4ES_COMPILE_FOR_USE_IN_SHARED_LIB) && defined(AMIGAOS4)
    os4CloseLib();
#endif
}

#ifdef BUILD_WINDOWS_DLL
#if !defined(_MSC_VER) || defined(__clang__)
__attribute__((constructor(103)))
#endif
void dll_init_done() { dll_inited = 1; }
#endif

#if defined(_MSC_VER) && !defined(NO_INIT_CONSTRUCTOR) && !defined(__clang__)
#pragma const_seg(".CRT$XCU")
void (*const gl4es_ctors[])() = { initialize_gl4es, dll_init_done };
#pragma const_seg(".CRT$XTX")
void (*const gl4es_dtor)()    = close_gl4es;
#pragma const_seg()
#endif
