// =============================================================================
//  shaderconv.c — GLSL Desktop → GLSL ES Shader Converter (ES 3.2 Edition)
//
//  Perubahan utama dari versi original:
//
//  1. ES3 PATH (glsl_target >= 300) — BARU:
//     - Header: "#version 320 es" atau "#version 300 es"
//     - attribute → layout(location=N) in  (vertex)
//     - varying   → out  (vertex) / in  (fragment)
//     - gl_FragColor  → out vec4 _gl4es_FragColor (deklarasi eksplisit)
//     - gl_FragData[N]→ layout(location=N) out vec4 _gl4es_FragData_N
//     - gl_FragDepth  → gl_FragDepth (core di ES3, tidak perlu extension)
//     - texture2D()   → texture()
//     - texture2DProj()  → textureProj()
//     - textureCube()    → texture()
//     - texture2DLod()   → textureLod()
//     - texture2DProjLod()→ textureProjLod()
//     - textureCubeLod() → textureLod()
//     - texture2DGradARB()     → textureGrad()
//     - texture2DProjGradARB() → textureProjGrad()
//     - textureCubeGradARB()   → textureGrad()
//     - dFdx/dFdy/fwidth: core di ES3, tidak perlu extension
//     - gl_InstanceID: core di ES3, tidak perlu define
//
//  2. Legacy path (glsl_target < 300) dipertahankan IDENTIK dengan original
//     untuk backward compatibility.
//
//  3. versionHeader sekarang BENAR-BENAR digunakan (di original selalu 0
//     karena dibungkus #if 0 — sudah dihapus).
//
//  4. Seluruh builtin_attrib, builtin_matrix, dan helper functions
//     (isBuiltinAttrib, isBuiltinMatrix, hasBuiltinAttrib, dll.)
//     dipertahankan IDENTIK agar tidak break subsystem lain (array.c, dll.)
// =============================================================================

#include "shaderconv.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../glx/hardext.h"
#include "debug.h"
#include "fpe_shader.h"
#include "init.h"
#include "preproc.h"
#include "string_utils.h"
#include "shader_hacks.h"
#include "logs.h"

// =============================================================================
//  Builtin attribute & matrix tables — TIDAK DIUBAH
// =============================================================================

typedef struct {
    const char* glname;
    const char* name;
    const char* type;
    const char* prec;
    int attrib;
} builtin_attrib_t;

const builtin_attrib_t builtin_attrib[] = {
    {"gl_Vertex",          "_gl4es_Vertex",          "vec4",  "highp",  ARB_VERTEX},
    {"gl_Color",           "_gl4es_Color",           "vec4",  "lowp",   ARB_COLOR},
    {"gl_MultiTexCoord0",  "_gl4es_MultiTexCoord0",  "vec4",  "highp",  ARB_MULTITEXCOORD0},
    {"gl_MultiTexCoord1",  "_gl4es_MultiTexCoord1",  "vec4",  "highp",  ARB_MULTITEXCOORD1},
    {"gl_MultiTexCoord2",  "_gl4es_MultiTexCoord2",  "vec4",  "highp",  ARB_MULTITEXCOORD2},
    {"gl_MultiTexCoord3",  "_gl4es_MultiTexCoord3",  "vec4",  "highp",  ARB_MULTITEXCOORD3},
    {"gl_MultiTexCoord4",  "_gl4es_MultiTexCoord4",  "vec4",  "highp",  ARB_MULTITEXCOORD4},
    {"gl_MultiTexCoord5",  "_gl4es_MultiTexCoord5",  "vec4",  "highp",  ARB_MULTITEXCOORD5},
    {"gl_MultiTexCoord6",  "_gl4es_MultiTexCoord6",  "vec4",  "highp",  ARB_MULTITEXCOORD6},
    {"gl_MultiTexCoord7",  "_gl4es_MultiTexCoord7",  "vec4",  "highp",  ARB_MULTITEXCOORD7},
    {"gl_MultiTexCoord8",  "_gl4es_MultiTexCoord8",  "vec4",  "highp",  ARB_MULTITEXCOORD8},
    {"gl_MultiTexCoord9",  "_gl4es_MultiTexCoord9",  "vec4",  "highp",  ARB_MULTITEXCOORD9},
    {"gl_MultiTexCoord10", "_gl4es_MultiTexCoord10", "vec4",  "highp",  ARB_MULTITEXCOORD10},
    {"gl_MultiTexCoord11", "_gl4es_MultiTexCoord11", "vec4",  "highp",  ARB_MULTITEXCOORD11},
    {"gl_MultiTexCoord12", "_gl4es_MultiTexCoord12", "vec4",  "highp",  ARB_MULTITEXCOORD12},
    {"gl_MultiTexCoord13", "_gl4es_MultiTexCoord13", "vec4",  "highp",  ARB_MULTITEXCOORD13},
    {"gl_MultiTexCoord14", "_gl4es_MultiTexCoord14", "vec4",  "highp",  ARB_MULTITEXCOORD14},
    {"gl_MultiTexCoord15", "_gl4es_MultiTexCoord15", "vec4",  "highp",  ARB_MULTITEXCOORD15},
    {"gl_SecondaryColor",  "_gl4es_SecondaryColor",  "vec4",  "lowp",   ARB_SECONDARY},
    {"gl_Normal",          "_gl4es_Normal",           "vec3",  "highp",  ARB_NORMAL},
    {"gl_FogCoord",        "_gl4es_FogCoord",         "float", "highp",  ARB_FOGCOORD}
};

const builtin_attrib_t builtin_attrib_compressed[] = {
    {"gl_Vertex",          "_gl4es_Vertex",          "vec4",  "highp",  COMP_VERTEX},
    {"gl_Color",           "_gl4es_Color",           "vec4",  "lowp",   COMP_COLOR},
    {"gl_MultiTexCoord0",  "_gl4es_MultiTexCoord0",  "vec4",  "highp",  COMP_MULTITEXCOORD0},
    {"gl_MultiTexCoord1",  "_gl4es_MultiTexCoord1",  "vec4",  "highp",  COMP_MULTITEXCOORD1},
    {"gl_MultiTexCoord2",  "_gl4es_MultiTexCoord2",  "vec4",  "highp",  COMP_MULTITEXCOORD2},
    {"gl_MultiTexCoord3",  "_gl4es_MultiTexCoord3",  "vec4",  "highp",  COMP_MULTITEXCOORD3},
    {"gl_MultiTexCoord4",  "_gl4es_MultiTexCoord4",  "vec4",  "highp",  COMP_MULTITEXCOORD4},
    {"gl_MultiTexCoord5",  "_gl4es_MultiTexCoord5",  "vec4",  "highp",  COMP_MULTITEXCOORD5},
    {"gl_MultiTexCoord6",  "_gl4es_MultiTexCoord6",  "vec4",  "highp",  COMP_MULTITEXCOORD6},
    {"gl_MultiTexCoord7",  "_gl4es_MultiTexCoord7",  "vec4",  "highp",  COMP_MULTITEXCOORD7},
    {"gl_MultiTexCoord8",  "_gl4es_MultiTexCoord8",  "vec4",  "highp",  COMP_MULTITEXCOORD8},
    {"gl_MultiTexCoord9",  "_gl4es_MultiTexCoord9",  "vec4",  "highp",  COMP_MULTITEXCOORD9},
    {"gl_MultiTexCoord10", "_gl4es_MultiTexCoord10", "vec4",  "highp",  COMP_MULTITEXCOORD10},
    {"gl_MultiTexCoord11", "_gl4es_MultiTexCoord11", "vec4",  "highp",  COMP_MULTITEXCOORD11},
    {"gl_MultiTexCoord12", "_gl4es_MultiTexCoord12", "vec4",  "highp",  COMP_MULTITEXCOORD12},
    {"gl_MultiTexCoord13", "_gl4es_MultiTexCoord13", "vec4",  "highp",  COMP_MULTITEXCOORD13},
    {"gl_MultiTexCoord14", "_gl4es_MultiTexCoord14", "vec4",  "highp",  COMP_MULTITEXCOORD14},
    {"gl_MultiTexCoord15", "_gl4es_MultiTexCoord15", "vec4",  "highp",  COMP_MULTITEXCOORD15},
    {"gl_SecondaryColor",  "_gl4es_SecondaryColor",  "vec4",  "lowp",   COMP_SECONDARY},
    {"gl_Normal",          "_gl4es_Normal",           "vec3",  "highp",  COMP_NORMAL},
    {"gl_FogCoord",        "_gl4es_FogCoord",         "float", "highp",  COMP_FOGCOORD}
};

// Attribute location mapping (mengikuti konvensi OpenGL/gl4es)
// Digunakan saat emit "layout(location=N) in" untuk ES3 vertex path
static int attrib_location(int attrib_id) {
    switch (attrib_id) {
        case ARB_VERTEX:         return 0;
        case ARB_NORMAL:         return 2;
        case ARB_COLOR:          return 3;
        case ARB_SECONDARY:      return 4;
        case ARB_FOGCOORD:       return 5;
        case ARB_MULTITEXCOORD0: return 8;
        case ARB_MULTITEXCOORD1: return 9;
        case ARB_MULTITEXCOORD2: return 10;
        case ARB_MULTITEXCOORD3: return 11;
        case ARB_MULTITEXCOORD4: return 12;
        case ARB_MULTITEXCOORD5: return 13;
        case ARB_MULTITEXCOORD6: return 14;
        case ARB_MULTITEXCOORD7: return 15;
        default: return -1; // generic attrib — tidak ada fixed location
    }
}

typedef struct {
    const char* glname;
    const char* name;
    const char* type;
    int   texarray;
    reserved_matrix_t matrix;
} builtin_matrix_t;

const builtin_matrix_t builtin_matrix[] = {
    {"gl_ModelViewMatrixInverseTranspose",          "_gl4es_ITModelViewMatrix",             "mat4", 0, MAT_MV_IT},
    {"gl_ModelViewMatrixInverse",                   "_gl4es_IModelViewMatrix",              "mat4", 0, MAT_MV_I},
    {"gl_ModelViewMatrixTranspose",                 "_gl4es_TModelViewMatrix",              "mat4", 0, MAT_MV_T},
    {"gl_ModelViewMatrix",                          "_gl4es_ModelViewMatrix",               "mat4", 0, MAT_MV},
    {"gl_ProjectionMatrixInverseTranspose",         "_gl4es_ITProjectionMatrix",            "mat4", 0, MAT_P_IT},
    {"gl_ProjectionMatrixInverse",                  "_gl4es_IProjectionMatrix",             "mat4", 0, MAT_P_I},
    {"gl_ProjectionMatrixTranspose",                "_gl4es_TProjectionMatrix",             "mat4", 0, MAT_P_T},
    {"gl_ProjectionMatrix",                         "_gl4es_ProjectionMatrix",              "mat4", 0, MAT_P},
    {"gl_ModelViewProjectionMatrixInverseTranspose","_gl4es_ITModelViewProjectionMatrix",   "mat4", 0, MAT_MVP_IT},
    {"gl_ModelViewProjectionMatrixInverse",         "_gl4es_IModelViewProjectionMatrix",    "mat4", 0, MAT_MVP_I},
    {"gl_ModelViewProjectionMatrixTranspose",       "_gl4es_TModelViewProjectionMatrix",    "mat4", 0, MAT_MVP_T},
    {"gl_ModelViewProjectionMatrix",                "_gl4es_ModelViewProjectionMatrix",     "mat4", 0, MAT_MVP},
    // Non-standard per-index texture matrices (avoid array uniform issues)
    {"gl_TextureMatrix_0",  "_gl4es_TextureMatrix_0",  "mat4", 0, MAT_T0},
    {"gl_TextureMatrix_1",  "_gl4es_TextureMatrix_1",  "mat4", 0, MAT_T1},
    {"gl_TextureMatrix_2",  "_gl4es_TextureMatrix_2",  "mat4", 0, MAT_T2},
    {"gl_TextureMatrix_3",  "_gl4es_TextureMatrix_3",  "mat4", 0, MAT_T3},
    {"gl_TextureMatrix_4",  "_gl4es_TextureMatrix_4",  "mat4", 0, MAT_T4},
    {"gl_TextureMatrix_5",  "_gl4es_TextureMatrix_5",  "mat4", 0, MAT_T5},
    {"gl_TextureMatrix_6",  "_gl4es_TextureMatrix_6",  "mat4", 0, MAT_T6},
    {"gl_TextureMatrix_7",  "_gl4es_TextureMatrix_7",  "mat4", 0, MAT_T7},
    {"gl_TextureMatrix_8",  "_gl4es_TextureMatrix_8",  "mat4", 0, MAT_T8},
    {"gl_TextureMatrix_9",  "_gl4es_TextureMatrix_9",  "mat4", 0, MAT_T9},
    {"gl_TextureMatrix_10", "_gl4es_TextureMatrix_10", "mat4", 0, MAT_T10},
    {"gl_TextureMatrix_11", "_gl4es_TextureMatrix_11", "mat4", 0, MAT_T11},
    {"gl_TextureMatrix_12", "_gl4es_TextureMatrix_12", "mat4", 0, MAT_T12},
    {"gl_TextureMatrix_13", "_gl4es_TextureMatrix_13", "mat4", 0, MAT_T13},
    {"gl_TextureMatrix_14", "_gl4es_TextureMatrix_14", "mat4", 0, MAT_T14},
    {"gl_TextureMatrix_15", "_gl4es_TextureMatrix_15", "mat4", 0, MAT_T15},
    // Standard texture matrix arrays
    {"gl_TextureMatrixInverseTranspose", "_gl4es_ITTextureMatrix", "mat4", 1, MAT_T0_IT},
    {"gl_TextureMatrixInverse",          "_gl4es_ITextureMatrix",  "mat4", 1, MAT_T0_I},
    {"gl_TextureMatrixTranspose",        "_gl4es_TTextureMatrix",  "mat4", 1, MAT_T0_T},
    {"gl_TextureMatrix",                 "_gl4es_TextureMatrix",   "mat4", 1, MAT_T0},
    {"gl_NormalMatrix",                  "_gl4es_NormalMatrix",    "mat3", 0, MAT_N}
};

// =============================================================================
//  Konstanta header & source strings
// =============================================================================

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)
static const char* gl4es_MaxLightsSource       = "#define _gl4es_MaxLights "      STR(MAX_LIGHT)      "\n";
static const char* gl4es_MaxClipPlanesSource   = "#define _gl4es_MaxClipPlanes "  STR(MAX_CLIP_PLANES) "\n";
static const char* gl4es_MaxTextureUnitsSource = "#define _gl4es_MaxTextureUnits " STR(MAX_TEX)        "\n";
static const char* gl4es_MaxTextureCoordsSource= "#define _gl4es_MaxTextureCoords " STR(MAX_TEX)       "\n";
#undef STR
#undef STR_HELPER

// ── GLESHeader ────────────────────────────────────────────────────────────────
// [0] Legacy ES2  : #version 100
// [1] Desktop 120 : #version 120
// [2] ES3 310     : #version 310 es (dengan #define compat untuk legacy path)
// [3] ES3 300     : #version 300 es (dengan #define compat)
// [4] ES3 320     : #version 320 es (path baru — BERSIH, tanpa #define compat)
// [5] ES3 300 bersih: #version 300 es (path baru — BERSIH)
//
// Index 4 & 5 digunakan ketika kita melakukan konversi PENUH (attribute→in, dll.)
// Index 2 & 3 digunakan untuk backward compat ketika konversi tidak dilakukan
static const char* GLESHeader[] = {
    // [0] Legacy: #version 100
    "#version 100\n%sprecision %s float;\nprecision %s int;\n",
    // [1] Desktop: #version 120
    "#version 120\n%sprecision %s float;\nprecision %s int;\n",
    // [2] ES3 310 (dengan macro compat — fallback jika konversi tidak penuh)
    "#version 310 es\n#define attribute in\n#define varying out\n%sprecision %s float;\nprecision %s int;\n",
    // [3] ES3 300 (dengan macro compat — fallback)
    "#version 300 es\n#define attribute in\n#define varying out\n%sprecision %s float;\nprecision %s int;\n",
    // [4] ES3 320 bersih (digunakan dengan konversi penuh attribute→in, varying→out/in)
    "#version 320 es\n%sprecision highp float;\nprecision highp int;\n",
    // [5] ES3 300 bersih
    "#version 300 es\n%sprecision highp float;\nprecision highp int;\n",
};

// ── Lighting & Material structs ───────────────────────────────────────────────
static const char* gl4es_LightSourceParametersSource =
    "struct gl_LightSourceParameters\n"
    "{\n"
    "   vec4 ambient;\n"
    "   vec4 diffuse;\n"
    "   vec4 specular;\n"
    "   vec4 position;\n"
    "   vec4 halfVector;\n"
    "   vec3 spotDirection;\n"
    "   float spotExponent;\n"
    "   float spotCutoff;\n"
    "   float spotCosCutoff;\n"
    "   float constantAttenuation;\n"
    "   float linearAttenuation;\n"
    "   float quadraticAttenuation;\n"
    "};\n"
    "uniform gl_LightSourceParameters gl_LightSource[gl_MaxLights];\n";

static const char* gl4es_LightModelParametersSource =
    "struct gl_LightModelParameters {\n"
    "  vec4 ambient;\n"
    "};\n"
    "uniform gl_LightModelParameters gl_LightModel;\n";

static const char* gl4es_MaterialParametersSource =
    "struct gl_MaterialParameters\n"
    "{\n"
    "   vec4 emission;\n"
    "   vec4 ambient;\n"
    "   vec4 diffuse;\n"
    "   vec4 specular;\n"
    "   float shininess;\n"
    "};\n"
    "uniform gl_MaterialParameters gl_FrontMaterial;\n"
    "uniform gl_MaterialParameters gl_BackMaterial;\n";

static const char* gl4es_LightModelProductsSource =
    "struct gl_LightModelProducts\n"
    "{\n"
    "   vec4 sceneColor;\n"
    "};\n"
    "uniform gl_LightModelProducts gl_FrontLightModelProduct;\n"
    "uniform gl_LightModelProducts gl_BackLightModelProduct;\n";

static const char* gl4es_LightProductsSource =
    "struct gl_LightProducts\n"
    "{\n"
    "   vec4 ambient;\n"
    "   vec4 diffuse;\n"
    "   vec4 specular;\n"
    "};\n"
    "uniform gl_LightProducts gl_FrontLightProduct[gl_MaxLights];\n"
    "uniform gl_LightProducts gl_BackLightProduct[gl_MaxLights];\n";

static const char* gl4es_PointSpriteSource =
    "struct gl_PointParameters\n"
    "{\n"
    "   float size;\n"
    "   float sizeMin;\n"
    "   float sizeMax;\n"
    "   float fadeThresholdSize;\n"
    "   float distanceConstantAttenuation;\n"
    "   float distanceLinearAttenuation;\n"
    "   float distanceQuadraticAttenuation;\n"
    "};\n"
    "uniform gl_PointParameters gl_Point;\n";

static const char* gl4es_FogParametersSource =
    "struct gl_FogParameters {\n"
    "    lowp vec4 color;\n"
    "    mediump float density;\n"
    "    mediump float start;\n"
    "    mediump float end;\n"
    "    mediump float scale;\n"
    "};\n"
    "uniform gl_FogParameters gl_Fog;\n";

static const char* gl4es_FogParametersSourceHighp =
    "struct gl_FogParameters {\n"
    "    lowp vec4 color;\n"
    "    mediump float density;\n"
    "    highp   float start;\n"
    "    highp   float end;\n"
    "    highp   float scale;\n"
    "};\n"
    "uniform gl_FogParameters gl_Fog;\n";

static const char* gl4es_texenvcolorSource =
    "uniform vec4 gl_TextureEnvColor[gl_MaxTextureUnits];\n";

static const char* gl4es_texgeneyeSource[4] = {
    "uniform vec4 gl_EyePlaneS[gl_MaxTextureCoords];\n",
    "uniform vec4 gl_EyePlaneT[gl_MaxTextureCoords];\n",
    "uniform vec4 gl_EyePlaneR[gl_MaxTextureCoords];\n",
    "uniform vec4 gl_EyePlaneQ[gl_MaxTextureCoords];\n"
};

static const char* gl4es_texgenobjSource[4] = {
    "uniform vec4 gl_ObjectPlaneS[gl_MaxTextureCoords];\n",
    "uniform vec4 gl_ObjectPlaneT[gl_MaxTextureCoords];\n",
    "uniform vec4 gl_ObjectPlaneR[gl_MaxTextureCoords];\n",
    "uniform vec4 gl_ObjectPlaneQ[gl_MaxTextureCoords];\n"
};

static const char* gl4es_clipplanesSource =
    "uniform vec4  gl_ClipPlane[gl_MaxClipPlanes];\n";

static const char* gl4es_normalscaleSource =
    "uniform float gl_NormalScale;\n";

// gl_InstanceID — ES2 perlu #define, ES3 ini core
static const char* gl4es_instanceID_es2 =
    "#define GL_ARB_draw_instanced 1\n"
    "uniform int _gl4es_InstanceID;\n";
static const char* gl4es_instanceID_es3 =
    "#define GL_ARB_draw_instanced 1\n"
    "#define _gl4es_InstanceID gl_InstanceID\n";

// ── Varying sources — ES2 pakai "varying", ES3 pakai "in"/"out" ──────────────
// Untuk ES3: isVertex → "out", !isVertex → "in"
// Format: sprintf(buf, gl4es_frontColorSource, qualifier)
static const char* gl4es_frontColorSource_fmt         = "%s lowp vec4 _gl4es_FrontColor;\n";
static const char* gl4es_backColorSource_fmt          = "%s lowp vec4 _gl4es_BackColor;\n";
static const char* gl4es_frontSecondaryColorSource_fmt= "%s lowp vec4 _gl4es_FrontSecondaryColor;\n";
static const char* gl4es_backSecondaryColorSource_fmt = "%s lowp vec4 _gl4es_BackSecondaryColor;\n";
static const char* gl4es_texcoordSource_fmt           = "%s mediump vec4 _gl4es_TexCoord[%d];\n";
static const char* gl4es_texcoordSourceAlt_fmt        = "%s mediump vec4 _gl4es_TexCoord_%d;\n";
static const char* gl4es_fogcoordSource_fmt           = "%s mediump float _gl4es_FogFragCoord;\n";

// Legacy strings (untuk ES2 path — backward compat)
static const char* gl4es_frontColorSource          = "varying lowp vec4 _gl4es_FrontColor;\n";
static const char* gl4es_backColorSource           = "varying lowp vec4 _gl4es_BackColor;\n";
static const char* gl4es_frontSecondaryColorSource = "varying lowp vec4 _gl4es_FrontSecondaryColor;\n";
static const char* gl4es_backSecondaryColorSource  = "varying lowp vec4 _gl4es_BackSecondaryColor;\n";
static const char* gl4es_texcoordSource            = "varying mediump vec4 _gl4es_TexCoord[%d];\n";
static const char* gl4es_texcoordSourceAlt         = "varying mediump vec4 _gl4es_TexCoord_%d;\n";
static const char* gl4es_fogcoordSource            = "varying mediump float _gl4es_FogFragCoord;\n";

// ── Fragment output — ES3 ─────────────────────────────────────────────────────
// gl_FragColor → deklarasi out + replacement
static const char* gl4es_fragColorDecl =
    "out lowp vec4 _gl4es_FragColor;\n";
// gl_FragData[N] → layout(location=N) out vec4 _gl4es_FragData_N;
// (dihasilkan dinamis via sprintf)

// ── Misc sources ──────────────────────────────────────────────────────────────
static const char* gl4es_ftransformSource =
    "\n"
    "highp vec4 ftransform() {\n"
    " return gl_ModelViewProjectionMatrix * gl_Vertex;\n"
    "}\n";

static const char* gl4es_ClipVertex       = "vec4 gl4es_ClipVertex;\n";
static const char* gl4es_ClipVertexSource = "gl4es_ClipVertex";
static const char* gl4es_ClipVertex_clip  =
    "\nif(any(lessThanEqual(gl4es_ClipVertex.xyz, vec3(-gl4es_ClipVertex.w)))"
    " || any(greaterThanEqual(gl4es_ClipVertex.xyz, vec3(gl4es_ClipVertex.w)))) discard;\n";

static const char* gl_TexCoordSource = "gl_TexCoord[";

static const char* gl_TexMatrixSources[] = {
    "gl_TextureMatrixInverseTranspose[",
    "gl_TextureMatrixInverse[",
    "gl_TextureMatrixTranspose[",
    "gl_TextureMatrix["
};

static const char* useEXTDrawBuffers = "#extension GL_EXT_draw_buffers : enable\n";

// ── Sampler & VertexAttrib names ──────────────────────────────────────────────
static const char* gl_ProgramEnv   = "gl_ProgramEnv";
static const char* gl_ProgramLocal = "gl_ProgramLocal";
static const char* gl_Samplers1D   = "gl_Sampler1D_";
static const char* gl_Samplers2D   = "gl_Sampler2D_";
static const char* gl_Samplers3D   = "gl_Sampler3D_";
static const char* gl_SamplersCube = "gl_SamplerCube_";
static const char* gl4es_Samplers1D   = "_gl4es_Sampler1D_";
static const char* gl4es_Samplers2D   = "_gl4es_Sampler2D_";
static const char* gl4es_Samplers3D   = "_gl4es_Sampler3D_";
static const char* gl4es_SamplersCube = "_gl4es_SamplerCube_";
static const char* gl4es_Samplers1D_uniform   = "uniform sampler2D _gl4es_Sampler1D_";
static const char* gl4es_Samplers2D_uniform   = "uniform sampler2D _gl4es_Sampler2D_";
static const char* gl4es_Samplers3D_uniform   = "uniform sampler2D _gl4es_Sampler3D_";
static const char* gl4es_SamplersCube_uniform = "uniform samplerCube _gl4es_SamplerCube_";

static const char* gl_VertexAttrib   = "gl_VertexAttrib_";
static const char* gl4es_VertexAttrib= "_gl4es_VertexAttrib_";

char gl_VA[MAX_VATTRIB][32]    = {0};
char gl4es_VA[MAX_VATTRIB][32] = {0};

// =============================================================================
//  Int/Float overload hacks — dipertahankan IDENTIK (masih perlu di ES3)
// =============================================================================

static const char* HackAltPow =
    "float pow(float f, int a) {\n return pow(f, float(a));\n}\n";

static const char* HackAltMax =
    "float max(float a, int b) {\n return max(a, float(b));\n}\n"
    "float max(int a, float b) {\n return max(float(a), b);\n}\n";

static const char* HackAltMin =
    "float min(float a, int b) {\n return min(a, float(b));\n}\n"
    "float min(int a, float b) {\n return min(float(a), b);\n}\n";

static const char* HackAltClamp =
    "float clamp(float f, int a, int b) {\n return clamp(f, float(a), float(b));\n}\n"
    "float clamp(float f, float a, int b) {\n return clamp(f, a, float(b));\n}\n"
    "float clamp(float f, int a, float b) {\n return clamp(f, float(a), b);\n}\n"
    "vec2 clamp(vec2 f, int a, int b) {\n return clamp(f, float(a), float(b));\n}\n"
    "vec2 clamp(vec2 f, float a, int b) {\n return clamp(f, a, float(b));\n}\n"
    "vec2 clamp(vec2 f, int a, float b) {\n return clamp(f, float(a), b);\n}\n"
    "vec3 clamp(vec3 f, int a, int b) {\n return clamp(f, float(a), float(b));\n}\n"
    "vec3 clamp(vec3 f, float a, int b) {\n return clamp(f, a, float(b));\n}\n"
    "vec3 clamp(vec3 f, int a, float b) {\n return clamp(f, float(a), b);\n}\n"
    "vec4 clamp(vec4 f, int a, int b) {\n return clamp(f, float(a), float(b));\n}\n"
    "vec4 clamp(vec4 f, float a, int b) {\n return clamp(f, a, float(b));\n}\n"
    "vec4 clamp(vec4 f, int a, float b) {\n return clamp(f, float(a), b);\n}\n";

static const char* HackAltMod =
    "float mod(float f, int a) {\n return mod(f, float(a));\n}\n"
    "vec2 mod(vec2 f, int a) {\n return mod(f, float(a));\n}\n"
    "vec3 mod(vec3 f, int a) {\n return mod(f, float(a));\n}\n"
    "vec4 mod(vec4 f, int a) {\n return mod(f, float(a));\n}\n";

// gl4es_transpose (dibutuhkan karena ES tidak punya built-in transpose di versi lama)
static const char* gl4es_transpose =
    "mat2 gl4es_transpose(mat2 m) {\n"
    " return mat2(m[0][0], m[1][0],\n"
    "             m[0][1], m[1][1]);\n"
    "}\n"
    "mat3 gl4es_transpose(mat3 m) {\n"
    " return mat3(m[0][0], m[1][0], m[2][0],\n"
    "             m[0][1], m[1][1], m[2][1],\n"
    "             m[0][2], m[1][2], m[2][2]);\n"
    "}\n"
    "mat4 gl4es_transpose(mat4 m) {\n"
    " return mat4(m[0][0], m[1][0], m[2][0], m[3][0],\n"
    "             m[0][1], m[1][1], m[2][1], m[3][1],\n"
    "             m[0][2], m[1][2], m[2][2], m[3][2],\n"
    "             m[0][3], m[1][3], m[2][3], m[3][3]);\n"
    "}\n";

// Texture LOD/Grad fallback helpers — hanya untuk ES2 legacy path
static const char* texture2DLodAlt =
    "vec4 _gl4es_texture2DLod(sampler2D sampler, vec2 coord, float lod) {\n"
    " return texture2D(sampler, coord);\n}\n";
static const char* texture2DProjLodAlt =
    "vec4 _gl4es_texture2DProjLod(sampler2D sampler, vec3 coord, float lod) {\n"
    " return texture2DProj(sampler, coord);\n}\n"
    "vec4 _gl4es_texture2DProjLod(sampler2D sampler, vec4 coord, float lod) {\n"
    " return texture2DProj(sampler, coord);\n}\n";
static const char* textureCubeLodAlt =
    "vec4 _gl4es_textureCubeLod(samplerCube sampler, vec3 coord, float lod) {\n"
    " return textureCube(sampler, coord);\n}\n";
static const char* texture2DGradAlt =
    "vec4 _gl4es_texture2DGrad(sampler2D sampler, vec2 coord, vec2 dPdx, vec2 dPdy) {\n"
    " return texture2D(sampler, coord);\n}\n";
static const char* texture2DProjGradAlt =
    "vec4 _gl4es_texture2DProjGrad(sampler2D sampler, vec3 coord, vec2 dPdx, vec2 dPdy) {\n"
    " return texture2DProj(sampler, coord);\n}\n"
    "vec4 _gl4es_texture2DProjGrad(sampler2D sampler, vec4 coord, vec2 dPdx, vec2 dPdy) {\n"
    " return texture2DProj(sampler, coord);\n}\n";
static const char* textureCubeGradAlt =
    "vec4 _gl4es_textureCubeGrad(samplerCube sampler, vec3 coord, vec2 dPdx, vec2 dPdy) {\n"
    " return textureCube(sampler, coord);\n}\n";

// =============================================================================
//  ConvertShader — Entry point utama
// =============================================================================
char* ConvertShader(const char* pEntry, int isVertex, shaderconv_need_t* need)
{
    #define ShadAppend(S) Tmp = gl4es_append(Tmp, &tmpsize, S)

    // Inisialisasi VA name cache
    if (gl_VA[0][0] == '\0') {
        for (int i = 0; i < MAX_VATTRIB; ++i) {
            sprintf(gl_VA[i],    "%s%d", gl_VertexAttrib,    i);
            sprintf(gl4es_VA[i], "%s%d", gl4es_VertexAttrib, i);
        }
    }

    // ── Deteksi FPE shader ────────────────────────────────────────────────
    int fpeShader = (strstr(pEntry, fpeshader_signature) != NULL) ? 1 : 0;

    // ── Debug log sebelum konversi ────────────────────────────────────────
    int maskbefore = 4 | (isVertex ? 1 : 2);
    int maskafter  = 8 | (isVertex ? 1 : 2);
    if ((globals4es.dbgshaderconv & maskbefore) == maskbefore)
        printf("Shader source%s:\n%s\n", fpeShader ? " (FPEShader)" : "", pEntry);

    int comments = globals4es.comments;

    // ── ES3 path selection ────────────────────────────────────────────────
    // Gunakan konversi penuh ES3 jika:
    // - glsl_target >= 300
    // - BUKAN FPE shader (FPE sudah dikonversi oleh fpe_shader.c)
    int es3 = (globals4es.glsl_target >= 300) && !fpeShader;

    // Qualifier untuk varying/in/out
    const char* vary_src = es3 ? (isVertex ? "out" : "in") : "varying";

    // ── Pre-process ───────────────────────────────────────────────────────
    char*  pBuffer     = (char*)pEntry;
    char*  versionString = NULL;

    if (!fpeShader) {
        extensions_t exts = {0};
        char* pHacked = ShaderHacks(pBuffer);
        pBuffer = preproc(pHacked, comments, globals4es.shadernogles, &exts, &versionString);
        if (pHacked != pEntry && pHacked != pBuffer) free(pHacked);
        // Komentar baris "precision" agar tidak konflik dengan precision header
        if (strstr(pBuffer, "\nprecision")) {
            int sz = strlen(pBuffer);
            pBuffer = gl4es_inplace_replace(pBuffer, &sz, "\nprecision", "\n//precision");
        }
        if (exts.ext) free(exts.ext);
    }

    // ── Need struct default ───────────────────────────────────────────────
    static shaderconv_need_t dummy_need = {0};
    if (!need) {
        need = &dummy_need;
        need->need_texcoord = -1;
        need->need_clean    = 1;
    }

    int notexarray = globals4es.notexarray || need->need_notexarray || fpeShader;

    // ── Version header selection ──────────────────────────────────────────
    // Pilih index ke GLESHeader[]:
    //   es3 + konversi penuh: 4 (320 es) atau 5 (300 es)
    //   es3 + compat defines: 2 (310 es) atau 3 (300 es)
    //   legacy ES2:           0 (#version 100)
    int versionHeader;
    int wanthighp = !fpeShader && hardext.highp;

    if (es3) {
        // Gunakan header bersih (index 4 atau 5) karena kita melakukan
        // konversi penuh attribute→in, varying→out/in
        versionHeader = (globals4es.glsl_target >= 320) ? 4 : 5;
    } else {
        // Legacy path: selalu #version 100
        versionHeader = 0;
        wanthighp = !fpeShader && hardext.highp;
    }

    char GLESFullHeader[512];
    sprintf(GLESFullHeader, GLESHeader[versionHeader],
            "", // placeholder untuk extension (sudah dipindah ke atas)
            (versionHeader >= 4) ? "highp" : (wanthighp ? "highp" : "mediump"),
            (versionHeader >= 4) ? "highp" : (wanthighp ? "highp" : "mediump"));

    // ── Alokasi buffer ────────────────────────────────────────────────────
    int tmpsize = strlen(pBuffer) * 2 + strlen(GLESFullHeader) + 512;
    char* Tmp = (char*)calloc(1, tmpsize);
    strcpy(Tmp, pBuffer);

    // ── Ganti/hapus baris #version dan insert header baru ─────────────────
    char* newptr = strstr(Tmp, "#version");
    if (!newptr) {
        Tmp = gl4es_inplace_insert(Tmp, GLESFullHeader, Tmp, &tmpsize);
    } else {
        while (*newptr != 0x0a) newptr++;
        newptr++;
        memmove(Tmp, newptr, strlen(newptr) + 1);
        Tmp = gl4es_inplace_insert(Tmp, GLESFullHeader, Tmp, &tmpsize);
    }
    int headline = 3;  // baris setelah header (version + precision float + precision int)
    if (versionHeader >= 4) headline = 3; // version 320 es + 2 precision lines

    // ── Pindahkan semua #extension ke zona header ─────────────────────────
    while (strstr(Tmp, "#extension") &&
           strstr(Tmp, "#extension") > gl4es_getline(Tmp, headline - 2)) {
        char* ext = strstr(Tmp, "#extension");
        size_t l = (uintptr_t)strstr(ext, "\n") - (uintptr_t)ext + sizeof("\n");
#ifndef _MSC_VER
        char e[l];
#else
        char* e = _alloca(l);
#endif
        memset(e, 0, l);
        strncpy(e, ext, l - 1);
        Tmp = gl4es_inplace_replace_simple(Tmp, &tmpsize, e, "");
        Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline - 2), e, Tmp, &tmpsize);
        ++headline;
    }

    // ── gl_FragDepth ──────────────────────────────────────────────────────
    int fragdepth = strstr(pBuffer, "gl_FragDepth") ? 1 : 0;
    if (fragdepth) {
        if (es3) {
            // ES3: gl_FragDepth adalah core — tidak perlu extension
            // (tidak ada yang perlu ditambahkan)
        } else if (hardext.fragdepth) {
            const char* GLESUseFragDepth = "#extension GL_EXT_frag_depth : enable\n";
            Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, 1), GLESUseFragDepth, Tmp, &tmpsize);
            headline++;
        } else {
            const char* GLESFakeFragDepth = "mediump float fakeFragDepth = 0.0;\n";
            Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline - 1), GLESFakeFragDepth, Tmp, &tmpsize);
            headline++;
        }
    }

    // ── Non-constant global initializers ──────────────────────────────────
    if (!es3) {
        // ES2 path: ekstensi ini membantu beberapa driver
        const char* ext_nonconstinit =
            "#extension GL_EXT_shader_non_constant_global_initializers : enable\n";
        Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, 1), ext_nonconstinit, Tmp, &tmpsize);
        ++headline;
    }

    // ── #define GL4ES marker ──────────────────────────────────────────────
    Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline - 1), "#define GL4ES\n", Tmp, &tmpsize);

    // ── Standard derivatives (dFdx, dFdy, fwidth) ────────────────────────
    int derivatives = (strstr(pBuffer, "dFdx(") || strstr(pBuffer, "dFdy(") ||
                       strstr(pBuffer, "fwidth(")) ? 1 : 0;
    if (derivatives) {
        if (es3) {
            // ES 3.0+ core — tidak perlu extension
        } else if (hardext.derivatives) {
            const char* ext = "#extension GL_OES_standard_derivatives : enable\n";
            Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, 1), ext, Tmp, &tmpsize);
            headline++;
        } else {
            // Fake derivatives untuk hardware yang tidak mendukung
            const char* fake =
                "float dFdx(float p) {return 0.0001;}  vec2 dFdx(vec2 p) {return vec2(0.0001);}  vec3 dFdx(vec3 p) {return vec3(0.0001);}\n"
                "float dFdy(float p) {return 0.0001;}  vec2 dFdy(vec2 p) {return vec2(0.0001);}  vec3 dFdy(vec3 p) {return vec3(0.0001);}\n"
                "float fwidth(float p) {return abs(dFdx(p))+abs(dFdy(p));}  vec2 fwidth(vec2 p) {return abs(dFdx(p))+abs(dFdy(p));}  vec3 fwidth(vec3 p) {return abs(dFdx(p))+abs(dFdy(p));}\n";
            Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline - 1), fake, Tmp, &tmpsize);
            headline++;
        }
    }

    // ── gl_FragData[] draw buffers ────────────────────────────────────────
    if (hardext.maxdrawbuffers > 1 && strstr(pBuffer, "gl_FragData[")) {
        if (es3) {
            // ES3: deklarasikan tiap gl_FragData[N] sebagai layout(location=N) out
            // Scan dulu berapa banyak yang digunakan
            int maxfragdata = 0;
            const char* p = pBuffer;
            while ((p = strstr(p, "gl_FragData["))) {
                p += strlen("gl_FragData[");
                if (*p >= '0' && *p <= '9') {
                    int n = (*p - '0');
                    if (p[1] >= '0' && p[1] <= '9') n = n * 10 + (p[1] - '0');
                    if (n > maxfragdata) maxfragdata = n;
                }
            }
            char decl[256] = "";
            char tmp2[100];
            for (int i = 0; i <= maxfragdata; i++) {
                sprintf(tmp2, "layout(location=%d) out lowp vec4 _gl4es_FragData_%d;\n", i, i);
                strcat(decl, tmp2);
            }
            Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), decl, Tmp, &tmpsize);
            headline += maxfragdata + 1;
            // Ganti gl_FragData[N] dengan _gl4es_FragData_N
            for (int i = maxfragdata; i >= 0; i--) {
                char from[32], to[32];
                sprintf(from, "gl_FragData[%d]", i);
                sprintf(to,   "_gl4es_FragData_%d", i);
                Tmp = gl4es_inplace_replace(Tmp, &tmpsize, from, to);
            }
        } else {
            Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, 1), useEXTDrawBuffers, Tmp, &tmpsize);
        }
    }

    // ── Int/float overload hacks ──────────────────────────────────────────
    // Perlu untuk kedua path (ES2 dan ES3) karena GLSL ES strict typing
    if (!fpeShader && !globals4es.nointovlhack) {
        if (strstr(Tmp, "pow(")   || strstr(Tmp, "pow ("))
            Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), HackAltPow,   Tmp, &tmpsize);
        if (strstr(Tmp, "max(")   || strstr(Tmp, "max ("))
            Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), HackAltMax,   Tmp, &tmpsize);
        if (strstr(Tmp, "min(")   || strstr(Tmp, "min ("))
            Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), HackAltMin,   Tmp, &tmpsize);
        if (strstr(Tmp, "clamp(") || strstr(Tmp, "clamp ("))
            Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), HackAltClamp, Tmp, &tmpsize);
        if (strstr(Tmp, "mod(")   || strstr(Tmp, "mod ("))
            Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), HackAltMod,   Tmp, &tmpsize);
    }

    // ── Texture LOD / Grad functions ──────────────────────────────────────
    if (!isVertex) {
        if (es3) {
            // ES3: semua fungsi ini sudah tersedia sebagai core dengan nama baru
            // texture2DLod       → textureLod
            // texture2DProjLod   → textureProjLod
            // textureCubeLod     → textureLod
            // texture2DGradARB   → textureGrad
            // texture2DProjGradARB → textureProjGrad
            // textureCubeGradARB → textureGrad
            // texture2DLodEXT    → textureLod
            // texture2DProjLodEXT→ textureProjLod
            // textureCubeLodEXT  → textureLod
            if (gl4es_find_string(Tmp, "texture2DLodEXT"))
                Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "texture2DLodEXT", "textureLod");
            if (gl4es_find_string(Tmp, "texture2DProjLodEXT"))
                Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "texture2DProjLodEXT", "textureProjLod");
            if (gl4es_find_string(Tmp, "textureCubeLodEXT"))
                Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "textureCubeLodEXT", "textureLod");
            if (gl4es_find_string(Tmp, "texture2DLod"))
                Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "texture2DLod", "textureLod");
            if (gl4es_find_string(Tmp, "texture2DProjLod"))
                Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "texture2DProjLod", "textureProjLod");
            if (gl4es_find_string(Tmp, "textureCubeLod"))
                Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "textureCubeLod", "textureLod");
            if (gl4es_find_string(Tmp, "texture2DGradARB"))
                Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "texture2DGradARB", "textureGrad");
            if (gl4es_find_string(Tmp, "texture2DProjGradARB"))
                Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "texture2DProjGradARB", "textureProjGrad");
            if (gl4es_find_string(Tmp, "textureCubeGradARB"))
                Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "textureCubeGradARB", "textureGrad");
            if (gl4es_find_string(Tmp, "texture2DGradEXT"))
                Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "texture2DGradEXT", "textureGrad");
            if (gl4es_find_string(Tmp, "texture2DProjGradEXT"))
                Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "texture2DProjGradEXT", "textureProjGrad");
            if (gl4es_find_string(Tmp, "textureCubeGradEXT"))
                Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "textureCubeGradEXT", "textureGrad");
        } else {
            // ES2 legacy path — sama dengan original
            if (hardext.shaderlod && (
                gl4es_find_string(Tmp, "texture2DLod") ||
                gl4es_find_string(Tmp, "texture2DProjLod") ||
                gl4es_find_string(Tmp, "textureCubeLod") ||
                gl4es_find_string(Tmp, "texture2DGradARB") ||
                gl4es_find_string(Tmp, "texture2DProjGradARB") ||
                gl4es_find_string(Tmp, "textureCubeGradARB"))) {
                const char* extlod = "#extension GL_EXT_shader_texture_lod : enable\n";
                Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, 1), extlod, Tmp, &tmpsize);
            }
            if (gl4es_find_string(Tmp, "texture2DLod")) {
                if (hardext.shaderlod)
                    Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "texture2DLod", "texture2DLodEXT");
                else {
                    Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "texture2DLod", "_gl4es_texture2DLod");
                    Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), texture2DLodAlt, Tmp, &tmpsize);
                }
            }
            if (gl4es_find_string(Tmp, "texture2DProjLod")) {
                if (hardext.shaderlod)
                    Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "texture2DProjLod", "texture2DProjLodEXT");
                else {
                    Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "texture2DProjLod", "_gl4es_texture2DProjLod");
                    Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), texture2DProjLodAlt, Tmp, &tmpsize);
                }
            }
            if (gl4es_find_string(Tmp, "textureCubeLod")) {
                if (hardext.shaderlod) {
                    if (!hardext.cubelod)
                        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "textureCubeLod", "textureCubeLodEXT");
                } else {
                    Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "textureCubeLod", "_gl4es_textureCubeLod");
                    Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), textureCubeLodAlt, Tmp, &tmpsize);
                }
            }
            if (gl4es_find_string(Tmp, "texture2DGradARB")) {
                if (hardext.shaderlod)
                    Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "texture2DGradARB", "texture2DGradEXT");
                else {
                    Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "texture2DGradARB", "_gl4es_texture2DGrad");
                    Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), texture2DGradAlt, Tmp, &tmpsize);
                }
            }
            if (gl4es_find_string(Tmp, "texture2DProjGradARB")) {
                if (hardext.shaderlod)
                    Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "texture2DProjGradARB", "texture2DProjGradEXT");
                else {
                    Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "texture2DProjGradARB", "_gl4es_texture2DProjGrad");
                    Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), texture2DProjGradAlt, Tmp, &tmpsize);
                }
            }
            if (gl4es_find_string(Tmp, "textureCubeGradARB")) {
                if (hardext.shaderlod) {
                    if (!hardext.cubelod)
                        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "textureCubeGradARB", "textureCubeGradEXT");
                } else {
                    Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "textureCubeGradARB", "_gl4es_textureCubeGrad");
                    Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), textureCubeGradAlt, Tmp, &tmpsize);
                }
            }
        }
    }

    // ── Hapus "\\r\n" dan "\\n" di preprocessor macros ────────────────────
    newptr = Tmp;
    while (*newptr != 0x00) {
        if (*newptr == '\\') {
            if (*(newptr+1) == '\r' && *(newptr+2) == '\n')
                memmove(newptr, newptr+3, strlen(newptr+3)+1);
            else if (*(newptr+1) == '\n')
                memmove(newptr, newptr+2, strlen(newptr+2)+1);
        }
        newptr++;
    }

    // ── Hapus trailing 'f' setelah float literals ─────────────────────────
    newptr = Tmp;
    int state = 0;
    while (*newptr != 0x00) {
        switch (state) {
            case 0:
                if (*newptr >= '0' && *newptr <= '9') state = 1;
                else if (*newptr == '.') state = 2;
                else if (*newptr == ' ' || *newptr == 0x0d || *newptr == 0x0a ||
                         *newptr == '-' || *newptr == '+' || *newptr == '*' ||
                         *newptr == '/' || *newptr == '(' || *newptr == ')' ||
                         *newptr == '>' || *newptr == '<') state = 0;
                else state = 3;
                break;
            case 1:
                if (*newptr >= '0' && *newptr <= '9') state = 1;
                else if (*newptr == '.') state = 2;
                else if (*newptr == ' ' || *newptr == 0x0d || *newptr == 0x0a ||
                         *newptr == '-' || *newptr == '+' || *newptr == '*' ||
                         *newptr == '/' || *newptr == '(' || *newptr == ')' ||
                         *newptr == '>' || *newptr == '<') state = 0;
                else if (*newptr == 'f') { memmove(newptr, newptr+1, strlen(newptr+1)+1); newptr--; }
                else state = 3;
                break;
            case 2:
                if (*newptr >= '0' && *newptr <= '9') state = 2;
                else if (*newptr == ' ' || *newptr == 0x0d || *newptr == 0x0a ||
                         *newptr == '-' || *newptr == '+' || *newptr == '*' ||
                         *newptr == '/' || *newptr == '(' || *newptr == ')' ||
                         *newptr == '>' || *newptr == '<') state = 0;
                else if (*newptr == 'f') { memmove(newptr, newptr+1, strlen(newptr+1)+1); newptr--; }
                else state = 3;
                break;
            case 3:
                if (*newptr == ' ' || *newptr == 0x0d || *newptr == 0x0a ||
                    *newptr == '-' || *newptr == '+' || *newptr == '*' ||
                    *newptr == '/' || *newptr == '(' || *newptr == ')' ||
                    *newptr == '>' || *newptr == '<') state = 0;
                else state = 3;
                break;
        }
        newptr++;
    }

    // ── gl_FragDepth replacement ──────────────────────────────────────────
    if (fragdepth) {
        if (es3) {
            // ES3: gl_FragDepth core, biarkan
        } else {
            Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_FragDepth",
                hardext.fragdepth ? "gl_FragDepthEXT" : "fakeFragDepth");
        }
    }

    // ==========================================================================
    //  Vertex shader: builtin attributes
    // ==========================================================================
    if (isVertex) {
        // ftransform()
        if (strstr(Tmp, "ftransform(")) {
            Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline),
                                        gl4es_ftransformSource, Tmp, &tmpsize);
        }

        // Builtin OpenGL attributes (gl_Vertex, gl_Color, gl_MultiTexCoordN, dll.)
        int n = sizeof(builtin_attrib) / sizeof(builtin_attrib_t);
        for (int i = 0; i < n; i++) {
            if (strstr(Tmp, builtin_attrib[i].glname)) {
                // Ganti gl_Name dengan _gl4es_Name
                Tmp = gl4es_inplace_replace(Tmp, &tmpsize,
                                             builtin_attrib[i].glname,
                                             builtin_attrib[i].name);
                // Deklarasikan attribute
                char def[128];
                if (es3) {
                    // ES3: layout(location=N) in prec type name;
                    int loc = attrib_location(builtin_attrib[i].attrib);
                    if (loc >= 0)
                        sprintf(def, "layout(location=%d) in %s %s %s;\n",
                                loc, builtin_attrib[i].prec,
                                builtin_attrib[i].type, builtin_attrib[i].name);
                    else
                        sprintf(def, "in %s %s %s;\n",
                                builtin_attrib[i].prec,
                                builtin_attrib[i].type, builtin_attrib[i].name);
                } else {
                    // ES2: attribute prec type name;
                    sprintf(def, "attribute %s %s %s;\n",
                            builtin_attrib[i].prec,
                            builtin_attrib[i].type, builtin_attrib[i].name);
                }
                Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline++), def, Tmp, &tmpsize);
            }
        }

        // Generic vertex attribs (gl_VertexAttrib_N)
        if (strstr(Tmp, gl_VertexAttrib)) {
            for (int i = 0; i < MAX_VATTRIB; ++i) {
                if (gl4es_find_string(Tmp, gl_VA[i])) {
                    char A[100];
                    if (es3)
                        sprintf(A, "in highp vec4 %s%d;\n", gl4es_VertexAttrib, i);
                    else
                        sprintf(A, "attribute highp vec4 %s%d;\n", gl4es_VertexAttrib, i);
                    Tmp = gl4es_inplace_replace(Tmp, &tmpsize, gl_VA[i], gl4es_VA[i]);
                    Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline++), A, Tmp, &tmpsize);
                }
            }
        }
    }

    // ==========================================================================
    //  Builtin varyings: Color, SecondaryColor, FogFragCoord, TexCoord
    // ==========================================================================
    int nvarying = 0;
    char vbuf[256];

    // ── gl_Color / gl_FrontColor / gl_BackColor ───────────────────────────
    if (strstr(Tmp, "gl_Color") || need->need_color) {
        if (need->need_color < 1) need->need_color = 1;
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_Color",
            (need->need_color == 1)
                ? "gl_FrontColor"
                : "(gl_FrontFacing?gl_FrontColor:gl_BackColor)");
    }
    if (strstr(Tmp, "gl_FrontColor") || need->need_color) {
        if (need->need_color < 1) need->need_color = 1;
        nvarying++;
        sprintf(vbuf, gl4es_frontColorSource_fmt, vary_src);
        Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), vbuf, Tmp, &tmpsize);
        headline += gl4es_countline(vbuf);
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_FrontColor", "_gl4es_FrontColor");
    }
    if (strstr(Tmp, "gl_BackColor") || (need->need_color == 2)) {
        need->need_color = 2;
        nvarying++;
        sprintf(vbuf, gl4es_backColorSource_fmt, vary_src);
        Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), vbuf, Tmp, &tmpsize);
        headline += gl4es_countline(vbuf);
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_BackColor", "_gl4es_BackColor");
    }

    // ── gl_SecondaryColor / gl_FrontSecondaryColor / gl_BackSecondaryColor ─
    if (strstr(Tmp, "gl_SecondaryColor") || need->need_secondary) {
        if (need->need_secondary < 1) need->need_secondary = 1;
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_SecondaryColor",
            (need->need_secondary == 1)
                ? "gl_FrontSecondaryColor"
                : "(gl_FrontFacing?gl_FrontSecondaryColor:gl_BackSecondaryColor)");
    }
    if (strstr(Tmp, "gl_FrontSecondaryColor") || need->need_secondary) {
        if (need->need_secondary < 1) need->need_secondary = 1;
        nvarying++;
        sprintf(vbuf, gl4es_frontSecondaryColorSource_fmt, vary_src);
        Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), vbuf, Tmp, &tmpsize);
        headline += gl4es_countline(vbuf);
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_FrontSecondaryColor", "_gl4es_FrontSecondaryColor");
    }
    if (strstr(Tmp, "gl_BackSecondaryColor") || (need->need_secondary == 2)) {
        need->need_secondary = 2;
        nvarying++;
        sprintf(vbuf, gl4es_backSecondaryColorSource_fmt, vary_src);
        Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), vbuf, Tmp, &tmpsize);
        headline += gl4es_countline(vbuf);
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_BackSecondaryColor", "_gl4es_BackSecondaryColor");
    }

    // ── gl_FogFragCoord ───────────────────────────────────────────────────
    if (strstr(Tmp, "gl_FogFragCoord") || need->need_fogcoord) {
        need->need_fogcoord = 1;
        nvarying++;
        sprintf(vbuf, gl4es_fogcoordSource_fmt, vary_src);
        Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), vbuf, Tmp, &tmpsize);
        headline += gl4es_countline(vbuf);
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_FogFragCoord", "_gl4es_FogFragCoord");
    }

    // ── gl_TexCoord[N] ────────────────────────────────────────────────────
    if (strstr(Tmp, "gl_TexCoord") || need->need_texcoord != -1) {
        int ntex = need->need_texcoord;
        char* p = Tmp;
        int notexarray_ok = 1;
        while ((p = strstr(p, gl_TexCoordSource))) {
            p += strlen(gl_TexCoordSource);
            if (*p >= '0' && *p <= '9') {
                int n = (*p) - '0';
                if (p[1] >= '0' && p[1] <= '9') n = n * 10 + (p[1] - '0');
                if (ntex < n) ntex = n;
            } else {
                notexarray_ok = 0;
            }
        }
        if (ntex == -1) ntex = hardext.maxtex;

        // Prefer notexarray di ES3 untuk efisiensi
        if (!notexarray && ntex + nvarying > hardext.maxvarying && !need->need_clean && notexarray_ok) {
            notexarray = 1; need->need_notexarray = 1;
        }
        if (!isVertex && notexarray_ok && !need->need_clean) {
            notexarray = 1; need->need_notexarray = 1;
        }
        if (!notexarray && ntex + nvarying > hardext.maxvarying)
            ntex = hardext.maxvarying - nvarying;
        need->need_texcoord = ntex;

        char d[100];
        if (notexarray) {
            for (int k = 0; k <= ntex; k++) {
                char d2[100];
                sprintf(d2, "gl_TexCoord[%d]", k);
                if (strstr(Tmp, d2)) {
                    sprintf(d, gl4es_texcoordSourceAlt_fmt, vary_src, k);
                    Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), d, Tmp, &tmpsize);
                    headline += gl4es_countline(d);
                    sprintf(d, "_gl4es_TexCoord_%d", k);
                    Tmp = gl4es_inplace_replace(Tmp, &tmpsize, d2, d);
                }
                sprintf(d2, "_gl4es_TexCoord_%d", k);
                if (strstr(Tmp, d2)) need->need_texs |= (1 << k);
            }
        } else {
            sprintf(d, gl4es_texcoordSource_fmt, vary_src, ntex + 1);
            Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), d, Tmp, &tmpsize);
            headline += gl4es_countline(d);
            Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_TexCoord", "_gl4es_TexCoord");
            for (int k = 0; k <= ntex; k++) need->need_texs |= (1 << k);
        }
    }

    // ==========================================================================
    //  ES3: Konversi penuh texture functions dan attribute/varying qualifiers
    // ==========================================================================
    if (es3) {
        // ── Texture function replacement ──────────────────────────────────
        // Urutan penting: panjang → pendek untuk hindari partial match!
        // texture2DProj SEBELUM texture2D
        if (gl4es_find_string(Tmp, "texture2DProj"))
            Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "texture2DProj", "textureProj");
        if (gl4es_find_string(Tmp, "textureCube"))
            Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "textureCube", "texture");
        if (gl4es_find_string(Tmp, "texture2D"))
            Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "texture2D", "texture");
        if (gl4es_find_string(Tmp, "texture3D"))
            Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "texture3D", "texture");
        if (gl4es_find_string(Tmp, "texture1D"))
            Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "texture1D", "texture");

        // ── Fragment output: gl_FragColor → _gl4es_FragColor ──────────────
        if (!isVertex && strstr(Tmp, "gl_FragColor")) {
            // Deklarasikan "out vec4 _gl4es_FragColor;" di header
            Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline),
                                        gl4es_fragColorDecl, Tmp, &tmpsize);
            headline += gl4es_countline(gl4es_fragColorDecl);
            // Ganti semua gl_FragColor
            Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_FragColor", "_gl4es_FragColor");
        }

        // ── "attribute" → "in" (vertex), "varying" → "out"/"in" ──────────
        // Ganti keyword standalone saja (bukan bagian dari nama identifier)
        // Strategi: pakai gl4es_inplace_replace yang sudah tahu word boundary
        if (isVertex) {
            // "attribute " → "in "
            if (strstr(Tmp, "attribute "))
                Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "attribute ", "in ");
            if (strstr(Tmp, "attribute\t"))
                Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "attribute\t", "in\t");
            // "varying " → "out " (vertex output)
            if (strstr(Tmp, "varying "))
                Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "varying ", "out ");
            if (strstr(Tmp, "varying\t"))
                Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "varying\t", "out\t");
        } else {
            // "varying " → "in " (fragment input)
            if (strstr(Tmp, "varying "))
                Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "varying ", "in ");
            if (strstr(Tmp, "varying\t"))
                Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "varying\t", "in\t");
        }

        // ── gl_InstanceID ─────────────────────────────────────────────────
        // ES3: gl_InstanceID core — hanya perlu #define _gl4es_ alias
        if (strstr(Tmp, "gl_InstanceID") || strstr(Tmp, "gl_InstanceIDARB")) {
            Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline),
                                        gl4es_instanceID_es3, Tmp, &tmpsize);
            headline += gl4es_countline(gl4es_instanceID_es3);
            Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_InstanceIDARB", "_gl4es_InstanceID");
            Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_InstanceID",    "_gl4es_InstanceID");
        }
    } else {
        // ES2 legacy: gl_InstanceID via uniform
        if (strstr(Tmp, "gl_InstanceID") || strstr(Tmp, "gl_InstanceIDARB")) {
            Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline),
                                        gl4es_instanceID_es2, Tmp, &tmpsize);
            headline += gl4es_countline(gl4es_instanceID_es2);
            Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_InstanceIDARB", "_gl4es_InstanceID");
            Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_InstanceID",    "_gl4es_InstanceID");
        }
    }

    // ==========================================================================
    //  Builtin matrices
    // ==========================================================================
    {
        // transpose() → gl4es_transpose() (ES tidak punya built-in di versi lama)
        // ES3 punya built-in transpose, tapi kita biarkan untuk safety
        if (strstr(Tmp, "transpose(") || strstr(Tmp, "transpose ") || strstr(Tmp, "transpose\t")) {
            if (!es3) {
                // ES2: tidak ada built-in transpose
                Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), gl4es_transpose, Tmp, &tmpsize);
                gl4es_inplace_replace(Tmp, &tmpsize, "transpose", "gl4es_transpose");
            }
            // ES3: transpose() adalah core — tidak perlu penggantian
        }

        // Scan max texture matrix index
        int ntex = -1;
        for (int i = 0; i < 4; ++i) {
            char* p = Tmp;
            while ((p = strstr(p, gl_TexMatrixSources[i]))) {
                p += strlen(gl_TexMatrixSources[i]);
                if (*p >= '0' && *p <= '9') {
                    int n = 0;
                    while (*p >= '0' && *p <= '9') n = n * 10 + (*(p++) - '0');
                    if (ntex < n) ntex = n;
                }
            }
        }
        if (ntex == -1) ntex = need->need_texcoord; else ++ntex;

        // Ubah gl_TextureMatrix[X] → gl_TextureMatrix_X jika bisa
        int change_textmat = notexarray;
        if (!change_textmat) {
            change_textmat = 1;
            char* p = Tmp;
            while (change_textmat && (p = strstr(p, "gl_TextureMatrix["))) {
                p += strlen("gl_TextureMatrix[");
                while ((*p) >= '0' && (*p) <= '9') ++p;
                if ((*p) != ']') change_textmat = 0;
            }
        }
        if (change_textmat) {
            for (int k = 0; k <= ntex; k++) {
                char d[100], d2[100];
                sprintf(d2, "gl_TextureMatrix[%d]", k);
                if (strstr(Tmp, d2)) {
                    sprintf(d, "gl_TextureMatrix_%d", k);
                    Tmp = gl4es_inplace_replace(Tmp, &tmpsize, d2, d);
                }
            }
        }

        // Builtin matrix uniforms
        int n_mat = sizeof(builtin_matrix) / sizeof(builtin_matrix_t);
        for (int i = 0; i < n_mat; i++) {
            if (strstr(Tmp, builtin_matrix[i].glname)) {
                Tmp = gl4es_inplace_replace(Tmp, &tmpsize,
                                             builtin_matrix[i].glname,
                                             builtin_matrix[i].name);
                char def[128];
                int ishighp = (isVertex || hardext.highp) ? 1 : 0;
                if (builtin_matrix[i].matrix == MAT_N) {
                    if (need->need_normalmatrix && !hardext.highp) ishighp = 0;
                    if (!hardext.highp && !isVertex) need->need_normalmatrix = 1;
                }
                if (builtin_matrix[i].matrix == MAT_MV) {
                    if (need->need_mvmatrix && !hardext.highp) ishighp = 0;
                    if (!hardext.highp && !isVertex) need->need_mvmatrix = 1;
                }
                if (builtin_matrix[i].matrix == MAT_MVP) {
                    if (need->need_mvpmatrix && !hardext.highp) ishighp = 0;
                    if (!hardext.highp && !isVertex) need->need_mvpmatrix = 1;
                }
                if (builtin_matrix[i].texarray)
                    sprintf(def, "uniform %s%s %s[%d];\n",
                            ishighp ? "highp " : "mediump ",
                            builtin_matrix[i].type, builtin_matrix[i].name, ntex);
                else
                    sprintf(def, "uniform %s%s %s;\n",
                            ishighp ? "highp " : "mediump ",
                            builtin_matrix[i].type, builtin_matrix[i].name);
                Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline++), def, Tmp, &tmpsize);
            }
        }
    }

    // ── "centroid" keyword cleanup ────────────────────────────────────────
    {
        char* p = Tmp;
        while ((p = strstr(p, "centroid")) != NULL) {
            if (p[8] == ' ' || p[8] == '\t') {
                const char* p2 = gl4es_get_next_str(p + 8);
                if (strcmp(p2, "uniform") == 0 || strcmp(p2, "varying") == 0)
                    memset(p, ' ', 8);
            }
            p += 8;
        }
    }

    // ==========================================================================
    //  Lighting, Material, Fog, Point & misc builtin state
    //  (Dipertahankan IDENTIK dengan original — tidak ada yang berubah di sini
    //   karena struct uniforms tetap compatible antara ES2 dan ES3)
    // ==========================================================================

    if (strstr(Tmp, "gl_LightSourceParameters") || strstr(Tmp, "gl_LightSource")) {
        Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), gl4es_LightSourceParametersSource, Tmp, &tmpsize);
        headline += gl4es_countline(gl4es_LightSourceParametersSource);
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_LightSourceParameters", "_gl4es_LightSourceParameters");
    }
    if (strstr(Tmp, "gl_LightModelParameters") || strstr(Tmp, "gl_LightModel")) {
        Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), gl4es_LightModelParametersSource, Tmp, &tmpsize);
        headline += gl4es_countline(gl4es_LightModelParametersSource);
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_LightModelParameters", "_gl4es_LightModelParameters");
    }
    if (strstr(Tmp, "gl_LightModelProducts") || strstr(Tmp, "gl_FrontLightModelProduct") || strstr(Tmp, "gl_BackLightModelProduct")) {
        Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), gl4es_LightModelProductsSource, Tmp, &tmpsize);
        headline += gl4es_countline(gl4es_LightModelProductsSource);
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_LightModelProducts", "_gl4es_LightModelProducts");
    }
    if (strstr(Tmp, "gl_LightProducts") || strstr(Tmp, "gl_FrontLightProduct") || strstr(Tmp, "gl_BackLightProduct")) {
        Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), gl4es_LightProductsSource, Tmp, &tmpsize);
        headline += gl4es_countline(gl4es_LightProductsSource);
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_LightProducts", "_gl4es_LightProducts");
    }
    if (strstr(Tmp, "gl_MaterialParameters ") || strstr(Tmp, "gl_FrontMaterial") || strstr(Tmp, "gl_BackMaterial")) {
        Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), gl4es_MaterialParametersSource, Tmp, &tmpsize);
        headline += gl4es_countline(gl4es_MaterialParametersSource);
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_MaterialParameters", "_gl4es_MaterialParameters");
    }
    if (strstr(Tmp, "gl_LightSource"))
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_LightSource", "_gl4es_LightSource");
    if (strstr(Tmp, "gl_LightModel"))
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_LightModel", "_gl4es_LightModel");
    if (strstr(Tmp, "gl_FrontLightModelProduct"))
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_FrontLightModelProduct", "_gl4es_FrontLightModelProduct");
    if (strstr(Tmp, "gl_BackLightModelProduct"))
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_BackLightModelProduct", "_gl4es_BackLightModelProduct");
    if (strstr(Tmp, "gl_FrontLightProduct"))
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_FrontLightProduct", "_gl4es_FrontLightProduct");
    if (strstr(Tmp, "gl_BackLightProduct"))
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_BackLightProduct", "_gl4es_BackLightProduct");
    if (strstr(Tmp, "gl_FrontMaterial"))
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_FrontMaterial", "_gl4es_FrontMaterial");
    if (strstr(Tmp, "gl_BackMaterial"))
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_BackMaterial", "_gl4es_BackMaterial");
    if (strstr(Tmp, "gl_MaxLights")) {
        Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, 2), gl4es_MaxLightsSource, Tmp, &tmpsize);
        headline += gl4es_countline(gl4es_MaxLightsSource);
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_MaxLights", "_gl4es_MaxLights");
    }
    if (strstr(Tmp, "gl_NormalScale")) {
        Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), gl4es_normalscaleSource, Tmp, &tmpsize);
        headline += gl4es_countline(gl4es_normalscaleSource);
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_NormalScale", "_gl4es_NormalScale");
    }
    if (strstr(Tmp, "gl_ClipPlane")) {
        Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), gl4es_clipplanesSource, Tmp, &tmpsize);
        headline += gl4es_countline(gl4es_clipplanesSource);
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_ClipPlane", "_gl4es_ClipPlane");
    }
    if (strstr(Tmp, "gl_MaxClipPlanes")) {
        Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, 2), gl4es_MaxClipPlanesSource, Tmp, &tmpsize);
        headline += gl4es_countline(gl4es_MaxClipPlanesSource);
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_MaxClipPlanes", "_gl4es_MaxClipPlanes");
    }
    if (strstr(Tmp, "gl_PointParameters") || strstr(Tmp, "gl_Point")) {
        Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), gl4es_PointSpriteSource, Tmp, &tmpsize);
        headline += gl4es_countline(gl4es_PointSpriteSource);
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_PointParameters", "_gl4es_PointParameters");
    }
    if (strstr(Tmp, "gl_Point"))
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_Point", "_gl4es_Point");
    if (strstr(Tmp, "gl_FogParameters") || strstr(Tmp, "gl_Fog")) {
        const char* fogsrc = hardext.highp ? gl4es_FogParametersSourceHighp : gl4es_FogParametersSource;
        Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), fogsrc, Tmp, &tmpsize);
        headline += gl4es_countline(fogsrc);
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_FogParameters", "_gl4es_FogParameters");
    }
    if (strstr(Tmp, "gl_Fog"))
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_Fog", "_gl4es_Fog");
    if (strstr(Tmp, "gl_TextureEnvColor")) {
        Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), gl4es_texenvcolorSource, Tmp, &tmpsize);
        headline += gl4es_countline(gl4es_texenvcolorSource);
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_TextureEnvColor", "_gl4es_TextureEnvColor");
    }
    // TexGen planes
    static const char* eyeNames[]  = {"gl_EyePlaneS","gl_EyePlaneT","gl_EyePlaneR","gl_EyePlaneQ"};
    static const char* eyeInternalNames[] = {"_gl4es_EyePlaneS","_gl4es_EyePlaneT","_gl4es_EyePlaneR","_gl4es_EyePlaneQ"};
    static const char* objNames[]  = {"gl_ObjectPlaneS","gl_ObjectPlaneT","gl_ObjectPlaneR","gl_ObjectPlaneQ"};
    static const char* objInternalNames[] = {"_gl4es_ObjectPlaneS","_gl4es_ObjectPlaneT","_gl4es_ObjectPlaneR","_gl4es_ObjectPlaneQ"};
    for (int i = 0; i < 4; i++) {
        if (strstr(Tmp, eyeNames[i])) {
            Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), gl4es_texgeneyeSource[i], Tmp, &tmpsize);
            headline += gl4es_countline(gl4es_texgeneyeSource[i]);
            Tmp = gl4es_inplace_replace(Tmp, &tmpsize, eyeNames[i], eyeInternalNames[i]);
        }
        if (strstr(Tmp, objNames[i])) {
            Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), gl4es_texgenobjSource[i], Tmp, &tmpsize);
            headline += gl4es_countline(gl4es_texgenobjSource[i]);
            Tmp = gl4es_inplace_replace(Tmp, &tmpsize, objNames[i], objInternalNames[i]);
        }
    }
    if (strstr(Tmp, "gl_MaxTextureUnits")) {
        Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, 2), gl4es_MaxTextureUnitsSource, Tmp, &tmpsize);
        headline += gl4es_countline(gl4es_MaxTextureUnitsSource);
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_MaxTextureUnits", "_gl4es_MaxTextureUnits");
    }
    if (strstr(Tmp, "gl_MaxTextureCoords")) {
        Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, 2), gl4es_MaxTextureCoordsSource, Tmp, &tmpsize);
        headline += gl4es_countline(gl4es_MaxTextureCoordsSource);
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_MaxTextureCoords", "_gl4es_MaxTextureCoords");
    }

    // ── gl_ClipVertex ─────────────────────────────────────────────────────
    if (strstr(Tmp, "gl_ClipVertex")) {
        Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, 2), gl4es_ClipVertex, Tmp, &tmpsize);
        headline += gl4es_countline(gl4es_ClipVertex);
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "gl_ClipVertex", gl4es_ClipVertexSource);
        need->need_clipvertex = 1;
    } else if (isVertex && need && need->need_clipvertex) {
        Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, 2), gl4es_ClipVertex, Tmp, &tmpsize);
        headline += gl4es_countline(gl4es_ClipVertex);
        char* p = strchr(gl4es_find_string_nc(Tmp, "main"), '{');
        if (p) Tmp = gl4es_inplace_insert(p + 1, gl4es_ClipVertex_clip, Tmp, &tmpsize);
    }

    // ── gl_ProgramEnv / gl_ProgramLocal (ARB programs) ───────────────────
    #define HANDLE_PROG_UNIFORM(gl_name, internal_prefix, maxv_vtx, maxv_frg)  \
    if (gl4es_find_string(Tmp, gl_name)) {                                      \
        int maxind = -1; int noarray = 1;                                       \
        char* p = Tmp;                                                          \
        while (noarray && (p = gl4es_find_string_nc(p, gl_name))) {            \
            p += strlen(gl_name);                                               \
            if (*p == '[') { ++p;                                               \
                if (*p >= '0' && *p <= '9') {                                  \
                    int n = (*p) - '0';                                         \
                    if (p[1] >= '0' && p[1] <= '9') n = n*10 + (p[1]-'0');    \
                    if (maxind < n) maxind = n;                                 \
                } else noarray = 0;                                             \
            } else noarray = 0;                                                 \
        }                                                                       \
        char F[60], T[60], U[300];                                              \
        if (noarray) {                                                          \
            for (int i = 0; i <= maxind; ++i) {                                \
                sprintf(F, "%s[%d]", gl_name, i);                              \
                sprintf(T, "_gl4es_%s_%s_%d", isVertex?"Vertex":"Fragment",    \
                        internal_prefix, i);                                    \
                Tmp = gl4es_inplace_replace(Tmp, &tmpsize, F, T);              \
                if (gl4es_find_string(Tmp, T)) {                               \
                    sprintf(U, "uniform vec4 %s;\n", T);                       \
                    Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), U, Tmp, &tmpsize); \
                    headline++;                                                 \
                }                                                               \
            }                                                                   \
        } else {                                                                \
            sprintf(T, "_gl4es_%s_%s", isVertex?"Vertex":"Fragment", internal_prefix); \
            sprintf(U, "uniform vec4 %s[%d];\n", T, isVertex?maxv_vtx:maxv_frg); \
            Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), U, Tmp, &tmpsize); \
            headline++;                                                         \
            Tmp = gl4es_inplace_replace(Tmp, &tmpsize, gl_name, T);            \
        }                                                                       \
    }

    HANDLE_PROG_UNIFORM("gl_ProgramEnv",   "ProgramEnv",   MAX_VTX_PROG_ENV_PARAMS, MAX_FRG_PROG_ENV_PARAMS)
    HANDLE_PROG_UNIFORM("gl_ProgramLocal", "ProgramLocal", MAX_VTX_PROG_LOC_PARAMS, MAX_FRG_PROG_LOC_PARAMS)
    #undef HANDLE_PROG_UNIFORM

    // ── Sampler replacements ──────────────────────────────────────────────
    #define GO(A)                                                                     \
    if (strstr(Tmp, gl_Samplers ## A)) {                                              \
        char S[60], D[60], U[80];                                                     \
        for (int i = 0; i < MAX_TEX; ++i) {                                          \
            sprintf(S, "%s%d", gl_Samplers ## A, i);                                 \
            if (gl4es_find_string(Tmp, S)) {                                          \
                sprintf(D, "%s%d", gl4es_Samplers ## A, i);                          \
                sprintf(U, "%s%d;\n", gl4es_Samplers ## A ## _uniform, i);           \
                Tmp = gl4es_inplace_replace(Tmp, &tmpsize, S, D);                     \
                Tmp = gl4es_inplace_insert(gl4es_getline(Tmp, headline), U, Tmp, &tmpsize); \
                headline++;                                                            \
            }                                                                         \
        }                                                                             \
    }
    GO(1D) GO(2D) GO(3D) GO(Cube)
    #undef GO

    // ── Non-square matrix normalization ──────────────────────────────────
    if (strstr(Tmp, "mat2x2"))
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "mat2x2", "mat2");
    if (strstr(Tmp, "mat3x3"))
        Tmp = gl4es_inplace_replace(Tmp, &tmpsize, "mat3x3", "mat3");

    // ── Debug log setelah konversi ────────────────────────────────────────
    if ((globals4es.dbgshaderconv & maskafter) == maskafter)
        printf("New Shader source:\n%s\n", Tmp);

    // ── Cleanup ───────────────────────────────────────────────────────────
    if (versionString != NULL) free(versionString);
    if (pEntry != pBuffer)     free(pBuffer);

    return Tmp;
    #undef ShadAppend
}

// =============================================================================
//  Helper functions — IDENTIK dengan original
// =============================================================================

int isBuiltinAttrib(const char* name) {
    int n = sizeof(builtin_attrib) / sizeof(builtin_attrib_t);
    for (int i = 0; i < n; i++)
        if (strcmp(builtin_attrib[i].name, name) == 0)
            return builtin_attrib[i].attrib;
    return -1;
}

int isBuiltinMatrix(const char* name) {
    int ret = -1;
    int n = sizeof(builtin_matrix) / sizeof(builtin_matrix_t);
    for (int i = 0; i < n && ret == -1; i++) {
        if (strncmp(builtin_matrix[i].name, name, strlen(builtin_matrix[i].name)) == 0) {
            int l = strlen(builtin_matrix[i].name);
            if (strlen(name) == (size_t)l ||
               (strlen(name) == (size_t)(l+3) && name[l] == '[' && builtin_matrix[i].texarray) ||
               (strlen(name) == (size_t)(l+4) && name[l] == '[' && builtin_matrix[i].texarray)) {
                ret = builtin_matrix[i].matrix;
                if (builtin_matrix[i].texarray) {
                    int n2 = name[l+1] - '0';
                    if (name[l+2] >= '0' && name[l+2] <= '9')
                        n2 = n2 * 10 + name[l+2] - '0';
                    ret += n2 * 4;
                }
            }
        }
    }
    return ret;
}

const char* hasBuiltinAttrib(const char* vertexShader, int Att) {
    if (!vertexShader) return NULL;
    const char* ret = NULL;
    if (hardext.maxvattrib > 8) {
        int n = sizeof(builtin_attrib) / sizeof(builtin_attrib_t);
        for (int i = 0; i < n && !ret; i++)
            if (builtin_attrib[i].attrib == Att)
                ret = builtin_attrib[i].name;
    } else {
        int n = sizeof(builtin_attrib_compressed) / sizeof(builtin_attrib_t);
        for (int i = 0; i < n && !ret; i++)
            if (builtin_attrib_compressed[i].attrib == Att)
                ret = builtin_attrib_compressed[i].name;
    }
    if (!ret) return NULL;
    if (strstr(vertexShader, ret)) return ret;
    if (strstr(vertexShader, gl4es_VA[Att])) return gl4es_VA[Att];
    return NULL;
}

const char* builtinAttribGLName(const char* name) {
    int n = sizeof(builtin_attrib) / sizeof(builtin_attrib_t);
    for (int i = 0; i < n; ++i)
        if (!strcmp(name, builtin_attrib[i].name))
            return builtin_attrib[i].glname;
    if (!strncmp(name, gl4es_VertexAttrib, strlen(gl4es_VertexAttrib))) {
        int l = strlen(gl4es_VertexAttrib), n2 = 0;
        while (name[l] >= '0' && name[l] <= '9') n2 = n2 * 10 + name[l++] - '0';
        return gl_VA[n2];
    }
    return name;
}

const char* builtinAttribInternalName(const char* name) {
    int n = sizeof(builtin_attrib) / sizeof(builtin_attrib_t);
    for (int i = 0; i < n; ++i)
        if (!strcmp(name, builtin_attrib[i].glname))
            return builtin_attrib[i].name;
    if (!strncmp(name, gl_VertexAttrib, strlen(gl_VertexAttrib))) {
        int l = strlen(gl_VertexAttrib), n2 = 0;
        while (name[l] >= '0' && name[l] <= '9') n2 = n2 * 10 + name[l++] - '0';
        return gl4es_VA[n2];
    }
    return name;
}
