// =============================================================================
//  fpe_shader.c — Fixed Pipeline Emulation Shader Generator (GLES 3.2 Edition)
//
//  Perubahan utama dari versi original:
//
//  1. DUAL PATH:
//     - Legacy  (glsl_target < 300): GLSL 100, attribute/varying, texture2D()
//     - ES3     (glsl_target >= 300): GLSL ES 3.20/3.00, in/out, texture()
//
//  2. ES3 path mengganti SEMUA GLSL 1.x built-ins yang dihapus di ES3:
//     - gl_Vertex, gl_Color, gl_Normal, gl_MultiTexCoordN, dll.
//       → layout(location=N) in ... dengan #define alias
//     - gl_ModelViewProjectionMatrix, gl_NormalMatrix, dll.
//       → uniform _gl4es_* dengan #define alias
//     - gl_FrontMaterial, gl_BackMaterial, gl_Fog, gl_Point, dll.
//       → struct uniform _gl4es_* dengan #define alias
//     - gl_FragColor → out vec4 _gl4es_FragColor (fragment)
//
//  3. Texture functions:
//     - Legacy : texture2D(), texture2DProj(), textureCube()
//     - ES3    : texture(), textureProj(), texture()
//
//  4. Qualifier selection:
//     - VS output / FS input: varying → out / in
//     - VS input: (built-in attr di GLSL 100) → layout in
//
//  Catatan penting untuk Minecraft 1.12.2:
//  - MC menggunakan fixed-function pipeline untuk GUI, partikel, langit, dll.
//  - Shader utama MC (terrain chunk) dikonversi via shaderconv.c
//  - FPE shader ini dipakai untuk semua render yang TIDAK pakai glShaderSource
// =============================================================================

#include "fpe_shader.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "string_utils.h"
#include "init.h"
#include "../glx/hardext.h"

// ── Debug ─────────────────────────────────────────────────────────────────────
//#define DEBUG
#ifdef DEBUG
#define DBG(a) a
#else
#define DBG(a)
#endif

// ── Buffer management ─────────────────────────────────────────────────────────
const char* fpeshader_signature = "// FPE_Shader generated\n";

static char* shad     = NULL;
static int   shad_cap = 0;
static int   comments = 1;

#define ShadAppend(S) shad = gl4es_append(shad, &shad_cap, S)

// =============================================================================
//  Runtime path selection — diset ulang di awal setiap fungsi shader
//
//  use_es3      : 1 jika glsl_target >= 300 (GLSL ES 3.00/3.20)
//  vary_vs_out  : qualifier output vertex shader ("out" atau "varying")
//  vary_fs_in   : qualifier input fragment shader ("in"  atau "varying")
//  frag_out_var : nama variabel output fragment  ("_gl4es_FragColor" atau "gl_FragColor")
// =============================================================================
static int         use_es3      = 0;
static const char* vary_vs_out  = "varying";
static const char* vary_fs_in   = "varying";
static const char* frag_out_var = "gl_FragColor";

// =============================================================================
//  Texture function name tables
//
//  Index: t-1 dimana t = FPE_TEX_2D=1, FPE_TEX_RECT=2, FPE_TEX_3D=3,
//         FPE_TEX_CUBE=4, FPE_TEX_STRM=5
//
//  texname     = fungsi untuk sampling DENGAN proyeksi (texture2DProj / textureProj)
//  texnoproj   = fungsi untuk sampling TANPA proyeksi  (texture2D     / texture)
//  texsampler  = tipe sampler (sama untuk legacy dan ES3)
// =============================================================================

// ── Legacy (GLSL 100) ─────────────────────────────────────────────────────────
static const char* texname_legacy[]    = {
    "texture2DProj", "texture2D",   "texture2D",   "textureCube",  "textureStreamIMG"
};
static const char* texnoproj_legacy[]  = {
    "texture2D",     "texture2D",   "texture2D",   "textureCube",  "textureStreamIMG"
};

// ── ES3 (GLSL ES 3.00+) ───────────────────────────────────────────────────────
// textureCube → texture() (satu fungsi overloaded di ES3)
// textureStreamIMG tidak ada di ES3, fallback ke texture() (extension disabled)
static const char* texname_es3[]       = {
    "textureProj",   "texture",     "texture",     "texture",      "texture"
};
static const char* texnoproj_es3[]     = {
    "texture",       "texture",     "texture",     "texture",      "texture"
};

// ── Aktif (pointer ke salah satu table) ───────────────────────────────────────
static const char** texname    = (const char**)texname_legacy;
static const char** texnoproj  = (const char**)texnoproj_legacy;

// ── Sampler types (sama untuk kedua path) ────────────────────────────────────
const char* texsampler[] = {
    "sampler2D",  "sampler2D", "sampler2D", "samplerCube", "samplerExternalOES"
};
// samplerStreamIMG tidak valid di ES3 — pakai samplerExternalOES sebagai fallback

// ── Ukuran texture coordinate ─────────────────────────────────────────────────
const char* texvecsize[] = { "vec4", "vec2", "vec2", "vec3", "vec2" };
const char* texxyzsize[] = { "stpq", "st",   "st",  "stp",  "st"   };
int texnsize[]           = { 2, 2, 3, 3, 2 };

// ── Nama komponen koordinat texture ──────────────────────────────────────────
const char texcoordname[] = { 's', 't', 'r', 'q' };
const char texcoordNAME[] = { 'S', 'T', 'R', 'Q' };
const char texcoordxy[]   = { 'x', 'y', 'z', 'w' };

// ── Alpha ref uniform ─────────────────────────────────────────────────────────
const char* gl4es_alphaRefSource = "uniform float _gl4es_AlphaRef;\n";

// =============================================================================
//  Inisialisasi path selection (dipanggil di awal setiap shader function)
// =============================================================================
static void fpe_shader_init_path(void) {
    use_es3      = (globals4es.glsl_target >= 300);
    vary_vs_out  = use_es3 ? "out"             : "varying";
    vary_fs_in   = use_es3 ? "in"              : "varying";
    frag_out_var = use_es3 ? "_gl4es_FragColor" : "gl_FragColor";
    texname      = use_es3 ? (const char**)texname_es3   : (const char**)texname_legacy;
    texnoproj    = use_es3 ? (const char**)texnoproj_es3 : (const char**)texnoproj_legacy;
}

// =============================================================================
//  ES3 Vertex Shader Preamble
//
//  Mengganti semua GLSL 1.x built-in attribute dan state uniform dengan
//  deklarasi eksplisit ber-prefix _gl4es_, lalu menambahkan #define alias
//  sehingga body shader tidak perlu diubah.
//
//  Standard OpenGL fixed-function attribute locations:
//    0 = gl_Vertex (position)
//    2 = gl_Normal
//    3 = gl_Color
//    4 = gl_SecondaryColor
//    5 = gl_FogCoord
//    8 = gl_MultiTexCoord0
//    9 = gl_MultiTexCoord1
//    ...
//    15= gl_MultiTexCoord7
// =============================================================================
static void emit_es3_vertex_preamble(fpe_state_t* state,
                                      int lighting, int twosided,
                                      int light_separate, int secondary,
                                      int fog, int fogsource,
                                      int point, int texgens,
                                      int need_normal_attr)
{
    char buff[512];

    // ── Version + precision ────────────────────────────────────────────────
    if (globals4es.glsl_target >= 320)
        ShadAppend("#version 320 es\n");
    else
        ShadAppend("#version 300 es\n");

    ShadAppend("precision highp float;\n");
    ShadAppend("precision highp int;\n");

    // ── Position attribute (selalu dibutuhkan) ────────────────────────────
    ShadAppend("layout(location=0) in highp vec4 _gl4es_Vertex;\n");
    ShadAppend("#define gl_Vertex _gl4es_Vertex\n");

    // ── Normal attribute ───────────────────────────────────────────────────
    if (need_normal_attr || lighting || texgens) {
        ShadAppend("layout(location=2) in highp vec3 _gl4es_Normal;\n");
        ShadAppend("#define gl_Normal _gl4es_Normal\n");
    }

    // ── Color attribute ────────────────────────────────────────────────────
    ShadAppend("layout(location=3) in lowp vec4 _gl4es_Color;\n");
    ShadAppend("#define gl_Color _gl4es_Color\n");

    // ── SecondaryColor attribute ───────────────────────────────────────────
    if (secondary || (lighting && light_separate)) {
        ShadAppend("layout(location=4) in lowp vec4 _gl4es_SecondaryColor;\n");
        ShadAppend("#define gl_SecondaryColor _gl4es_SecondaryColor\n");
    }

    // ── FogCoord attribute ─────────────────────────────────────────────────
    if (fog && fogsource == FPE_FOG_SRC_COORD) {
        ShadAppend("layout(location=5) in mediump float _gl4es_FogCoord;\n");
        ShadAppend("#define gl_FogCoord _gl4es_FogCoord\n");
    }

    // ── TexCoord attributes ────────────────────────────────────────────────
    for (int i = 0; i < hardext.maxtex; i++) {
        sprintf(buff, "layout(location=%d) in highp vec4 _gl4es_MultiTexCoord%d;\n", 8 + i, i);
        ShadAppend(buff);
        sprintf(buff, "#define gl_MultiTexCoord%d _gl4es_MultiTexCoord%d\n", i, i);
        ShadAppend(buff);
    }

    // ── Matriks utama ─────────────────────────────────────────────────────
    ShadAppend("uniform highp mat4 _gl4es_ModelViewProjectionMatrix;\n");
    ShadAppend("#define gl_ModelViewProjectionMatrix _gl4es_ModelViewProjectionMatrix\n");

    // ModelViewMatrix dan NormalMatrix hanya jika dibutuhkan
    if (lighting || texgens || point || (fog && fogsource != FPE_FOG_SRC_COORD)) {
        ShadAppend("uniform highp mat4 _gl4es_ModelViewMatrix;\n");
        ShadAppend("#define gl_ModelViewMatrix _gl4es_ModelViewMatrix\n");
    }
    if (lighting || need_normal_attr) {
        ShadAppend("uniform highp mat3 _gl4es_NormalMatrix;\n");
        ShadAppend("#define gl_NormalMatrix _gl4es_NormalMatrix\n");
    }

    // ── Lighting state uniforms ────────────────────────────────────────────
    if (lighting) {
        // Material struct (emission, ambient, diffuse, specular, shininess)
        ShadAppend(
            "struct _gl4es_MaterialParameters {\n"
            "    highp vec4 emission;\n"
            "    highp vec4 ambient;\n"
            "    highp vec4 diffuse;\n"
            "    highp vec4 specular;\n"
            "    highp float shininess;\n"
            "};\n"
        );
        ShadAppend("uniform _gl4es_MaterialParameters _gl4es_FrontMaterialState;\n");
        ShadAppend("#define gl_FrontMaterial _gl4es_FrontMaterialState\n");
        if (twosided) {
            ShadAppend("uniform _gl4es_MaterialParameters _gl4es_BackMaterialState;\n");
            ShadAppend("#define gl_BackMaterial _gl4es_BackMaterialState\n");
        }

        // LightModel (ambient scenelight)
        ShadAppend(
            "struct _gl4es_LightModelParameters {\n"
            "    highp vec4 ambient;\n"
            "};\n"
            "uniform _gl4es_LightModelParameters _gl4es_LightModel;\n"
        );
        ShadAppend("#define gl_LightModel _gl4es_LightModel\n");

        // LightModelProduct (scene color = emission + ambient * scenelight)
        ShadAppend(
            "struct _gl4es_LightModelProductsT {\n"
            "    highp vec4 sceneColor;\n"
            "};\n"
            "uniform _gl4es_LightModelProductsT _gl4es_FrontLightModelProduct;\n"
        );
        ShadAppend("#define gl_FrontLightModelProduct _gl4es_FrontLightModelProduct\n");
        if (twosided) {
            ShadAppend("uniform _gl4es_LightModelProductsT _gl4es_BackLightModelProduct;\n");
            ShadAppend("#define gl_BackLightModelProduct _gl4es_BackLightModelProduct\n");
        }
    }

    // ── Point rendering state ──────────────────────────────────────────────
    if (point) {
        ShadAppend(
            "struct _gl4es_PointParameters {\n"
            "    highp float size;\n"
            "    highp float sizeMin;\n"
            "    highp float sizeMax;\n"
            "    highp float distanceConstantAttenuation;\n"
            "    highp float distanceLinearAttenuation;\n"
            "    highp float distanceQuadraticAttenuation;\n"
            "};\n"
            "uniform _gl4es_PointParameters _gl4es_Point;\n"
        );
        ShadAppend("#define gl_Point _gl4es_Point\n");
    }

    ShadAppend("\n");
}

// =============================================================================
//  ES3 Fragment Shader Preamble
// =============================================================================
static void emit_es3_fragment_preamble(int fog, int fogdist, int fogsource,
                                        int shaderblend)
{
    // ── Version + precision ────────────────────────────────────────────────
    if (globals4es.glsl_target >= 320)
        ShadAppend("#version 320 es\n");
    else
        ShadAppend("#version 300 es\n");

    ShadAppend("precision mediump float;\n");
    ShadAppend("precision highp int;\n");

    // ── Fragment output (menggantikan gl_FragColor) ────────────────────────
    ShadAppend("out vec4 _gl4es_FragColor;\n");

    // ── Fog state uniform ─────────────────────────────────────────────────
    if (fog) {
        ShadAppend(
            "struct _gl4es_FogParameters {\n"
            "    highp vec4 color;\n"
            "    highp float density;\n"
            "    highp float start;\n"
            "    highp float end;\n"
            "    highp float scale;\n" // = 1.0 / (end - start), precomputed
            "};\n"
            "uniform _gl4es_FogParameters _gl4es_Fog;\n"
        );
        ShadAppend("#define gl_Fog _gl4es_Fog\n");
    }

    // ── Blend (ARM framebuffer fetch — masih valid di ES3) ─────────────────
    if (shaderblend) {
        // GL_ARM_shader_framebuffer_fetch tetap valid di ES3
    }

    ShadAppend("\n");
}

// =============================================================================
//  Utility functions — sama dengan original
// =============================================================================

const char* fpe_texenvSrc(int src, int tmu, int twosided) {
    static char buff[200];
    switch (src) {
        case FPE_SRC_TEXTURE:
            sprintf(buff, "texColor%d", tmu);
            break;
        case FPE_SRC_TEXTURE0:  case FPE_SRC_TEXTURE1:  case FPE_SRC_TEXTURE2:
        case FPE_SRC_TEXTURE3:  case FPE_SRC_TEXTURE4:  case FPE_SRC_TEXTURE5:
        case FPE_SRC_TEXTURE6:  case FPE_SRC_TEXTURE7:  case FPE_SRC_TEXTURE8:
        case FPE_SRC_TEXTURE9:  case FPE_SRC_TEXTURE10: case FPE_SRC_TEXTURE11:
        case FPE_SRC_TEXTURE12: case FPE_SRC_TEXTURE13: case FPE_SRC_TEXTURE14:
        case FPE_SRC_TEXTURE15:
            sprintf(buff, "texColor%d", src - FPE_SRC_TEXTURE0);
            break;
        case FPE_SRC_CONSTANT:
            sprintf(buff, "_gl4es_TextureEnvColor_%d", tmu);
            break;
        case FPE_SRC_PRIMARY_COLOR:
            sprintf(buff, "%s", twosided ? "((gl_FrontFacing)?Color:BackColor)" : "Color");
            break;
        case FPE_SRC_PREVIOUS:
            sprintf(buff, "fColor");
            break;
        case FPE_SRC_ONE:
            sprintf(buff, "vec4(1.0)");
            break;
        case FPE_SRC_ZERO:
            sprintf(buff, "vec4(0.0)");
            break;
        case FPE_SRC_SECONDARY_COLOR:
            sprintf(buff, "%s", twosided ? "((gl_FrontFacing)?SecColor:SecBackColor)" : "SecColor");
            break;
    }
    return buff;
}

int fpe_texenvSecondary(fpe_state_t* state) {
    for (int i = 0; i < hardext.maxtex; i++) {
        int t = state->texture[i].textype;
        if (t) {
            int texenv = state->texenv[i].texenv;
            if (texenv == FPE_COMBINE) {
                int combine_rgb = state->texcombine[i] & 0xf;
                int src_r[4];
                src_r[0] = state->texenv[i].texsrcrgb0;
                src_r[1] = state->texenv[i].texsrcrgb1;
                src_r[2] = state->texenv[i].texsrcrgb2;
                src_r[3] = state->texenv[i].texsrcrgb3;
                if (combine_rgb == FPE_CR_DOT3_RGBA) {
                    src_r[2] = -1;
                } else {
                    if (combine_rgb == FPE_CR_REPLACE)
                        src_r[1] = src_r[2] = -1;
                    else if (combine_rgb < FPE_CR_MOD_ADD && combine_rgb != FPE_CR_INTERPOLATE)
                        src_r[2] = -1;
                }
                if (src_r[0] == FPE_SRC_SECONDARY_COLOR ||
                    src_r[1] == FPE_SRC_SECONDARY_COLOR ||
                    src_r[2] == FPE_SRC_SECONDARY_COLOR)
                    return 1;
            }
        }
    }
    return 0;
}

char* fpe_packed64(uint64_t x, int s, int k) {
    static char buff[8][65];
    static int  idx = 0;
    idx &= 7;
    uint64_t mask = (1L << k) - 1L;
    const char* hex = "0123456789ABCDEF";
    int j = s / k;
    buff[idx][j] = '\0';
    for (int i = 0; i < s; i += k) {
        buff[idx][--j] = hex[(x & mask)];
        x >>= k;
    }
    return buff[idx++];
}

char* fpe_packed(int x, int s, int k) {
    static char buff[8][33];
    static int  idx = 0;
    idx &= 7;
    int mask = (1 << k) - 1;
    const char* hex = "0123456789ABCDEF";
    int j = s / k;
    buff[idx][j] = '\0';
    for (int i = 0; i < s; i += k) {
        buff[idx][--j] = hex[(x & mask)];
        x >>= k;
    }
    return buff[idx++];
}

char* fpe_binary(int x, int s) {
    return fpe_packed(x, s, 1);
}

// =============================================================================
//  fpe_VertexShader — Generate vertex shader FPE
// =============================================================================
const char* const* fpe_VertexShader(shaderconv_need_t* need, fpe_state_t* state)
{
    // ── Init ──────────────────────────────────────────────────────────────
    if (!shad_cap) shad_cap = 1024;
    if (!shad) shad = (char*)malloc(shad_cap);

    fpe_shader_init_path();

    fpe_state_t default_state = {0};
    int is_default = !!need;
    if (!state) state = &default_state;

    int lighting         = state->lighting;
    int twosided         = state->twosided && lighting;
    if (need && ((need->need_color > 1) || (need->need_secondary > 1)))
        twosided = 1;
    int light_separate   = state->light_separate && lighting;
    int secondary        = (state->colorsum && !(lighting && light_separate))
                           || fpe_texenvSecondary(state)
                           || (need && need->need_secondary);
    int fog              = state->fog || (need && need->need_fogcoord);
    int fogsource        = state->fogsource;
    int fogdist          = state->fogdist;
    int fogmode          = state->fogmode;
    int color_material   = state->color_material && lighting;
    int point            = state->point;
    int pointsprite      = state->pointsprite;
    int planes           = state->plane;
    int cm_front_nullexp = state->cm_front_nullexp;
    int cm_back_nullexp  = state->cm_back_nullexp;
    int texgens = 0, texmats = 0;

    for (int i = 0; i < hardext.maxtex; ++i) {
        if (state->texgen[i].texgen_s || state->texgen[i].texgen_t ||
            state->texgen[i].texgen_r || state->texgen[i].texgen_q)
            texgens = 1;
        if (state->texture[i].texmat)
            texmats = 1;
    }

    const char* fogp = hardext.highp ? "highp" : "mediump";

    char buff[1024];
    int  need_vertex = 0;
    int  need_eyeplane[MAX_TEX][4] = {0};
    int  need_objplane[MAX_TEX][4] = {0};
    int  need_adjust[MAX_TEX]      = {0};
    int  need_lightproduct[2][MAX_LIGHT] = {0};

    // ── ES3: emit preamble SEBELUM signature ──────────────────────────────
    // (versi directive HARUS menjadi baris pertama shader)
    shad[0] = '\0';
    if (use_es3) {
        emit_es3_vertex_preamble(state, lighting, twosided, light_separate,
                                  secondary, fog, fogsource, point, texgens,
                                  /*need_normal_attr=*/0);
    }

    // ── Signature + comments ──────────────────────────────────────────────
    ShadAppend(fpeshader_signature);
    int headers = gl4es_countline(shad);

    comments = globals4es.comments;
    DBG(comments = 1 - comments;)

    if (comments) {
        sprintf(buff,
            "// ** Vertex Shader (%s) **\n"
            "// lighting=%d (twosided=%d, separate=%d, color_material=%d)\n"
            "// secondary=%d, planes=%s\n"
            "// point=%d%s\n",
            use_es3 ? "ES3" : "ES2",
            lighting, twosided, light_separate, color_material,
            secondary, fpe_binary(planes, 6),
            point, need ? " with need" : "");
        ShadAppend(buff);
        headers += gl4es_countline(buff);
        if (need) {
            sprintf(buff, "// need: color=%d, texs=%s, fogcoord=%d\n",
                    need->need_color, fpe_binary(need->need_texs, 16), need->need_fogcoord);
            ShadAppend(buff);
            headers += gl4es_countline(buff);
        }
    }

    // ── Varyings & uniforms di header ─────────────────────────────────────

    // Default path "is_default=1 && need" uses gl_FrontColor etc.
    // Untuk ES3, kita declare custom output dan define alias
    if (!is_default) {
        if (use_es3) {
            sprintf(buff, "%s vec4 Color;\n", vary_vs_out);
        } else {
            sprintf(buff, "%s vec4 Color;\n", vary_vs_out);
        }
        ShadAppend(buff);
        headers++;
    } else if (use_es3 && need) {
        // ES3: gl_FrontColor/BackColor tidak ada → declare custom out varying
        if (need->need_color >= 1) {
            ShadAppend("out vec4 _gl4es_FrontColor;\n");
            ShadAppend("#define gl_FrontColor _gl4es_FrontColor\n");
            headers += 2;
        }
        if (need->need_color >= 2) {
            ShadAppend("out vec4 _gl4es_BackColor;\n");
            ShadAppend("#define gl_BackColor _gl4es_BackColor\n");
            headers += 2;
        }
        if (need->need_secondary >= 1) {
            ShadAppend("out vec4 _gl4es_FrontSecondaryColor;\n");
            ShadAppend("#define gl_FrontSecondaryColor _gl4es_FrontSecondaryColor\n");
            headers += 2;
        }
        if (need->need_secondary >= 2) {
            ShadAppend("out vec4 _gl4es_BackSecondaryColor;\n");
            ShadAppend("#define gl_BackSecondaryColor _gl4es_BackSecondaryColor\n");
            headers += 2;
        }
    }

    // Clip planes varyings
    if (planes) {
        for (int i = 0; i < hardext.maxplanes; i++) {
            if ((planes >> i) & 1) {
                sprintf(buff, "uniform highp vec4 _gl4es_ClipPlane_%d;\n", i);
                ShadAppend(buff); ++headers;
                sprintf(buff, "%s mediump float clippedvertex_%d;\n", vary_vs_out, i);
                ShadAppend(buff); ++headers;
            }
        }
    }

    // Lighting struct declarations (NON-ES3 path: questi sono built-in in GLSL 100)
    // ES3 path: già emessi nel preamble
    if (lighting) {
        if (use_es3) {
            // Structs già nel preamble ES3 — solo aggiungiamo quelli FPE-specific
            // che l'originale già dichiara manualmente (_gl4es_FPELightSourceParameters*)
        }
        sprintf(buff,
            "struct _gl4es_FPELightSourceParameters1\n"
            "{\n"
            "%s"
            "   highp vec4 specular;\n"
            "   highp vec4 position;\n"
            "   highp vec3 spotDirection;\n"
            "   highp float spotExponent;\n"
            "   highp float spotCosCutoff;\n"
            "   highp float constantAttenuation;\n"
            "   highp float linearAttenuation;\n"
            "   highp float quadraticAttenuation;\n"
            "};\n",
            color_material ?
                "   highp vec4 ambient;\n"
                "   highp vec4 diffuse;\n" : "");
        ShadAppend(buff); headers += gl4es_countline(buff);

        sprintf(buff,
            "struct _gl4es_FPELightSourceParameters0\n"
            "{\n"
            "%s"
            "   highp vec4 specular;\n"
            "   highp vec4 position;\n"
            "   highp vec3 spotDirection;\n"
            "   highp float spotExponent;\n"
            "   highp float spotCosCutoff;\n"
            "};\n",
            color_material ?
                "   highp vec4 ambient;\n"
                "   highp vec4 diffuse;\n" : "");
        ShadAppend(buff); headers += gl4es_countline(buff);

        ShadAppend(
            "struct _gl4es_LightProducts\n"
            "{\n"
            "   highp vec4 ambient;\n"
            "   highp vec4 diffuse;\n"
            "   highp vec4 specular;\n"
            "};\n");
        headers += 6;

        if (!(cm_front_nullexp && color_material)) {
            ShadAppend("uniform highp float _gl4es_FrontMaterial_shininess;\n");
            headers++;
        }
        if (twosided && !(cm_back_nullexp && color_material)) {
            ShadAppend("uniform highp float _gl4es_BackMaterial_shininess;\n");
            headers++;
        }
        if (!(color_material && (state->cm_front_mode == FPE_CM_DIFFUSE ||
                                  state->cm_front_mode == FPE_CM_AMBIENTDIFFUSE))) {
            ShadAppend("uniform highp float _gl4es_FrontMaterial_alpha;\n");
            headers++;
            if (twosided) {
                ShadAppend("uniform highp float _gl4es_BackMaterial_alpha;\n");
                headers++;
            }
        }
        for (int i = 0; i < hardext.maxlights; i++) {
            if (state->light & (1 << i)) {
                sprintf(buff, "uniform _gl4es_FPELightSourceParameters%d _gl4es_LightSource_%d;\n",
                        (state->light_direction >> i & 1) ? 1 : 0, i);
                ShadAppend(buff); headers++;
                sprintf(buff, "uniform _gl4es_LightProducts _gl4es_FrontLightProduct_%d;\n", i);
                ShadAppend(buff); headers++;
                if (twosided) {
                    sprintf(buff, "uniform _gl4es_LightProducts _gl4es_BackLightProduct_%d;\n", i);
                    ShadAppend(buff); headers++;
                }
            }
        }
    }

    if (!is_default) {
        if (twosided) {
            sprintf(buff, "%s vec4 BackColor;\n", vary_vs_out);
            ShadAppend(buff); headers++;
        }
        if (light_separate || secondary) {
            sprintf(buff, "%s vec4 SecColor;\n", vary_vs_out);
            ShadAppend(buff); headers++;
            if (twosided) {
                sprintf(buff, "%s vec4 SecBackColor;\n", vary_vs_out);
                ShadAppend(buff); headers++;
            }
        }
    }

    if (fog) {
        if (fogsource == FPE_FOG_SRC_COORD)
            sprintf(buff, "%s %s float FogSrc;\n", vary_vs_out, fogp);
        else {
            if (need_vertex < 1) need_vertex = 1;
            switch (fogdist) {
                case FPE_FOG_DIST_RADIAL:
                    sprintf(buff, "%s %s vec3 FogSrc;\n", vary_vs_out, fogp); break;
                default:
                    sprintf(buff, "%s %s float FogSrc;\n", vary_vs_out, fogp); break;
            }
        }
        ShadAppend(buff);
    }

    // Texture coordinate varyings
    for (int i = 0; i < hardext.maxtex; i++) {
        int t = state->texture[i].textype;
        if (need) t = (need->need_texs & (1 << i)) ? 1 : 0;
        if (t) {
            sprintf(buff, "%s %s _gl4es_TexCoord_%d;\n", vary_vs_out, texvecsize[t-1], i);
            ShadAppend(buff); headers++;
            if (state->texture[i].texmat) {
                sprintf(buff, "uniform highp mat4 _gl4es_TextureMatrix_%d;\n", i);
                ShadAppend(buff); headers++;
            }
        }
    }

    // ── main() ────────────────────────────────────────────────────────────
    ShadAppend("\nvoid main() {\n");
    int need_normal = 0;
    int normal_line = gl4es_countline(shad) - headers;

    // Clip planes
    if (planes) {
        for (int i = 0; i < hardext.maxplanes; i++) {
            if ((planes >> i) & 1) {
                sprintf(buff, "clippedvertex_%d = dot(gl_Vertex, _gl4es_ClipPlane_%d);\n", i, i);
                ShadAppend(buff);
            }
        }
        if (!need_vertex) need_vertex = 1;
    }

    // Transform
    ShadAppend("gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;\n");

    // ── Color / Lighting ──────────────────────────────────────────────────
    if (!lighting) {
        if (is_default && need) {
            if (need->need_color >= 1) ShadAppend("gl_FrontColor = gl_Color;\n");
            if (need->need_color == 2) ShadAppend("gl_BackColor = gl_Color;\n");
            if (need->need_secondary >= 1) ShadAppend("gl_FrontSecondaryColor = gl_SecondaryColor;\n");
            if (need->need_secondary == 2) ShadAppend("gl_BackSecondaryColor = gl_SecondaryColor;\n");
        } else {
            if (!need || (need && need->need_color))
                ShadAppend("Color = gl_Color;\n");
            if (secondary)
                ShadAppend("SecColor = gl_SecondaryColor;\n");
        }
    } else {
        if (comments) {
            sprintf(buff, "// ColorMaterial On=%d Front=%d Back=%d\n",
                    color_material, state->cm_front_mode, state->cm_back_mode);
            ShadAppend(buff);
        }
        if (is_default && need) {
            ShadAppend("vec4 Color;\n");
            if (twosided)    ShadAppend("vec4 BackColor;\n");
            if (secondary)   ShadAppend("vec4 SecColor;\n");
            if (secondary && twosided) ShadAppend("vec4 SecBackColor;\n");
        }

        // Material shortcuts (original logic preserved)
        char fm_emission[60], fm_ambient[60], fm_diffuse[60], fm_specular[60];
        char bm_emission[60], bm_ambient[60], bm_diffuse[60], bm_specular[60];

        sprintf(fm_emission, "%s",
            (color_material && state->cm_front_mode == FPE_CM_EMISSION)
            ? "gl_Color" : "gl_FrontMaterial.emission");
        sprintf(fm_ambient, "%s",
            (color_material && (state->cm_front_mode == FPE_CM_AMBIENT ||
                                state->cm_front_mode == FPE_CM_AMBIENTDIFFUSE))
            ? "gl_Color" : "gl_FrontMaterial.ambient");
        sprintf(fm_diffuse, "%s",
            (color_material && (state->cm_front_mode == FPE_CM_DIFFUSE ||
                                state->cm_front_mode == FPE_CM_AMBIENTDIFFUSE))
            ? "gl_Color.xyz * _gl4es_LightSource_" : "_gl4es_FrontLightProduct_");
        sprintf(fm_specular, "%s",
            (color_material && state->cm_front_mode == FPE_CM_SPECULAR)
            ? "gl_Color.xyz * _gl4es_LightSource_" : "_gl4es_FrontLightProduct_");

        if (twosided) {
            sprintf(bm_emission, "%s",
                (color_material && state->cm_back_mode == FPE_CM_EMISSION)
                ? "gl_Color" : "gl_BackMaterial.emission");
            sprintf(bm_ambient, "%s",
                (color_material && (state->cm_back_mode == FPE_CM_AMBIENT ||
                                    state->cm_back_mode == FPE_CM_AMBIENTDIFFUSE))
                ? "gl_Color" : "gl_BackMaterial.ambient");
            sprintf(bm_diffuse, "%s",
                (color_material && (state->cm_back_mode == FPE_CM_DIFFUSE ||
                                    state->cm_back_mode == FPE_CM_AMBIENTDIFFUSE))
                ? "gl_Color.xyz * _gl4es_LightSource_" : "_gl4es_BackLightProduct_");
            sprintf(bm_specular, "%s",
                (color_material && state->cm_back_mode == FPE_CM_SPECULAR)
                ? "gl_Color.xyz * _gl4es_LightSource_" : "_gl4es_BackLightProduct_");
        }

        // Scene color / emission
        if (color_material &&
            (state->cm_front_mode == FPE_CM_EMISSION ||
             state->cm_front_mode == FPE_CM_AMBIENT   ||
             state->cm_front_mode == FPE_CM_AMBIENTDIFFUSE ||
             (twosided && (state->cm_back_mode == FPE_CM_EMISSION ||
                           state->cm_back_mode == FPE_CM_AMBIENT   ||
                           state->cm_back_mode == FPE_CM_AMBIENTDIFFUSE))))
        {
            sprintf(buff, "Color = %s;\n", fm_emission); ShadAppend(buff);
            if (twosided) { sprintf(buff, "BackColor = %s;\n", bm_emission); ShadAppend(buff); }
            sprintf(buff, "Color += %s*gl_LightModel.ambient;\n", fm_ambient); ShadAppend(buff);
            if (twosided) { sprintf(buff, "BackColor += %s*gl_LightModel.ambient;\n", bm_ambient); ShadAppend(buff); }
        } else {
            ShadAppend("Color = gl_FrontLightModelProduct.sceneColor;\n");
            if (twosided) ShadAppend("BackColor = gl_BackLightModelProduct.sceneColor;\n");
        }

        if (light_separate) {
            ShadAppend("SecColor=vec4(0.);\n");
            if (twosided) ShadAppend("SecBackColor=vec4(0.);\n");
        }

        ShadAppend("highp float att;\n");
        ShadAppend("highp float spot;\n");
        ShadAppend("highp vec3 VP;\n");
        ShadAppend("highp float lVP;\n");
        ShadAppend("highp float nVP;\n");
        ShadAppend("highp vec3 aa,dd,ss;\n");
        ShadAppend("highp vec3 hi;\n");
        if (twosided) ShadAppend("highp vec3 back_aa,back_dd,back_ss;\n");
        need_normal = 1;

        for (int i = 0; i < hardext.maxlights; i++) {
            if (state->light & (1 << i)) {
                if (comments) {
                    sprintf(buff, "// light %d on, dir=%d, cutoff180=%d\n",
                            i, (state->light_direction >> i & 1), (state->light_cutoff180 >> i & 1));
                    ShadAppend(buff);
                }
                if ((state->light_direction >> i & 1) == 0) {
                    ShadAppend("att = 1.0;\n");
                    sprintf(buff, "VP = normalize(_gl4es_LightSource_%d.position.xyz);\n", i);
                    ShadAppend(buff);
                } else {
                    sprintf(buff, "VP = _gl4es_LightSource_%d.position.xyz - gl_Vertex.xyz;\n", i);
                    ShadAppend(buff);
                    ShadAppend("lVP = length(VP);\n");
                    sprintf(buff,
                        "att = 1.0/(_gl4es_LightSource_%d.constantAttenuation"
                        " + lVP*(_gl4es_LightSource_%d.linearAttenuation"
                        " + _gl4es_LightSource_%d.quadraticAttenuation * lVP));\n", i, i, i);
                    ShadAppend(buff);
                    ShadAppend("VP = normalize(VP);\n");
                    if (!need_vertex) need_vertex = 1;
                }
                if (state->light_cutoff180 >> i & 1) {
                    sprintf(buff, "spot = max(dot(-VP, _gl4es_LightSource_%d.spotDirection), 0.);\n", i);
                    ShadAppend(buff);
                    sprintf(buff,
                        "if(spot<_gl4es_LightSource_%d.spotCosCutoff) spot=0.0;"
                        " else spot=pow(spot, _gl4es_LightSource_%d.spotExponent);\n", i, i);
                    ShadAppend(buff);
                    ShadAppend("att *= spot;\n");
                }
                if (color_material && (state->cm_front_mode == FPE_CM_AMBIENT ||
                                       state->cm_front_mode == FPE_CM_AMBIENTDIFFUSE)) {
                    sprintf(buff, "aa = %s.xyz * _gl4es_LightSource_%d.ambient.xyz;\n", fm_ambient, i);
                    ShadAppend(buff);
                } else {
                    sprintf(buff, "aa = _gl4es_FrontLightProduct_%d.ambient.xyz;\n", i);
                    ShadAppend(buff);
                    need_lightproduct[0][i] = 1;
                }
                if (twosided) {
                    if (color_material && (state->cm_back_mode == FPE_CM_AMBIENT ||
                                           state->cm_back_mode == FPE_CM_AMBIENTDIFFUSE)) {
                        sprintf(buff, "back_aa = %s.xyz * _gl4es_LightSource_%d.ambient.xyz;\n", bm_ambient, i);
                        ShadAppend(buff);
                    } else {
                        sprintf(buff, "back_aa = _gl4es_BackLightProduct_%d.ambient.xyz;\n", i);
                        ShadAppend(buff);
                        need_lightproduct[1][i] = 1;
                    }
                }
                sprintf(buff, "nVP = dot(normal, VP);\n"); ShadAppend(buff);
                sprintf(buff, "dd = (nVP>0.)?(nVP * %s%d.diffuse.xyz):vec3(0.);\n", fm_diffuse, i);
                ShadAppend(buff); need_lightproduct[0][i] = 1;
                if (twosided) {
                    sprintf(buff, "back_dd = (nVP<0.)?(-nVP * %s%d.diffuse.xyz):vec3(0.);\n", bm_diffuse, i);
                    ShadAppend(buff); need_lightproduct[1][i] = 1;
                }
                if (state->light_localviewer) {
                    ShadAppend("hi = normalize(VP + normalize(-gl_Vertex.xyz));\n");
                    if (!need_vertex) need_vertex = 1;
                } else {
                    ShadAppend("hi = normalize(VP + vec3(0., 0., 1.));\n");
                }
                ShadAppend("lVP = dot(normal, hi);\n");
                if (cm_front_nullexp)
                    sprintf(buff,
                        "ss = (nVP>0. && lVP>0.)?(pow(lVP, %s)*%s%d.specular.xyz):vec3(0.);\n",
                        color_material ? "gl_FrontMaterial.shininess" : "_gl4es_FrontMaterial_shininess",
                        fm_specular, i);
                else
                    sprintf(buff, "ss = (nVP>0. && lVP>0.)?(%s%d.specular.xyz):vec3(0.);\n", fm_specular, i);
                ShadAppend(buff);
                if (twosided) {
                    if (state->cm_back_nullexp)
                        sprintf(buff,
                            "back_ss = (nVP<0. && lVP<0.)?(pow(-lVP, %s)*%s%d.specular.xyz):vec3(0.);\n",
                            color_material ? "gl_BackMaterial.shininess" : "_gl4es_BackMaterial_shininess",
                            bm_specular, i);
                    else
                        sprintf(buff, "back_ss = (nVP<0. && lVP<0.)?(%s%d.specular.xyz):vec3(0.);\n",
                                bm_specular, i);
                    ShadAppend(buff);
                }
                if (state->light_separate) {
                    ShadAppend("Color.rgb += att*(aa+dd);\n");
                    ShadAppend("SecColor.rgb += att*(ss);\n");
                    if (twosided) {
                        ShadAppend("BackColor.rgb += att*(back_aa+back_dd);\n");
                        ShadAppend("SecBackColor.rgb += att*(back_ss);\n");
                    }
                } else {
                    ShadAppend("Color.rgb += att*(aa+dd+ss);\n");
                    if (twosided)
                        ShadAppend("BackColor.rgb += att*(back_aa+back_dd+back_ss);\n");
                }
                if (comments) { sprintf(buff, "// end light %d\n", i); ShadAppend(buff); }
            }
        }

        sprintf(buff, "Color.a = %s;\n",
            (color_material && (state->cm_front_mode == FPE_CM_DIFFUSE ||
                                state->cm_front_mode == FPE_CM_AMBIENTDIFFUSE))
            ? "gl_Color.a" : "_gl4es_FrontMaterial_alpha");
        ShadAppend(buff);
        ShadAppend("Color.rgb = clamp(Color.rgb, 0., 1.);\n");

        if (twosided) {
            sprintf(buff, "BackColor.a = %s;\n",
                (color_material && (state->cm_back_mode == FPE_CM_DIFFUSE ||
                                    state->cm_back_mode == FPE_CM_AMBIENTDIFFUSE))
                ? "gl_Color.a" : "_gl4es_BackMaterial_alpha");
            ShadAppend("BackColor.rgb = clamp(BackColor.rgb, 0., 1.);\n");
            ShadAppend(buff);
        }
        if (state->light_separate) {
            ShadAppend("SecColor.rgb = clamp(SecColor.rgb, 0., 1.);\n");
            if (twosided) ShadAppend("SecBackColor.rgb = clamp(SecBackColor.rgb, 0., 1.);\n");
        }

        if (is_default && need) {
            if (need->need_color > 0)     ShadAppend("gl_FrontColor = Color;\n");
            if (need->need_color > 1)     ShadAppend("gl_BackColor = BackColor;\n");
            if (need->need_secondary > 0) ShadAppend("gl_FrontSecondaryColor = SecColor;\n");
            if (need->need_secondary > 1) ShadAppend("gl_BackSecondaryColor = SecBackColor;\n");
        }
    }

    // ── Texture coordinates ───────────────────────────────────────────────
    if (comments) ShadAppend("// texturing\n");
    if (texgens) ShadAppend("vec4 tmp_tcoor;\n");
    int spheremap = 0, reflectmap = 0;
    if (texmats) ShadAppend("vec4 tmp_tex;\n");

    for (int i = 0; i < hardext.maxtex; i++) {
        int t = state->texture[i].textype;
        if (need && (need->need_texs & (1 << i)) && t == 0) t = 1;
        if (need && !(need->need_texs & (1 << i))) t = 0;
        int mat    = state->texture[i].texmat;
        int adjust = state->texture[i].texadjust;
        int tg[4]  = {
            state->texgen[i].texgen_s, state->texgen[i].texgen_t,
            state->texgen[i].texgen_r, state->texgen[i].texgen_q
        };
        if (t) {
            int ntc = texnsize[t-1];
            if (comments) {
                sprintf(buff, "// texture %d: %X %s %s\n",
                        i, t, mat ? "with matrix" : "", adjust ? "npot adjusted" : "");
                ShadAppend(buff);
            }
            char texcoord[50];
            if (tg[0] || tg[1] || tg[2] || tg[3]) {
                if (tg[0]) tg[0] = state->texgen[i].texgen_s_mode; else tg[0] = FPE_TG_NONE;
                if (tg[1]) tg[1] = state->texgen[i].texgen_t_mode; else tg[1] = FPE_TG_NONE;
                if (tg[2]) tg[2] = state->texgen[i].texgen_r_mode; else tg[2] = FPE_TG_NONE;
                if (tg[3]) tg[3] = state->texgen[i].texgen_q_mode; else tg[3] = FPE_TG_NONE;
                if (comments) {
                    sprintf(buff, "//  texgen %d/%d/%d/%d\n", tg[0], tg[1], tg[2], tg[3]);
                    ShadAppend(buff);
                }
                sprintf(texcoord, "tmp_tcoor");
                ShadAppend("tmp_tcoor=vec4(0., 0., 0., 1.);\n");
                if (mat) ntc = 4;
                for (int j = 0; j < ntc; j++) {
                    if (tg[j] == FPE_TG_NORMALMAP) {
                        need_normal = 1;
                        if (j == 0 && tg[j+1] == FPE_TG_NORMALMAP) {
                            sprintf(buff, "tmp_tcoor.%c%c=normal.%c%c;\n",
                                    texcoordxy[j], texcoordxy[j+1], texcoordxy[j], texcoordxy[j+1]);
                            ++j;
                        } else {
                            sprintf(buff, "tmp_tcoor.%c=normal.%c;\n", texcoordxy[j], texcoordxy[j]);
                        }
                    } else if (tg[j] == FPE_TG_SPHEREMAP) {
                        if (!spheremap) {
                            spheremap = 1;
                            if (!need_vertex) need_vertex = 1;
                            need_normal = 1;
                            ShadAppend("vec3 tmpsphere = reflect(normalize(gl_Vertex.xyz), normal);\n");
                            ShadAppend("tmpsphere.z+=1.0;\n");
                            if (j == 0 && tg[j+1] == FPE_TG_SPHEREMAP)
                                sprintf(buff, "tmp_tcoor.xy = tmpsphere.xy*(0.5*inversesqrt(dot(tmpsphere, tmpsphere))) + vec2(0.5);");
                            else
                                ShadAppend("tmpsphere.xy = tmpsphere.xy*(0.5*inversesqrt(dot(tmpsphere, tmpsphere))) + vec2(0.5);");
                        }
                        if (j == 0 && tg[j+1] == FPE_TG_SPHEREMAP) ++j;
                        else sprintf(buff, "tmp_tcoor.%c=tmpsphere.%c;\n", texcoordxy[j], texcoordxy[j]);
                    } else if (tg[j] == FPE_TG_OBJLINEAR) {
                        sprintf(buff, "tmp_tcoor.%c=dot(gl_Vertex, _gl4es_ObjectPlane%c_%d);\n",
                                texcoordxy[j], texcoordNAME[j], i);
                        need_objplane[i][j] = 1;
                    } else if (tg[j] == FPE_TG_EYELINEAR) {
                        sprintf(buff, "tmp_tcoor.%c=dot(vertex, _gl4es_EyePlane%c_%d);\n",
                                texcoordxy[j], texcoordNAME[j], i);
                        need_eyeplane[i][j] = 1;
                        if (!need_vertex) need_vertex = 1;
                    } else if (tg[j] == FPE_TG_REFLECMAP) {
                        if (!reflectmap) {
                            reflectmap = 1;
                            if (!need_vertex) need_vertex = 1;
                            need_normal = 1;
                            if (j == 0 && tg[j+1] == FPE_TG_REFLECMAP && tg[j+2] == FPE_TG_REFLECMAP)
                                sprintf(buff, "tmp_tcoor.xyz = reflect(normalize(gl_Vertex.xyz), normal);\n");
                            else
                                ShadAppend("vec3 tmpreflect = reflect(normalize(gl_Vertex.xyz), normal);\n");
                        }
                        if (j == 0 && tg[j+1] == FPE_TG_REFLECMAP && tg[j+2] == FPE_TG_REFLECMAP)
                            j += 2;
                        else
                            sprintf(buff, "tmp_tcoor.%c=tmpreflect.%c;\n", texcoordxy[j], texcoordxy[j]);
                    } else if (tg[j] == FPE_TG_NONE) {
                        sprintf(buff, "tmp_tcoor.%c=gl_MultiTexCoord%d.%c;\n",
                                texcoordxy[j], i, texcoordxy[j]);
                    }
                    ShadAppend(buff);
                }
            } else {
                sprintf(texcoord, "gl_MultiTexCoord%d", i);
            }
            const char* text_tmp = texcoord;
            static const char* tmp_tex = "tmp_tex";
            if (mat) {
                text_tmp = tmp_tex;
                sprintf(buff, "%s = (_gl4es_TextureMatrix_%d * %s);\n", text_tmp, i, texcoord);
                ShadAppend(buff);
            }
            if (t == FPE_TEX_STRM) {
                sprintf(buff, "_gl4es_TexCoord_%d = %s.%s / %s.q;\n",
                        i, text_tmp, texxyzsize[t-1], text_tmp);
            } else {
                sprintf(buff, "_gl4es_TexCoord_%d = %s.%s;\n", i, text_tmp, texxyzsize[t-1]);
            }
            ShadAppend(buff);
            if (adjust) {
                need_adjust[i] = 1;
                sprintf(buff, "_gl4es_TexCoord_%d.xy *= _gl4es_TexAdjust_%d;\n", i, i);
                ShadAppend(buff);
            }
        }
    }

    // ── Point sprite ──────────────────────────────────────────────────────
    if (point) {
        if (!need_vertex) need_vertex = 1;
        ShadAppend("float ps_d = length(gl_Vertex);\n");
        sprintf(buff,
            "gl_PointSize = clamp("
            "gl_Point.size*inversesqrt("
            "gl_Point.distanceConstantAttenuation"
            " + ps_d*(gl_Point.distanceLinearAttenuation"
            " + ps_d*gl_Point.distanceQuadraticAttenuation)),"
            "gl_Point.sizeMin, gl_Point.sizeMax);\n");
        ShadAppend(buff);
    }

    // ── Insert vertex / normal (inplace) ──────────────────────────────────
    if (need_vertex) {
        buff[0] = '\0';
        if (need_vertex == 1) strcat(buff, "vec4 ");
        strcat(buff, "vertex = gl_ModelViewMatrix * gl_Vertex;\n");
        shad = gl4es_inplace_insert(gl4es_getline(shad, normal_line + headers), buff, shad, &shad_cap);
        normal_line += gl4es_countline(buff);
    }
    if (need_normal) {
        if (state->rescaling || state->normalize || globals4es.normalize)
            strcpy(buff, "vec3 normal = normalize(gl_NormalMatrix * gl_Normal);\n");
        else
            strcpy(buff, "vec3 normal = gl_NormalMatrix * gl_Normal;\n");
        shad = gl4es_inplace_insert(gl4es_getline(shad, normal_line + headers), buff, shad, &shad_cap);
    }

    // ── Insert eye/obj planes & tex adjust uniforms ───────────────────────
    buff[0] = '\0';
    for (int i = 0; i < MAX_TEX; i++) {
        char tmp[100];
        for (int j = 0; j < 4; j++) {
            if (need_objplane[i][j]) {
                sprintf(tmp, "uniform vec4 _gl4es_ObjectPlane%c_%d;\n", texcoordNAME[j], i);
                strcat(buff, tmp);
            }
            if (need_eyeplane[i][j]) {
                sprintf(tmp, "uniform vec4 _gl4es_EyePlane%c_%d;\n", texcoordNAME[j], i);
                strcat(buff, tmp);
            }
        }
        if (need_adjust[i]) {
            sprintf(tmp, "uniform vec2 _gl4es_TexAdjust_%d;\n", i);
            strcat(buff, tmp);
        }
    }
    if (buff[0] != '\0') {
        shad = gl4es_inplace_insert(gl4es_getline(shad, headers), buff, shad, &shad_cap);
        headers += gl4es_countline(buff);
    }

    // ── Fog ───────────────────────────────────────────────────────────────
    if (fog) {
        if (comments) {
            sprintf(buff, "// Fog: mode=%X src=%X dist=%X\n", fogmode, fogsource, fogdist);
            ShadAppend(buff);
        }
        if (fogsource == FPE_FOG_SRC_COORD)
            sprintf(buff, "FogSrc = gl_FogCoord;\n");
        else switch (fogdist) {
            case FPE_FOG_DIST_RADIAL: sprintf(buff, "FogSrc = vertex.xyz;\n"); break;
            default: sprintf(buff, "FogSrc = vertex.z;\n"); break;
        }
        ShadAppend(buff);
    }

    ShadAppend("}\n");

    DBG(printf("FPE VertexShader:\n%s\n", shad);)
    return (const char* const*)&shad;
}

// =============================================================================
//  fpe_FragmentShader — Generate fragment shader FPE
// =============================================================================
const char* const* fpe_FragmentShader(shaderconv_need_t* need, fpe_state_t* state)
{
    if (!shad_cap) shad_cap = 1024;
    if (!shad) shad = (char*)malloc(shad_cap);

    fpe_shader_init_path();

    fpe_state_t default_state = {0};
    int is_default     = !need;
    if (!state) state  = &default_state;

    int headers        = 0;
    int lighting       = state->lighting;
    int twosided       = state->twosided && lighting;
    int light_separate = state->light_separate && lighting;
    int secondary      = is_default
                         ? ((state->colorsum && !(lighting && light_separate)) || fpe_texenvSecondary(state))
                         : need->need_secondary;
    int alpha_test     = state->alphatest;
    int alpha_func     = state->alphafunc;
    int fog            = is_default ? state->fog : need->need_fogcoord;
    int fogsource      = state->fogsource;
    int fogmode        = state->fogmode;
    int fogdist        = state->fogdist;
    int planes         = state->plane;
    int point          = state->point;
    int pointsprite    = state->pointsprite;
    int pointsprite_coord = state->pointsprite_coord;
    int pointsprite_upper = state->pointsprite_upper;
    int shaderblend    = state->blend_enable;
    int texenv_combine = 0;
    int texturing      = 0;
    const char* fogp   = hardext.highp ? "highp" : "mediump";
    char buff[1024];

    // ── ES3: preamble SEBELUM signature ───────────────────────────────────
    shad[0] = '\0';
    if (use_es3) {
        emit_es3_fragment_preamble(fog, fogdist, fogsource, shaderblend);
    }

    // ── Signature ─────────────────────────────────────────────────────────
    ShadAppend(fpeshader_signature);
    headers = gl4es_countline(shad);

    // ── Extensions ────────────────────────────────────────────────────────
    {
        int need_stream = 0;
        for (int i = 0; i < hardext.maxtex; i++) {
            int t = state->texture[i].textype;
            if (t == FPE_TEX_STRM) need_stream = 1;
            if (t) ++texturing;
        }
        if (need_stream && !use_es3) {
            // textureStreamIMG hanya ada di ES2 / GLSL 100
            ShadAppend("#extension GL_IMG_texture_stream2 : enable\n");
        }
    }

    if (shaderblend)
        ShadAppend("#extension GL_ARM_shader_framebuffer_fetch : enable\n");

    // ── Comments ──────────────────────────────────────────────────────────
    if (comments) {
        sprintf(buff,
            "// ** Fragment Shader (%s) **\n"
            "// lighting=%d alpha=%d secondary=%d planes=%s texturing=%d point=%d\n",
            use_es3 ? "ES3" : "ES2",
            lighting, alpha_test, secondary, fpe_binary(planes, 6), texturing, point);
        ShadAppend(buff); headers += gl4es_countline(buff);
    }

    // ── Varyings ──────────────────────────────────────────────────────────
    sprintf(buff, "%s vec4 Color;\n", vary_fs_in); ShadAppend(buff); headers++;
    if (twosided) {
        sprintf(buff, "%s vec4 BackColor;\n", vary_fs_in); ShadAppend(buff); headers++;
    }
    if (light_separate || secondary) {
        sprintf(buff, "%s vec4 SecColor;\n", vary_fs_in); ShadAppend(buff); headers++;
        if (twosided) {
            sprintf(buff, "%s vec4 SecBackColor;\n", vary_fs_in); ShadAppend(buff); headers++;
        }
    }

    // Fog varying
    if (fog) {
        if (fogsource == FPE_FOG_SRC_COORD)
            sprintf(buff, "%s %s float FogSrc;\n", vary_fs_in, fogp);
        else switch (fogdist) {
            case FPE_FOG_DIST_RADIAL:
                sprintf(buff, "%s %s vec3 FogSrc;\n", vary_fs_in, fogp); break;
            default:
                sprintf(buff, "%s %s float FogSrc;\n", vary_fs_in, fogp); break;
        }
        ShadAppend(buff);
    }

    // Clip plane varyings
    if (planes) {
        for (int i = 0; i < hardext.maxplanes; i++) {
            if ((planes >> i) & 1) {
                sprintf(buff, "%s mediump float clippedvertex_%d;\n", vary_fs_in, i);
                ShadAppend(buff); headers++;
            }
        }
    }

    // Texture coord varyings + samplers
    for (int i = 0; i < hardext.maxtex; i++) {
        int t = state->texture[i].textype;
        if (point && !pointsprite) t = 0;
        if (!is_default && t && !(need->need_texs & (1 << i))) t = 0;
        if (t) {
            sprintf(buff, "%s %s _gl4es_TexCoord_%d;\n", vary_fs_in, texvecsize[t-1], i);
            ShadAppend(buff);
            sprintf(buff, "uniform %s _gl4es_TexSampler_%d;\n", texsampler[t-1], i);
            ShadAppend(buff); headers++;

            int texenv = state->texenv[i].texenv;
            if (texenv >= FPE_COMBINE) {
                int n = 1 + texenv - FPE_COMBINE;
                if (n > texenv_combine) texenv_combine = n;
                if (state->texenv[i].texrgbscale) {
                    sprintf(buff, "uniform float _gl4es_TexEnvRGBScale_%d;\n", i);
                    ShadAppend(buff); headers++;
                }
                if (state->texenv[i].texalphascale) {
                    sprintf(buff, "uniform float _gl4es_TexEnvAlphaScale_%d;\n", i);
                    ShadAppend(buff); headers++;
                }
            }
        }
    }

    if (alpha_test && alpha_func > FPE_NEVER) {
        ShadAppend(gl4es_alphaRefSource); headers++;
    }

    // ── main() ────────────────────────────────────────────────────────────
    ShadAppend("void main() {\n");

    // Clip planes
    if (planes) {
        ShadAppend("if((");
        int k = 0;
        for (int i = 0; i < hardext.maxplanes; i++) {
            if ((planes >> i) & 1) {
                sprintf(buff, "%smin(0., clippedvertex_%d)", k ? "+" : "", i);
                ShadAppend(buff); k = 1;
            }
        }
        ShadAppend(")<0.) discard;\n");
    }

    // Initial color
    sprintf(buff, "vec4 fColor = %s;\n",
            twosided ? "(gl_FrontFacing)?Color:BackColor" : "Color");
    ShadAppend(buff);

    // ── Texture sampling + TexEnv ─────────────────────────────────────────
    if (texturing && (!point || pointsprite)) {
        // Fetch textures
        for (int i = 0; i < hardext.maxtex; i++) {
            int t = state->texture[i].textype;
            if (t) {
                if (point && pointsprite && pointsprite_coord) {
                    if (pointsprite_upper)
                        sprintf(buff, "vec4 texColor%d = %s(_gl4es_TexSampler_%d,"
                                " vec2(gl_PointCoord.x, 1.-gl_PointCoord.y));\n",
                                i, texnoproj[t-1], i);
                    else
                        sprintf(buff, "vec4 texColor%d = %s(_gl4es_TexSampler_%d, gl_PointCoord);\n",
                                i, texnoproj[t-1], i);
                } else {
                    sprintf(buff, "vec4 texColor%d = %s(_gl4es_TexSampler_%d, _gl4es_TexCoord_%d);\n",
                            i, texname[t-1], i, i);
                }
                ShadAppend(buff);
            }
        }

        // TexEnv combine temps
        if (texenv_combine > 0) {
            ShadAppend((texenv_combine == 2) ? "vec4 Arg0, Arg1, Arg2, Arg3;\n"
                                             : "vec4 Arg0, Arg1, Arg2;\n");
        }

        // TexEnv per-unit
        for (int i = 0; i < hardext.maxtex; i++) {
            int t = state->texture[i].textype;
            if (t) {
                int texenv   = state->texenv[i].texenv;
                int texformat = state->texture[i].texformat;
                if (comments) {
                    sprintf(buff, "// Texture %d: type=%X env=%X fmt=%X\n", i, t, texenv, texformat);
                    ShadAppend(buff);
                }
                int needclamp = 1;
                switch (texenv) {
                    case FPE_MODULATE:
                        if (texformat == FPE_TEX_RGB || texformat == FPE_TEX_LUM) {
                            sprintf(buff, "fColor.rgb *= texColor%d.rgb;\n", i); ShadAppend(buff);
                        } else if (texformat == FPE_TEX_ALPHA) {
                            sprintf(buff, "fColor.a *= texColor%d.a;\n", i); ShadAppend(buff);
                        } else {
                            sprintf(buff, "fColor *= texColor%d;\n", i); ShadAppend(buff);
                        }
                        needclamp = 0; break;
                    case FPE_ADD:
                        if (texformat != FPE_TEX_ALPHA) {
                            sprintf(buff, "fColor.rgb += texColor%d.rgb;\n", i); ShadAppend(buff);
                        }
                        sprintf(buff, (texformat == FPE_TEX_INTENSITY || texformat == FPE_TEX_DEPTH)
                                ? "fColor.a += texColor%d.a;\n"
                                : "fColor.a *= texColor%d.a;\n", i);
                        ShadAppend(buff); break;
                    case FPE_DECAL:
                        sprintf(buff, "fColor.rgb = mix(fColor.rgb, texColor%d.rgb, texColor%d.a);\n", i, i);
                        ShadAppend(buff); needclamp = 0; break;
                    case FPE_BLEND: {
                        sprintf(buff, "uniform lowp vec4 _gl4es_TextureEnvColor_%d;\n", i);
                        shad = gl4es_inplace_insert(gl4es_getline(shad, headers), buff, shad, &shad_cap);
                        headers += gl4es_countline(buff);
                        needclamp = 0;
                        if (texformat != FPE_TEX_ALPHA) {
                            sprintf(buff, "fColor.rgb = mix(fColor.rgb, _gl4es_TextureEnvColor_%d.rgb, texColor%d.rgb);\n", i, i);
                            ShadAppend(buff);
                        }
                        switch (texformat) {
                            case FPE_TEX_LUM: case FPE_TEX_RGB: break;
                            case FPE_TEX_INTENSITY: case FPE_TEX_DEPTH:
                                sprintf(buff, "fColor.a = mix(fColor.a, _gl4es_TextureEnvColor_%d.a, texColor%d.a);\n", i, i);
                                ShadAppend(buff); break;
                            default:
                                sprintf(buff, "fColor.a *= texColor%d.a;\n", i); ShadAppend(buff);
                        }
                        break;
                    }
                    case FPE_REPLACE:
                        if (texformat == FPE_TEX_RGB || texformat == FPE_TEX_LUM) {
                            sprintf(buff, "fColor.rgb = texColor%d.rgb;\n", i);
                        } else if (texformat == FPE_TEX_ALPHA) {
                            sprintf(buff, "fColor.a = texColor%d.a;\n", i);
                        } else {
                            sprintf(buff, "fColor = texColor%d;\n", i);
                        }
                        ShadAppend(buff); needclamp = 0; break;

                    case FPE_COMBINE:
                    case FPE_COMBINE4: {
                        int constant = 0;
                        int combine_rgb   = state->texcombine[i] & 0xf;
                        int combine_alpha = (state->texcombine[i] >> 4) & 0xf;
                        int src_r[4], op_r[4], src_a[4], op_a[4];
                        src_a[0]=state->texenv[i].texsrcalpha0; op_a[0]=state->texenv[i].texopalpha0;
                        src_r[0]=state->texenv[i].texsrcrgb0;   op_r[0]=state->texenv[i].texoprgb0;
                        src_a[1]=state->texenv[i].texsrcalpha1; op_a[1]=state->texenv[i].texopalpha1;
                        src_r[1]=state->texenv[i].texsrcrgb1;   op_r[1]=state->texenv[i].texoprgb1;
                        src_a[2]=state->texenv[i].texsrcalpha2; op_a[2]=state->texenv[i].texopalpha2;
                        src_r[2]=state->texenv[i].texsrcrgb2;   op_r[2]=state->texenv[i].texoprgb2;
                        src_a[3]=state->texenv[i].texsrcalpha3; op_a[3]=state->texenv[i].texopalpha3;
                        src_r[3]=state->texenv[i].texsrcrgb3;   op_r[3]=state->texenv[i].texoprgb3;

                        if (combine_rgb == FPE_CR_DOT3_RGBA) {
                            src_a[0]=src_a[1]=src_a[2]=-1; op_a[0]=op_a[1]=op_a[2]=-1;
                            src_r[2]=op_r[2]=-1; src_r[3]=op_r[3]=-1; src_a[3]=op_a[3]=-1;
                        } else {
                            if (combine_alpha == FPE_CR_REPLACE) { src_a[1]=src_a[2]=src_a[3]=-1; op_a[1]=op_a[2]=op_a[3]=-1; }
                            else if (combine_alpha >= FPE_CR_MOD_ADD || combine_alpha == FPE_CR_INTERPOLATE) { src_a[3]=-1; op_a[3]=-1; }
                            else if (!(texenv == FPE_COMBINE4 && (combine_alpha == FPE_CR_ADD || combine_alpha == FPE_CR_ADD_SIGNED))) { src_a[2]=src_a[3]=-1; op_a[2]=op_a[3]=-1; }
                            if (combine_rgb == FPE_CR_REPLACE) { src_r[1]=src_r[2]=src_r[3]=-1; op_r[1]=op_r[2]=op_r[3]=-1; }
                            else if (combine_rgb >= FPE_CR_MOD_ADD || combine_rgb == FPE_CR_INTERPOLATE) { src_r[3]=-1; op_r[3]=-1; }
                            else if (!(texenv == FPE_COMBINE4 && (combine_rgb == FPE_CR_ADD || combine_rgb == FPE_CR_ADD_SIGNED))) { src_r[2]=src_r[3]=-1; op_r[2]=op_r[3]=-1; }
                        }
                        for (int j = 0; j < 4; j++)
                            if (src_a[j] == FPE_SRC_CONSTANT || src_r[j] == FPE_SRC_CONSTANT) constant = 1;

                        if (comments) {
                            sprintf(buff, " // Combine RGB: fct=%d 0=%d/%d 1=%d/%d 2=%d/%d 3=%d/%d\n",
                                    combine_rgb, src_r[0],op_r[0], src_r[1],op_r[1], src_r[2],op_r[2], src_r[3],op_r[3]);
                            ShadAppend(buff);
                            sprintf(buff, " // Combine Alpha: fct=%d 0=%d/%d 1=%d/%d 2=%d/%d 3=%d/%d\n",
                                    combine_alpha, src_a[0],op_a[0], src_a[1],op_a[1], src_a[2],op_a[2], src_a[3],op_a[3]);
                            ShadAppend(buff);
                        }
                        if (constant) {
                            sprintf(buff, "uniform lowp vec4 _gl4es_TextureEnvColor_%d;\n", i);
                            shad = gl4es_inplace_insert(gl4es_getline(shad, headers), buff, shad, &shad_cap);
                            headers += gl4es_countline(buff);
                        }
                        for (int j = 0; j < 4; j++) {
                            if (src_r[j] == src_a[j] && op_r[j] == FPE_OP_SRCCOLOR && op_a[j] == FPE_OP_ALPHA) {
                                sprintf(buff, "Arg%d = %s;\n", j, fpe_texenvSrc(src_r[j], i, twosided));
                                ShadAppend(buff);
                            } else if (src_r[j] == src_a[j] && op_r[j] == FPE_OP_MINUSCOLOR && op_a[j] == FPE_OP_MINUSALPHA) {
                                sprintf(buff, "Arg%d = vec4(1.) - %s;\n", j, fpe_texenvSrc(src_r[j], i, twosided));
                                ShadAppend(buff);
                            } else {
                                if (op_r[j] != -1) switch (op_r[j]) {
                                    case FPE_OP_SRCCOLOR:   sprintf(buff, "Arg%d.rgb = %s.rgb;\n", j, fpe_texenvSrc(src_r[j], i, twosided)); ShadAppend(buff); break;
                                    case FPE_OP_MINUSCOLOR: sprintf(buff, "Arg%d.rgb = vec3(1.) - %s.rgb;\n", j, fpe_texenvSrc(src_r[j], i, twosided)); ShadAppend(buff); break;
                                    case FPE_OP_ALPHA:      sprintf(buff, "Arg%d.rgb = vec3(%s.a);\n", j, fpe_texenvSrc(src_r[j], i, twosided)); ShadAppend(buff); break;
                                    case FPE_OP_MINUSALPHA: sprintf(buff, "Arg%d.rgb = vec3(1. - %s.a);\n", j, fpe_texenvSrc(src_r[j], i, twosided)); ShadAppend(buff); break;
                                }
                                if (op_a[j] != -1) switch (op_a[j]) {
                                    case FPE_OP_ALPHA:      sprintf(buff, "Arg%d.a = %s.a;\n", j, fpe_texenvSrc(src_a[j], i, twosided)); ShadAppend(buff); break;
                                    case FPE_OP_MINUSALPHA: sprintf(buff, "Arg%d.a = 1. - %s.a;\n", j, fpe_texenvSrc(src_a[j], i, twosided)); ShadAppend(buff); break;
                                }
                            }
                        }
                        // Combine operations (body sama dengan original — hanya diringkas)
                        #define COMBINE_BOTH(op_eq, op4_eq, single_eq)                             \
                            if (texenv == FPE_COMBINE4) ShadAppend(op4_eq);                        \
                            else ShadAppend(single_eq);

                        if (combine_rgb != FPE_CR_DOT3_RGBA && combine_rgb != FPE_CR_DOT3_RGB
                            && combine_rgb == combine_alpha) {
                            switch (combine_alpha) {
                                case FPE_CR_REPLACE:    ShadAppend("fColor = Arg0;\n"); break;
                                case FPE_CR_MODULATE:   ShadAppend("fColor = Arg0 * Arg1;\n"); break;
                                case FPE_CR_ADD:        COMBINE_BOTH(,"fColor = Arg0*Arg1 + Arg2*Arg3;\n","fColor = Arg0 + Arg1;\n"); break;
                                case FPE_CR_ADD_SIGNED: COMBINE_BOTH(,"fColor = Arg0*Arg1 + Arg2*Arg3 - vec4(0.5);\n","fColor = Arg0 + Arg1 - vec4(0.5);\n"); break;
                                case FPE_CR_INTERPOLATE:ShadAppend("fColor = Arg0*Arg2 + Arg1*(vec4(1.)-Arg2);\n"); break;
                                case FPE_CR_SUBTRACT:   ShadAppend("fColor = Arg0 - Arg1;\n"); break;
                                case FPE_CR_MOD_ADD:    ShadAppend("fColor = Arg0*Arg2 + Arg1;\n"); break;
                                case FPE_CR_MOD_ADD_SIGNED: ShadAppend("fColor = Arg0*Arg2 + Arg1 - vec4(0.5);\n"); break;
                                case FPE_CR_MOD_SUB:    ShadAppend("fColor = Arg0*Arg2 - Arg1;\n"); break;
                            }
                        } else {
                            switch (combine_rgb) {
                                case FPE_CR_REPLACE:    ShadAppend("fColor.rgb = Arg0.rgb;\n"); break;
                                case FPE_CR_MODULATE:   ShadAppend("fColor.rgb = Arg0.rgb * Arg1.rgb;\n"); break;
                                case FPE_CR_ADD:        COMBINE_BOTH(,"fColor.rgb = Arg0.rgb*Arg1.rgb + Arg2.rgb*Arg3.rgb;\n","fColor.rgb = Arg0.rgb + Arg1.rgb;\n"); break;
                                case FPE_CR_ADD_SIGNED: COMBINE_BOTH(,"fColor.rgb = Arg0.rgb*Arg1.rgb + Arg2.rgb*Arg3.rgb - vec3(0.5);\n","fColor.rgb = Arg0.rgb + Arg1.rgb - vec3(0.5);\n"); break;
                                case FPE_CR_INTERPOLATE:ShadAppend("fColor.rgb = Arg0.rgb*Arg2.rgb + Arg1.rgb*(vec3(1.)-Arg2.rgb);\n"); break;
                                case FPE_CR_SUBTRACT:   ShadAppend("fColor.rgb = Arg0.rgb - Arg1.rgb;\n"); break;
                                case FPE_CR_DOT3_RGB:   ShadAppend("fColor.rgb = vec3(4.*((Arg0.r-0.5)*(Arg1.r-0.5)+(Arg0.g-0.5)*(Arg1.g-0.5)+(Arg0.b-0.5)*(Arg1.b-0.5)));\n"); break;
                                case FPE_CR_DOT3_RGBA:  ShadAppend("fColor = vec4(4.*((Arg0.r-0.5)*(Arg1.r-0.5)+(Arg0.g-0.5)*(Arg1.g-0.5)+(Arg0.b-0.5)*(Arg1.b-0.5)));\n"); break;
                                case FPE_CR_MOD_ADD:    ShadAppend("fColor.rgb = Arg0.rgb*Arg2.rgb + Arg1.rgb;\n"); break;
                                case FPE_CR_MOD_ADD_SIGNED: ShadAppend("fColor.rgb = Arg0.rgb*Arg2.rgb + Arg1.rgb - vec3(0.5);\n"); break;
                                case FPE_CR_MOD_SUB:    ShadAppend("fColor.rgb = Arg0.rgb*Arg2.rgb - Arg1.rgb;\n"); break;
                            }
                            if (combine_rgb != FPE_CR_DOT3_RGBA) switch (combine_alpha) {
                                case FPE_CR_REPLACE:    ShadAppend("fColor.a = Arg0.a;\n"); break;
                                case FPE_CR_MODULATE:   ShadAppend("fColor.a = Arg0.a * Arg1.a;\n"); break;
                                case FPE_CR_ADD:        COMBINE_BOTH(,"fColor.a = Arg0.a*Arg1.a + Arg2.a*Arg3.a;\n","fColor.a = Arg0.a + Arg1.a;\n"); break;
                                case FPE_CR_ADD_SIGNED: COMBINE_BOTH(,"fColor.a = Arg0.a*Arg1.a + Arg2.a*Arg3.a - 0.5;\n","fColor.a = Arg0.a + Arg1.a - 0.5;\n"); break;
                                case FPE_CR_INTERPOLATE:ShadAppend("fColor.a = Arg0.a*Arg2.a + Arg1.a*(1.-Arg2.a);\n"); break;
                                case FPE_CR_SUBTRACT:   ShadAppend("fColor.a = Arg0.a - Arg1.a;\n"); break;
                                case FPE_CR_MOD_ADD:    ShadAppend("fColor.a = Arg0.a*Arg2.a + Arg1.a;\n"); break;
                                case FPE_CR_MOD_ADD_SIGNED: ShadAppend("fColor.a = Arg0.a*Arg2.a + Arg1.a - 0.5;\n"); break;
                                case FPE_CR_MOD_SUB:    ShadAppend("fColor.a = Arg0.a*Arg2.a - Arg1.a;\n"); break;
                            }
                        }
                        #undef COMBINE_BOTH

                        if (state->texenv[i].texrgbscale && state->texenv[i].texalphascale) {
                            sprintf(buff, "fColor *= _gl4es_TexEnvRGBScale_%d;\n", i); ShadAppend(buff);
                        } else {
                            if (state->texenv[i].texrgbscale)   { sprintf(buff, "fColor.rgb *= _gl4es_TexEnvRGBScale_%d;\n", i);   ShadAppend(buff); }
                            if (state->texenv[i].texalphascale) { sprintf(buff, "fColor.a *= _gl4es_TexEnvAlphaScale_%d;\n", i); ShadAppend(buff); }
                        }
                        break;
                    }
                }
                if (needclamp) ShadAppend("fColor = clamp(fColor, 0., 1.);\n");
            }
        }
    }

    // ── Alpha Test ────────────────────────────────────────────────────────
    if (alpha_test) {
        if (comments) { sprintf(buff, "// Alpha Test fct=%X\n", alpha_func); ShadAppend(buff); }
        if (alpha_func == FPE_ALWAYS) {
            // pass
        } else if (alpha_func == FPE_NEVER) {
            ShadAppend("discard;\n");
        } else {
            const char* ops[] = { ">=", "!=", ">", "<=", "==", "<" };
            sprintf(buff, "if (floor(fColor.a*255.) %s _gl4es_AlphaRef) discard;\n",
                    ops[alpha_func - FPE_LESS]);
            ShadAppend(buff);
        }
    }

    // ── Secondary color ───────────────────────────────────────────────────
    if (light_separate || secondary) {
        if (comments) { sprintf(buff, "// Secondary color\n"); ShadAppend(buff); }
        sprintf(buff, "fColor.rgb += (%s).rgb;\n",
                twosided ? "(gl_FrontFacing)?SecColor:SecBackColor" : "SecColor");
        ShadAppend(buff);
        ShadAppend("fColor.rgb = clamp(fColor.rgb, 0., 1.);\n");
    }

    // ── Fog ───────────────────────────────────────────────────────────────
    if (fog) {
        if (comments) { sprintf(buff, "// Fog mode=%X src=%X\n", fogmode, fogsource); ShadAppend(buff); }
        char fogsrc[50];
        if (fogsource == FPE_FOG_SRC_COORD) strcpy(fogsrc, "FogSrc");
        else switch (fogdist) {
            case FPE_FOG_DIST_RADIAL: strcpy(fogsrc, "length(FogSrc)"); break;
            default: strcpy(fogsrc, "abs(FogSrc)"); break;
        }
        sprintf(buff, "%s float fog_c = %s;\n", fogp, fogsrc); ShadAppend(buff);
        switch (fogmode) {
            case FPE_FOG_EXP:
                sprintf(buff, "%s float FogF = clamp(exp(-gl_Fog.density * fog_c), 0., 1.);\n", fogp);
                break;
            case FPE_FOG_EXP2:
                sprintf(buff, "%s float FogF = clamp(exp(-(gl_Fog.density*fog_c)*(gl_Fog.density*fog_c)), 0., 1.);\n", fogp);
                break;
            case FPE_FOG_LINEAR:
                sprintf(buff, "%s float FogF = clamp((gl_Fog.end - fog_c) %s, 0., 1.);\n",
                        fogp, hardext.highp ? "* gl_Fog.scale" : "/ (gl_Fog.end - gl_Fog.start)");
                break;
        }
        ShadAppend(buff);
        ShadAppend("fColor.rgb = mix(gl_Fog.color.rgb, fColor.rgb, FogF);\n");
    }

    // ── Shader blend (ARM_shader_framebuffer_fetch) ───────────────────────
    if (shaderblend) {
        if (comments) {
            sprintf(buff, "// Blend: src=%d/%d dst=%d/%d eq=%d/%d\n",
                    state->blendsrcrgb, state->blendsrcalpha,
                    state->blenddstrgb, state->blenddstalpha,
                    state->blendeqrgb, state->blendeqalpha);
            ShadAppend(buff);
        }
        const char* frgcolor = "fColor";
        const char* dstcolor = "gl_LastFragColorARM";

        // Macro untuk generate blend factor code
        #define BLEND_FACTOR(blend, frgc, dstc, blendrgb, blendalpha)                      \
        do {                                                                                \
            if ((blendrgb) == (blendalpha)) {                                               \
                int nv4 = 0;                                                                \
                switch (blendrgb) {                                                         \
                    case FPE_BLEND_ZERO:               sprintf(buff, " %s = 0.0;\n", blend); break;             \
                    case FPE_BLEND_ONE:                sprintf(buff, " %s = 1.0;\n", blend); break;             \
                    case FPE_BLEND_SRC_COLOR:          nv4=1; sprintf(buff, " %s = %s;\n", blend, frgc); break; \
                    case FPE_BLEND_ONE_MINUS_SRC_COLOR:nv4=1; sprintf(buff, " %s = vec4(1.)-%s;\n", blend, frgc); break;\
                    case FPE_BLEND_DST_COLOR:          nv4=1; sprintf(buff, " %s = %s;\n", blend, dstc); break; \
                    case FPE_BLEND_ONE_MINUS_DST_COLOR:nv4=1; sprintf(buff, " %s = vec4(1.)-%s;\n", blend, dstc); break;\
                    case FPE_BLEND_SRC_ALPHA:          sprintf(buff, " %s = %s.a;\n", blend, frgc); break;      \
                    case FPE_BLEND_ONE_MINUS_SRC_ALPHA:sprintf(buff, " %s = 1.0-%s.a;\n", blend, frgc); break;  \
                    case FPE_BLEND_DST_ALPHA:          sprintf(buff, " %s = %s.a;\n", blend, dstc); break;      \
                    case FPE_BLEND_ONE_MINUS_DST_ALPHA:sprintf(buff, " %s = 1.0-%s.a;\n", blend, dstc); break;  \
                    case FPE_BLEND_CONSTANT_COLOR:     nv4=1; sprintf(buff, " %s = _gl4es_BlendColor;\n", blend); break;\
                    case FPE_BLEND_ONE_MINUS_CONSTANT_COLOR: nv4=1; sprintf(buff, " %s = vec4(1.)-_gl4es_BlendColor;\n", blend); break;\
                    case FPE_BLEND_CONSTANT_ALPHA:     sprintf(buff, " %s = _gl4es_BlendColor.a;\n", blend); break;\
                    case FPE_BLEND_ONE_MINUS_CONSTANT_ALPHA: sprintf(buff, " %s = 1.0-_gl4es_BlendColor.a;\n", blend); break;\
                    case FPE_BLEND_SRC_ALPHA_SATURATE: sprintf(buff, " %s = min(%s.a,1.0-%s.a);\n", blend, frgc, dstc); break;\
                }                                                                           \
                char buff2[60]; sprintf(buff2, nv4 ? "lowp vec4 %s;\n" : "lowp float %s;\n", blend); \
                ShadAppend(buff2); ShadAppend(buff);                                        \
            }                                                                               \
        } while(0)

        BLEND_FACTOR("srcblend", frgcolor, dstcolor, state->blendsrcrgb, state->blendsrcalpha);
        BLEND_FACTOR("dstblend", frgcolor, dstcolor, state->blenddstrgb, state->blenddstalpha);
        #undef BLEND_FACTOR

        // Blend equation
        #define BLENDEQ(ch, fch, sch, dch)                                                           \
        switch (state->blendeqrgb) {                                                                  \
            case FPE_BLENDEQ_FUNC_ADD:             sprintf(buff, ch"=srcblend"sch"*"fch sch"+dstblend"dch"*"dstcolor sch";\n", frgcolor, frgcolor, dstcolor); break;\
            case FPE_BLENDEQ_FUNC_SUBTRACT:        sprintf(buff, ch"=srcblend"sch"*"fch sch"-dstblend"dch"*"dstcolor sch";\n", frgcolor, frgcolor, dstcolor); break;\
            case FPE_BLENDEQ_FUNC_REVERSE_SUBTRACT:sprintf(buff, ch"=dstblend"dch"*"dstcolor sch"-srcblend"sch"*"fch sch";\n", frgcolor, dstcolor, frgcolor); break;\
            case FPE_BLENDEQ_MIN: sprintf(buff, ch"=min("fch sch","dstcolor sch");\n", frgcolor, frgcolor, dstcolor); break;\
            case FPE_BLENDEQ_MAX: sprintf(buff, ch"=max("fch sch","dstcolor sch");\n", frgcolor, frgcolor, dstcolor); break;\
        } ShadAppend(buff);

        if (state->blendeqrgb == state->blendeqalpha) {
            BLENDEQ("%s", "%s", "", "")
        } else {
            BLENDEQ("%s.rgb", "%s", ".rgb", ".rgb")
            switch (state->blendeqalpha) {
                case FPE_BLENDEQ_FUNC_ADD:             sprintf(buff, "%s.a=srcblend.a*%s.a+dstblend.a*%s.a;\n", frgcolor,frgcolor,dstcolor); break;
                case FPE_BLENDEQ_FUNC_SUBTRACT:        sprintf(buff, "%s.a=srcblend.a*%s.a-dstblend.a*%s.a;\n", frgcolor,frgcolor,dstcolor); break;
                case FPE_BLENDEQ_FUNC_REVERSE_SUBTRACT:sprintf(buff, "%s.a=dstblend.a*%s.a-srcblend.a*%s.a;\n", frgcolor,dstcolor,frgcolor); break;
                case FPE_BLENDEQ_MIN: sprintf(buff, "%s.a=min(%s.a,%s.a);\n", frgcolor,frgcolor,dstcolor); break;
                case FPE_BLENDEQ_MAX: sprintf(buff, "%s.a=max(%s.a,%s.a);\n", frgcolor,frgcolor,dstcolor); break;
            }
            ShadAppend(buff);
        }
        #undef BLENDEQ
    }

    // ── Fragment output ───────────────────────────────────────────────────
    // ES3: "out vec4 _gl4es_FragColor" (sudah dideklarasikan di preamble)
    // ES2: langsung ke gl_FragColor
    sprintf(buff, "%s = fColor;\n", frag_out_var);
    ShadAppend(buff);
    ShadAppend("}\n");

    DBG(printf("FPE FragmentShader:\n%s\n", shad);)
    return (const char* const*)&shad;
}

// =============================================================================
//  fpe_CustomVertexShader — Wrap user vertex shader with FPE additions
// =============================================================================
const char* const* fpe_CustomVertexShader(const char* initial, fpe_state_t* state,
                                            int default_fragment)
{
    fpe_shader_init_path();

    int planes = state->plane;
    char buff[1024];
    if (!shad_cap) shad_cap = 1024;
    if (!shad) shad = (char*)malloc(shad_cap);

    int headline = gl4es_getline_for(initial, "main");
    if (headline) --headline;

    shad[0] = '\0';
    ShadAppend(initial);

    int color = default_fragment ? (strstr(initial, "_gl4es_Color") ? 0 : 1) : 0;

    if (planes) {
        for (int i = 0; i < hardext.maxplanes; i++) {
            if ((planes >> i) & 1) {
                sprintf(buff, "uniform highp vec4 _gl4es_ClipPlane_%d;\n", i);
                ShadAppend(buff); ++headline;
                sprintf(buff, "%s mediump float clippedvertex_%d;\n", vary_vs_out, i);
                ShadAppend(buff); ++headline;
            }
        }
    }
    if (color) {
        if (use_es3) {
            // ES3: attribute → layout in
            sprintf(buff, "layout(location=3) in lowp vec4 _gl4es_Color;\n");
        } else {
            sprintf(buff, "attribute lowp vec4 _gl4es_Color;\n");
        }
        ShadAppend(buff); ++headline;
        sprintf(buff, "%s lowp vec4 Color;\n", vary_vs_out);
        ShadAppend(buff); ++headline;
    }

    if (planes || color)
        shad = gl4es_inplace_replace(shad, &shad_cap, "main", "_gl4es_main");

    if (strstr(shad, "_gl4es_main")) {
        ShadAppend("\nvoid main() {\n");
        if (color) {
            sprintf(buff, "Color = _gl4es_Color;\n"); ShadAppend(buff);
        }
        ShadAppend("_gl4es_main();\n");
        if (planes) {
            int clipvertex = strstr(shad, "gl4es_ClipVertex") ? 1 : 0;
            for (int i = 0; i < hardext.maxplanes; i++) {
                if ((planes >> i) & 1) {
                    sprintf(buff, "clippedvertex_%d = dot(%s, _gl4es_ClipPlane_%d);\n",
                            i, clipvertex ? "gl4es_ClipVertex" : "gl_ModelViewMatrix * gl_Vertex", i);
                    ShadAppend(buff);
                }
            }
        }
        ShadAppend("}\n");
    }

    return (const char* const*)&shad;
}

// =============================================================================
//  fpe_CustomFragmentShader — Wrap user fragment shader with FPE additions
// =============================================================================
const char* const* fpe_CustomFragmentShader(const char* initial, fpe_state_t* state)
{
    fpe_shader_init_path();

    if (!shad_cap) shad_cap = 1024;
    if (!shad) shad = (char*)malloc(shad_cap);

    int planes      = state->plane;
    int alpha_test  = state->alphatest;
    int alpha_func  = state->alphafunc;
    int shaderblend = state->blend_enable;
    char buff[1024];

    int headline = gl4es_getline_for(initial, "main");
    if (headline) --headline;

    shad[0] = '\0';
    if (shaderblend)
        ShadAppend("#extension GL_ARM_shader_framebuffer_fetch : enable\n");
    ShadAppend(initial);

    if (planes) {
        for (int i = 0; i < hardext.maxplanes; i++) {
            if ((planes >> i) & 1) {
                sprintf(buff, "%s mediump float clippedvertex_%d;\n", vary_fs_in, i);
                ShadAppend(buff);
            }
        }
    }

    // BlendColor uniform jika dibutuhkan
    if (shaderblend && (
        (state->blendsrcrgb >= FPE_BLEND_CONSTANT_COLOR && state->blendsrcrgb <= FPE_BLEND_ONE_MINUS_CONSTANT_ALPHA) ||
        (state->blenddstrgb >= FPE_BLEND_CONSTANT_COLOR && state->blenddstrgb <= FPE_BLEND_ONE_MINUS_CONSTANT_ALPHA) ||
        (state->blendsrcalpha >= FPE_BLEND_CONSTANT_COLOR && state->blendsrcalpha <= FPE_BLEND_ONE_MINUS_CONSTANT_ALPHA) ||
        (state->blenddstalpha >= FPE_BLEND_CONSTANT_COLOR && state->blenddstalpha <= FPE_BLEND_ONE_MINUS_CONSTANT_ALPHA))) {
        ShadAppend("uniform mediump vec4 _gl4es_BlendColor;\n");
    }

    int is_fragcolor = strstr(shad, "gl_FragColor") ? 1 : 0;

    if (alpha_test || planes || shaderblend) {
        shad = gl4es_inplace_replace(shad, &shad_cap, "main", "_gl4es_main");
        if (is_fragcolor) {
            int l_main = gl4es_getline_for(shad, gl4es_prev_str(shad, strstr(shad, "_gl4es_main"))) - 1;
            shad = gl4es_inplace_insert(gl4es_getline(shad, l_main),
                                         "lowp vec4 _gl4es_FragColor;\n", shad, &shad_cap);
            shad = gl4es_inplace_replace(shad, &shad_cap, "gl_FragColor", "_gl4es_FragColor");
        }
    }

    if (strstr(shad, "_gl4es_main")) {
        ShadAppend("void main() {\n");
        ShadAppend(" _gl4es_main();\n");

        if (planes) {
            ShadAppend(" if((");
            int k = 0;
            for (int i = 0; i < hardext.maxplanes; i++) {
                if ((planes >> i) & 1) {
                    sprintf(buff, "%smin(0., clippedvertex_%d)", k ? "+" : "", i);
                    ShadAppend(buff); k = 1;
                }
            }
            ShadAppend(")<0.) discard;\n");
        }

        if (alpha_test) {
            if (alpha_func > FPE_NEVER) {
                shad = gl4es_inplace_insert(gl4es_getline(shad, headline),
                                             gl4es_alphaRefSource, shad, &shad_cap);
                headline += gl4es_countline(gl4es_alphaRefSource);
            }
            if (comments) { sprintf(buff, " // Alpha Test fct=%X\n", alpha_func); ShadAppend(buff); }
            if (alpha_func == FPE_ALWAYS) {
                // pass
            } else if (alpha_func == FPE_NEVER) {
                ShadAppend(" discard;\n");
            } else {
                const char* ops[] = { ">=", "!=", ">", "<=", "==", "<" };
                sprintf(buff, " if (floor(%s.a*255.) %s _gl4es_AlphaRef) discard;\n",
                        is_fragcolor ? "_gl4es_FragColor" : "gl_FragData[0]",
                        ops[alpha_func - FPE_LESS]);
                ShadAppend(buff);
            }
        }

        if (shaderblend) {
            const char* frgcolor = is_fragcolor ? "_gl4es_FragColor" : "gl_FragData[0]";
            const char* dstcolor = "gl_LastFragColorARM";
            for (int i = 0; i < 2; ++i) {
                const char* blend   = i ? "dstblend" : "srcblend";
                int blendrgb   = i ? state->blenddstrgb   : state->blendsrcrgb;
                int blendalpha = i ? state->blenddstalpha  : state->blendsrcalpha;
                if (blendrgb == blendalpha) {
                    int nv4 = 0;
                    switch (blendrgb) {
                        case FPE_BLEND_ZERO:                  sprintf(buff, " %s = 0.0;\n", blend); break;
                        case FPE_BLEND_ONE:                   sprintf(buff, " %s = 1.0;\n", blend); break;
                        case FPE_BLEND_SRC_COLOR:          nv4=1; sprintf(buff, " %s = %s;\n", blend, frgcolor); break;
                        case FPE_BLEND_ONE_MINUS_SRC_COLOR:nv4=1; sprintf(buff, " %s = vec4(1.0)-%s;\n", blend, frgcolor); break;
                        case FPE_BLEND_DST_COLOR:          nv4=1; sprintf(buff, " %s = %s;\n", blend, dstcolor); break;
                        case FPE_BLEND_ONE_MINUS_DST_COLOR:nv4=1; sprintf(buff, " %s = vec4(1.0)-%s;\n", blend, dstcolor); break;
                        case FPE_BLEND_SRC_ALPHA:             sprintf(buff, " %s = %s.a;\n", blend, frgcolor); break;
                        case FPE_BLEND_ONE_MINUS_SRC_ALPHA:   sprintf(buff, " %s = 1.0 - %s.a;\n", blend, frgcolor); break;
                        case FPE_BLEND_DST_ALPHA:             sprintf(buff, " %s = %s.a;\n", blend, dstcolor); break;
                        case FPE_BLEND_ONE_MINUS_DST_ALPHA:   sprintf(buff, " %s = 1.0 - %s.a;\n", blend, dstcolor); break;
                        case FPE_BLEND_CONSTANT_COLOR:     nv4=1; sprintf(buff, " %s = _gl4es_BlendColor;\n", blend); break;
                        case FPE_BLEND_ONE_MINUS_CONSTANT_COLOR: nv4=1; sprintf(buff, " %s = vec4(1.0)-_gl4es_BlendColor;\n", blend); break;
                        case FPE_BLEND_CONSTANT_ALPHA:        sprintf(buff, " %s = _gl4es_BlendColor.a;\n", blend); break;
                        case FPE_BLEND_ONE_MINUS_CONSTANT_ALPHA: sprintf(buff, " %s = 1.0 - _gl4es_BlendColor.a;\n", blend); break;
                        case FPE_BLEND_SRC_ALPHA_SATURATE:    sprintf(buff, " %s = min(%s.a, 1.0-%s.a);\n", blend, frgcolor, dstcolor); break;
                        default: sprintf(buff, " %s = 1.0;\n", blend); break;
                    }
                    char buff2[64];
                    sprintf(buff2, nv4 ? " lowp vec4 %s;\n" : " lowp float %s;\n", blend);
                    ShadAppend(buff2);
                    ShadAppend(buff);
                } else {
                    sprintf(buff, " lowp vec4 %s;\n", blend); ShadAppend(buff);
                    // RGB
                    switch (blendrgb) {
                        case FPE_BLEND_ZERO:               sprintf(buff, " %s.rgb = vec3(0.0);\n", blend); break;
                        case FPE_BLEND_ONE:                sprintf(buff, " %s.rgb = vec3(1.0);\n", blend); break;
                        case FPE_BLEND_SRC_COLOR:          sprintf(buff, " %s.rgb = %s.rgb;\n", blend, frgcolor); break;
                        case FPE_BLEND_ONE_MINUS_SRC_COLOR:sprintf(buff, " %s.rgb = vec3(1.0)-%s.rgb;\n", blend, frgcolor); break;
                        case FPE_BLEND_DST_COLOR:          sprintf(buff, " %s.rgb = %s.rgb;\n", blend, dstcolor); break;
                        case FPE_BLEND_ONE_MINUS_DST_COLOR:sprintf(buff, " %s.rgb = vec3(1.0)-%s.rgb;\n", blend, dstcolor); break;
                        case FPE_BLEND_SRC_ALPHA:          sprintf(buff, " %s.rgb = vec3(%s.a);\n", blend, frgcolor); break;
                        case FPE_BLEND_ONE_MINUS_SRC_ALPHA:sprintf(buff, " %s.rgb = vec3(1.0 - %s.a);\n", blend, frgcolor); break;
                        case FPE_BLEND_DST_ALPHA:          sprintf(buff, " %s.rgb = vec3(%s.a);\n", blend, dstcolor); break;
                        case FPE_BLEND_ONE_MINUS_DST_ALPHA:sprintf(buff, " %s.rgb = vec3(1.0 - %s.a);\n", blend, dstcolor); break;
                        case FPE_BLEND_CONSTANT_COLOR:     sprintf(buff, " %s.rgb = _gl4es_BlendColor.rgb;\n", blend); break;
                        case FPE_BLEND_ONE_MINUS_CONSTANT_COLOR: sprintf(buff, " %s.rgb = vec3(1.0)-_gl4es_BlendColor.rgb;\n", blend); break;
                        case FPE_BLEND_CONSTANT_ALPHA:     sprintf(buff, " %s.rgb = vec3(_gl4es_BlendColor.a);\n", blend); break;
                        case FPE_BLEND_ONE_MINUS_CONSTANT_ALPHA: sprintf(buff, " %s.rgb = vec3(1.0 - _gl4es_BlendColor.a);\n", blend); break;
                        case FPE_BLEND_SRC_ALPHA_SATURATE: sprintf(buff, " %s.rgb = vec3(min(%s.a, 1.0-%s.a));\n", blend, frgcolor, dstcolor); break;
                        default: sprintf(buff, " %s.rgb = vec3(1.0);\n", blend); break;
                    }
                    ShadAppend(buff);
                    // Alpha
                    switch (blendalpha) {
                        case FPE_BLEND_ZERO:               sprintf(buff, " %s.a = 0.0;\n", blend); break;
                        case FPE_BLEND_ONE:                sprintf(buff, " %s.a = 1.0;\n", blend); break;
                        case FPE_BLEND_SRC_COLOR:          sprintf(buff, " %s.a = %s.a;\n", blend, frgcolor); break;
                        case FPE_BLEND_ONE_MINUS_SRC_COLOR:sprintf(buff, " %s.a = 1.0-%s.a;\n", blend, frgcolor); break;
                        case FPE_BLEND_DST_COLOR:          sprintf(buff, " %s.a = %s.a;\n", blend, dstcolor); break;
                        case FPE_BLEND_ONE_MINUS_DST_COLOR:sprintf(buff, " %s.a = 1.0-%s.a;\n", blend, dstcolor); break;
                        case FPE_BLEND_SRC_ALPHA:          sprintf(buff, " %s.a = %s.a;\n", blend, frgcolor); break;
                        case FPE_BLEND_ONE_MINUS_SRC_ALPHA:sprintf(buff, " %s.a = 1.0 - %s.a;\n", blend, frgcolor); break;
                        case FPE_BLEND_DST_ALPHA:          sprintf(buff, " %s.a = %s.a;\n", blend, dstcolor); break;
                        case FPE_BLEND_ONE_MINUS_DST_ALPHA:sprintf(buff, " %s.a = 1.0 - %s.a;\n", blend, dstcolor); break;
                        case FPE_BLEND_CONSTANT_COLOR:     sprintf(buff, " %s.a = _gl4es_BlendColor.a;\n", blend); break;
                        case FPE_BLEND_ONE_MINUS_CONSTANT_COLOR: sprintf(buff, " %s.a = 1.0-_gl4es_BlendColor.a;\n", blend); break;
                        case FPE_BLEND_CONSTANT_ALPHA:     sprintf(buff, " %s.a = _gl4es_BlendColor.a;\n", blend); break;
                        case FPE_BLEND_ONE_MINUS_CONSTANT_ALPHA: sprintf(buff, " %s.a = 1.0 - _gl4es_BlendColor.a;\n", blend); break;
                        case FPE_BLEND_SRC_ALPHA_SATURATE: sprintf(buff, " %s.a = min(%s.a, 1.0-%s.a);\n", blend, frgcolor, dstcolor); break;
                        default: sprintf(buff, " %s.a = 1.0;\n", blend); break;
                    }
                    ShadAppend(buff);
                }
            }
            // Blend equation
            if (state->blendeqrgb == state->blendeqalpha) {
                switch (state->blendeqrgb) {
                    case FPE_BLENDEQ_FUNC_ADD:             sprintf(buff, " %s = srcblend*%s + dstblend*%s;\n", frgcolor, frgcolor, dstcolor); break;
                    case FPE_BLENDEQ_FUNC_SUBTRACT:        sprintf(buff, " %s = srcblend*%s - dstblend*%s;\n", frgcolor, frgcolor, dstcolor); break;
                    case FPE_BLENDEQ_FUNC_REVERSE_SUBTRACT:sprintf(buff, " %s = dstblend*%s - srcblend*%s;\n", frgcolor, dstcolor, frgcolor); break;
                    case FPE_BLENDEQ_MIN: sprintf(buff, " %s = min(%s, %s);\n", frgcolor, frgcolor, dstcolor); break;
                    case FPE_BLENDEQ_MAX: sprintf(buff, " %s = max(%s, %s);\n", frgcolor, frgcolor, dstcolor); break;
                    default: buff[0] = '\0'; break;
                }
                if (buff[0]) ShadAppend(buff);
            } else {
                switch (state->blendeqrgb) {
                    case FPE_BLENDEQ_FUNC_ADD:             sprintf(buff, " %s.rgb = srcblend.rgb*%s.rgb + dstblend.rgb*%s.rgb;\n", frgcolor, frgcolor, dstcolor); break;
                    case FPE_BLENDEQ_FUNC_SUBTRACT:        sprintf(buff, " %s.rgb = srcblend.rgb*%s.rgb - dstblend.rgb*%s.rgb;\n", frgcolor, frgcolor, dstcolor); break;
                    case FPE_BLENDEQ_FUNC_REVERSE_SUBTRACT:sprintf(buff, " %s.rgb = dstblend.rgb*%s.rgb - srcblend.rgb*%s.rgb;\n", frgcolor, dstcolor, frgcolor); break;
                    case FPE_BLENDEQ_MIN: sprintf(buff, " %s.rgb = min(%s.rgb, %s.rgb);\n", frgcolor, frgcolor, dstcolor); break;
                    case FPE_BLENDEQ_MAX: sprintf(buff, " %s.rgb = max(%s.rgb, %s.rgb);\n", frgcolor, frgcolor, dstcolor); break;
                    default: buff[0] = '\0'; break;
                }
                if (buff[0]) ShadAppend(buff);
                switch (state->blendeqalpha) {
                    case FPE_BLENDEQ_FUNC_ADD:             sprintf(buff, " %s.a = srcblend.a*%s.a + dstblend.a*%s.a;\n", frgcolor, frgcolor, dstcolor); break;
                    case FPE_BLENDEQ_FUNC_SUBTRACT:        sprintf(buff, " %s.a = srcblend.a*%s.a - dstblend.a*%s.a;\n", frgcolor, frgcolor, dstcolor); break;
                    case FPE_BLENDEQ_FUNC_REVERSE_SUBTRACT:sprintf(buff, " %s.a = dstblend.a*%s.a - srcblend.a*%s.a;\n", frgcolor, dstcolor, frgcolor); break;
                    case FPE_BLENDEQ_MIN: sprintf(buff, " %s.a = min(%s.a, %s.a);\n", frgcolor, frgcolor, dstcolor); break;
                    case FPE_BLENDEQ_MAX: sprintf(buff, " %s.a = max(%s.a, %s.a);\n", frgcolor, frgcolor, dstcolor); break;
                    default: buff[0] = '\0'; break;
                }
                if (buff[0]) ShadAppend(buff);
            }
        }

        if ((alpha_test || planes || shaderblend) && is_fragcolor)
            ShadAppend(" gl_FragColor = _gl4es_FragColor;\n");

        ShadAppend("}\n");
    }

    return (const char* const*)&shad;
}

// =============================================================================
//  Reset internals (untuk shared library re-init)
// =============================================================================
#ifdef GL4ES_COMPILE_FOR_USE_IN_SHARED_LIB
void fpe_shader_reset_internals(void) {
    if (shad) { free(shad); shad = NULL; }
    shad_cap = 0;
    comments = 1;
    use_es3  = 0;
    vary_vs_out  = "varying";
    vary_fs_in   = "varying";
    frag_out_var = "gl_FragColor";
    texname   = (const char**)texname_legacy;
    texnoproj = (const char**)texnoproj_legacy;
}
#endif
