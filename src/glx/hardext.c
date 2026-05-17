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
#define EGL_PLATFORM_GBM_KHR                     0x31D7
#endif

static int tested = 0;

hardext_t hardext = {0};

char* gl4es_original_vendor = NULL;
char* gl4es_original_renderer = NULL;

static int testGLSL(const char* version, int uniformLoc) {
    // check if glsl 120 shaders are supported... by compiling one !
    LOAD_GLES2(glCreateShader);
    LOAD_GLES2(glShaderSource);
    LOAD_GLES2(glCompileShader);
    LOAD_GLES2(glGetShaderiv);
    LOAD_GLES2(glDeleteShader);
    LOAD_GLES(glGetError);

    GLuint shad = gles_glCreateShader(GL_VERTEX_SHADER);
    const char* shadTest[4] = {
        version,
        /* Guard with enable (not require) so a missing extension is a warning,
         * not a hard failure. Correct GLSL syntax: "#extension name : behavior" */
        "#extension GL_IMG_uniform_buffer_object : enable"
        "\n"
        "layout(location = 0) in vec4 vecPos;\n",
        uniformLoc?"layout(location = 0) uniform mat4 matMVP;\n":"uniform mat4 matMVP;\n",
        "void main() {\n"
        " gl_Position = matMVP * vecPos;\n"
        "}\n"
    };
    gles_glShaderSource(shad, 4, shadTest, NULL);
    gles_glCompileShader(shad);
    GLint compiled;
    gles_glGetShaderiv(shad, GL_COMPILE_STATUS, &compiled);
    /*
    if(!compiled) {
        LOAD_GLES2(glGetShaderInfoLog)
        char buff[500];
        gles_glGetShaderInfoLog(shad, 500, NULL, buff);
        printf("LIBGL: \"%s\" failed, message:\n%s\n", version, buff);
    }
    */
    gles_glDeleteShader(shad);
    gles_glGetError();	// reset GL Error

    return compiled;
}

static int testTextureCubeLod() {
    LOAD_GLES2(glCreateShader);
    LOAD_GLES2(glShaderSource);
    LOAD_GLES2(glCompileShader);
    LOAD_GLES2(glGetShaderiv);
    LOAD_GLES2(glDeleteShader);
    LOAD_GLES(glGetError);

    GLuint shad = gles_glCreateShader(GL_FRAGMENT_SHADER);
    const char* shadTest[3] = {
        "#version 100",
        "\n"
        "#extension GL_EXT_shader_texture_lod : enable\n"
        "uniform samplerCube samCube;\n"
        "varying mediump vec3 coordCube;\n",
        "void main() {\n"
        " gl_FragColor = textureCubeLod(samCube, coordCube, 0.0);\n"
        "}\n"
    };
    gles_glShaderSource(shad, 3, shadTest, NULL);
    gles_glCompileShader(shad);
    GLint compiled;
    gles_glGetShaderiv(shad, GL_COMPILE_STATUS, &compiled);
    gles_glDeleteShader(shad);
    gles_glGetError(); // reset GL Error

    return compiled;
}

EXPORT
void GetHardwareExtensions(int notest)
{
    if(tested) return;
    // put some default values
    hardext.maxtex = 2;
    hardext.maxsize = 2048;
    hardext.maxlights = 8;
    hardext.maxplanes = 6;
    hardext.maxdrawbuffers = 1;

    hardext.esversion = globals4es.es;
    if(notest) 
    {
#ifndef AMIGAOS4
        SHUT_LOGD("Hardware test disabled, nothing activated...\n");
#endif
        if(hardext.esversion>=2) {
            hardext.maxteximage = 4;
            hardext.maxvarying = 8;
            hardext.maxtex = 8;
            hardext.maxvattrib = 16;
#ifdef AMIGAOS4
            hardext.npot = 3;
#else
            // ES2: limited NPOT; ES3+ guarantees full NPOT (core feature)
            hardext.npot = (hardext.esversion>=3) ? 3 : 1;
#endif
            hardext.fbo = 1; 
            hardext.blendcolor = 1;
            hardext.blendsub = 1;
            hardext.blendfunc = 1;
            hardext.blendeq = 1;
            hardext.mirrored = 1;
            hardext.pointsprite = 1;
            hardext.pointsize = 1;
            hardext.cubemap = 1;
            hardext.maxdrawbuffers = 1;
#ifdef AMIGAOS4
            hardext.glsl300es = 1;
#else
            // ES 3.x context guarantees GLSL ES 3.00+ support
            if(hardext.esversion>=3) {
                hardext.glsl300es = 1;
                hardext.glsl310es = 1;
                // ES 3.2 additionally guarantees GLSL ES 3.20
                if(hardext.esversion>=3 && hardext.es_minor>=2)
                    hardext.glsl320es = 1;
            }
#endif
        }
        return;
    }
#if defined(BCMHOST) && !defined(ANDROID)
    rpi_init();
#endif
#ifdef NOEGL
    SHUT_LOGD("Hardware test on current Context...\n");
#else
    // used EGL & GLES functions
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
    LOAD_EGL(eglGetError);  /* needed to consume errors in 3-tier context fallback */

    EGLDisplay eglDisplay;
    EGLSurface eglSurface;
    EGLContext eglContext = EGL_NO_CONTEXT;  /* must be initialized — checked after 3-tier block */

    // -------------------------------------------------------------------------
    // Context attribute arrays for each ES version tier.
    // EGL_CONTEXT_MAJOR_VERSION_KHR (0x3098) is an alias for
    // EGL_CONTEXT_CLIENT_VERSION, accepted since EGL_KHR_create_context.
    // -------------------------------------------------------------------------

    // Tier 1 — ES 3.2: exposes GLSL ES 3.20, native VAO, full NPOT, UBO, etc.
    EGLint egl_ctx_es32[] = {
        EGL_CONTEXT_MAJOR_VERSION_KHR, 3,
        EGL_CONTEXT_MINOR_VERSION_KHR, 2,
        EGL_NONE
    };
    // Tier 2 — ES 3.0: native VAO, UBO, instancing, GLSL ES 3.00
    EGLint egl_ctx_es30[] = {
        EGL_CONTEXT_MAJOR_VERSION_KHR, 3,
        EGL_CONTEXT_MINOR_VERSION_KHR, 0,
        EGL_NONE
    };
    // Tier 3 — ES 2.0: original gl4es behavior, zero regression
    EGLint egl_ctx_es2[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    // ES 1.x: no version attribute needed
    EGLint egl_ctx_es1[] = {
        EGL_NONE
    };

    // PBuffer surface dimensions (32x32 is enough for capability probing)
    EGLint egl_attribs[] = {
        EGL_WIDTH,  32,
        EGL_HEIGHT, 32,
        EGL_NONE
    };

    // -------------------------------------------------------------------------
    // Determine the EGL_RENDERABLE_TYPE bit for eglChooseConfig.
    // EGL_OPENGL_ES3_BIT_KHR (0x0040) is a superset of ES2_BIT — any config
    // advertising ES3 also supports ES2 contexts, so we never need to re-query
    // the config when falling back from ES3 to ES2 context creation.
    // -------------------------------------------------------------------------
    EGLint renderableType;
    if (hardext.esversion == 1)
        renderableType = EGL_OPENGL_ES_BIT;
    else if (hardext.esversion >= 3)
        renderableType = EGL_OPENGL_ES3_BIT_KHR;
    else
        renderableType = EGL_OPENGL_ES2_BIT;

    EGLint configAttribs[] = {
#ifdef PANDORA
        EGL_RED_SIZE,   (hardext.esversion==1) ? 5 : 8,
        EGL_GREEN_SIZE, (hardext.esversion==1) ? 6 : 8,
        EGL_BLUE_SIZE,  (hardext.esversion==1) ? 5 : 8,
        EGL_ALPHA_SIZE, (hardext.esversion==1) ? 0 : 8,
#else
        EGL_RED_SIZE,   8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE,  8,
        EGL_ALPHA_SIZE, 8,
#endif
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,   /* overwritten below */
        EGL_NONE
    };
    // Index of the EGL_RENDERABLE_TYPE *value* (not the token itself).
    // Layout: RED(0,1) GREEN(2,3) BLUE(4,5) ALPHA(6,7) SURFACE(8,9) RENDER(10,11) NONE(12)
    configAttribs[11] = renderableType;

    int configsFound;
    static EGLConfig pbufConfigs[1];

#ifndef NO_GBM
    if (globals4es.usegbm) {
        LoadGBMFunctions();
        eglDisplay = OpenGBMDisplay(EGL_DEFAULT_DISPLAY);
    } else
#endif
        eglDisplay = egl_eglGetDisplay(EGL_DEFAULT_DISPLAY);

    egl_eglBindAPI(EGL_OPENGL_ES_API);
    if (egl_eglInitialize(eglDisplay, NULL, NULL) != EGL_TRUE) {
        LOGE("EGL init failed (%s), skipping hardware detection\n", PrintEGLError(0));
        egl_eglTerminate(eglDisplay);
        return;
    }

    egl_eglChooseConfig(eglDisplay, configAttribs, pbufConfigs, 1, &configsFound);

#ifndef NO_GBM
    const char* eglExts = egl_eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (eglExts && strstr(eglExts, "EGL_KHR_platform_gbm ")) {
        SHUT_LOGD("GBM Surfaces supported%s\n", globals4es.usegbm ? " and used" : "");
        hardext.gbm = 1;
    }
#endif

#ifndef PANDORA
    if (!configsFound) {
        // Retry without alpha channel (some drivers omit 8888 PBuffer configs)
        configAttribs[7] = 0;
        egl_eglChooseConfig(eglDisplay, configAttribs, pbufConfigs, 1, &configsFound);
        if (configsFound) {
            SHUT_LOGD("No alpha in PBuffer config, disabling EGL alpha channel\n");
            hardext.eglnoalpha = 1;
        }
    }
    // If ES3 config not found, gracefully downgrade to ES2 config
    if (!configsFound && hardext.esversion >= 3) {
        SHUT_LOGD("ES3 EGL config unavailable, downgrading config to ES2\n");
        configAttribs[11] = EGL_OPENGL_ES2_BIT;
        configAttribs[7]  = 8;  // restore alpha
        egl_eglChooseConfig(eglDisplay, configAttribs, pbufConfigs, 1, &configsFound);
        if (!configsFound) {
            configAttribs[7] = 0;
            egl_eglChooseConfig(eglDisplay, configAttribs, pbufConfigs, 1, &configsFound);
        }
        if (configsFound) {
            hardext.esversion = 2;
            globals4es.es     = 2;
        }
    }
#endif

    if (!configsFound) {
        SHUT_LOGE("eglChooseConfig failed (%s), skipping hardware detection\n", PrintEGLError(0));
        egl_eglTerminate(eglDisplay);
        return;
    }

    // -------------------------------------------------------------------------
    // 3-Tier EGL context negotiation
    //   Tier 1: ES 3.2 — modern features, GLSL ES 3.20
    //   Tier 2: ES 3.0 — VAO/UBO/instancing, GLSL ES 3.00 (fallback)
    //   Tier 3: ES 2.0 — original gl4es path, guaranteed on all targets
    // eglCreateContext returns EGL_NO_CONTEXT on failure; no crash, no assert.
    // -------------------------------------------------------------------------
    if (hardext.esversion >= 3) {
        // --- Tier 1: ES 3.2 ---
        eglContext = egl_eglCreateContext(eglDisplay, pbufConfigs[0],
                                          EGL_NO_CONTEXT, egl_ctx_es32);
        if (eglContext) {
            hardext.es_minor = 2;
            SHUT_LOGD("EGL ES 3.2 context negotiated successfully\n");
        } else {
            egl_eglGetError();  // consume EGL_BAD_MATCH or similar
            // --- Tier 2: ES 3.0 ---
            eglContext = egl_eglCreateContext(eglDisplay, pbufConfigs[0],
                                              EGL_NO_CONTEXT, egl_ctx_es30);
            if (eglContext) {
                hardext.es_minor = 0;
                SHUT_LOGD("EGL ES 3.0 context negotiated (ES 3.2 unavailable)\n");
            } else {
                egl_eglGetError();
                SHUT_LOGD("ES 3.x context failed, downgrading to ES 2.0\n");
                // Tier 3 path: set version to 2 so the block below handles it
                hardext.esversion = 2;
                globals4es.es     = 2;
            }
        }
    }

    // ES 2.0 / ES 1.x path — either directly requested or downgraded from ES3
    if (!eglContext) {
        eglContext = egl_eglCreateContext(eglDisplay, pbufConfigs[0],
                                          EGL_NO_CONTEXT,
                                          (hardext.esversion == 1)
                                              ? egl_ctx_es1
                                              : egl_ctx_es2);
    }

    if (!eglContext) {
        SHUT_LOGE("eglCreateContext failed (%s), skipping hardware detection\n",
                  PrintEGLError(0));
        egl_eglTerminate(eglDisplay);
        return;
    }

    eglSurface = egl_eglCreatePbufferSurface(eglDisplay, pbufConfigs[0], egl_attribs);
    if (!eglSurface) {
        SHUT_LOGE("eglCreatePbufferSurface failed (%s)\n", PrintEGLError(0));
        egl_eglDestroyContext(eglDisplay, eglContext);
        egl_eglTerminate(eglDisplay);
        return;
    }

    egl_eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext);
#endif
    // -------------------------------------------------------------------------
    // Verify the *actual* ES version the driver negotiated.
    // We use GL_MAJOR_VERSION / GL_MINOR_VERSION (core in ES 3.x) for precision,
    // and fall back to parsing glGetString(GL_VERSION) for ES 2.0/1.x.
    // This guards against drivers that silently downgrade the context version.
    // -------------------------------------------------------------------------
    {
        LOAD_GLES2(glGetIntegerv);
        LOAD_GLES2(glGetString);

        if (hardext.esversion >= 3 && gles_glGetIntegerv) {
            /* GL_MAJOR_VERSION (0x821B) and GL_MINOR_VERSION (0x821C) are core
             * in ES 3.0+ but may not be in every project header chain.
             * Use the raw token values with a guard to stay header-independent. */
#ifndef GL_MAJOR_VERSION
#define GL_MAJOR_VERSION 0x821B
#define GL_MINOR_VERSION 0x821C
#define GL4ES_DEFINED_MAJOR_MINOR
#endif
            GLint actual_major = 0, actual_minor = 0;
            gles_glGetIntegerv(GL_MAJOR_VERSION, &actual_major);
            gles_glGetIntegerv(GL_MINOR_VERSION, &actual_minor);
#ifdef GL4ES_DEFINED_MAJOR_MINOR
#undef GL_MAJOR_VERSION
#undef GL_MINOR_VERSION
#undef GL4ES_DEFINED_MAJOR_MINOR
#endif
            if (actual_major >= 3) {
                hardext.esversion = actual_major;
                hardext.es_minor  = actual_minor;
            } else if (actual_major > 0) {
                // Driver silently gave us a lower version
                SHUT_LOGD("Warning: driver gave ES %d.%d despite ES3 request\n",
                          actual_major, actual_minor);
                hardext.esversion = actual_major;
                hardext.es_minor  = actual_minor;
                globals4es.es     = actual_major;
            }
        } else if (gles_glGetString) {
            const char *glVer = (const char *)gles_glGetString(GL_VERSION);
            if (glVer) {
                // Format: "OpenGL ES <major>.<minor> ..."
                const char *p = strstr(glVer, "OpenGL ES ");
                if (p) {
                    int maj = 0, min = 0;
                    if (sscanf(p + 10, "%d.%d", &maj, &min) == 2) {
                        hardext.es_minor = min;
                        if (maj != hardext.esversion) {
                            SHUT_LOGD("Warning: esversion mismatch: requested %d, got %d\n",
                                      hardext.esversion, maj);
                            hardext.esversion = maj;
                            globals4es.es     = maj;
                        }
                    }
                }
            }
        }

        SHUT_LOGD("GLES backend confirmed: ES %d.%d\n",
                  hardext.esversion, hardext.es_minor);
    }

    tested = 1;
    LOAD_GLES(glGetString);
    LOAD_GLES(glGetIntegerv);
    LOAD_GLES(glGetError);

    // -------------------------------------------------------------------------
    // ES 3.0+ deprecates glGetString(GL_EXTENSIONS) — it returns NULL and sets
    // GL_INVALID_ENUM. We must check for NULL before any strstr() call.
    // For ES3 contexts, core features are guaranteed without extension lookup.
    // -------------------------------------------------------------------------
    const char *Exts = (const char *) gles_glGetString(GL_EXTENSIONS);
    // Consume any GL_INVALID_ENUM generated by the call above on ES3 contexts
    gles_glGetError();

    // ES3 NPOT: full non-power-of-two is a core feature — no extension needed.
    if(hardext.esversion >= 3) {
        hardext.npot = 3;
        SHUT_LOGD("Full NPOT is core in ES 3.x, enabled\n");
    } else if(hardext.esversion > 1) {
        hardext.npot = 1;
    }

    // Safe strstr wrapper — Exts may be NULL on ES3 contexts
    #define S(A, B, C) if(Exts && strstr(Exts, A)) { hardext.B = 1; SHUT_LOGD("Extension %s detected%s",A, C?" and used\n":"\n"); }
    if(Exts) {
        if(strstr(Exts, "GL_APPLE_texture_2D_limited_npot ")) hardext.npot = 1;
        if(strstr(Exts, "GL_IMG_texture_npot "))              hardext.npot = 1;
        if(strstr(Exts, "GL_ARB_texture_non_power_of_two ") || strstr(Exts, "GL_OES_texture_npot ")) hardext.npot = 3;
    }
    if(hardext.npot > 0) {
        SHUT_LOGD("Hardware %s NPOT in use\n", hardext.npot==3?"Full":(hardext.npot==2?"Limited+Mipmap":"Limited"));
    }
    // GL_EXT_blend_minmax is core in ES 3.0
    if(hardext.esversion >= 3) {
        hardext.blendminmax = 1;
        SHUT_LOGD("GL_EXT_blend_minmax is core in ES3, enabled\n");
    } else {
        S("GL_EXT_blend_minmax ", blendminmax, 1);
    }
    if (hardext.esversion>2) {
        SHUT_LOGD("Extension GL_EXT_draw_buffers is in core ES3, and so used\n");
        hardext.drawbuffers = 1;
    } else {
        S("GL_EXT_draw_buffers ", drawbuffers, 1);
    }
    /*if(hardext.blendcolor==0) {
        // try by just loading the function
        LOAD_GLES_OR_OES(glBlendColor);
        if(gles_glBlendColor != NULL) {
            hardext.blendcolor = 1;
	        SHUT_LOGD("Extension glBlendColor found and used\n");
	    }
    }*/ // I don't think this is correct
    if(hardext.esversion<2) {
        S("GL_OES_framebuffer_object ", fbo, 1);
        S("GL_OES_point_sprite ", pointsprite, 1); 
        S("GL_OES_point_size_array ", pointsize, 0);
        S("GL_OES_texture_cube_map ", cubemap, 1);
        S("GL_EXT_blend_color ", blendcolor, 1);
        S("GL_OES_blend_subtract ", blendsub, 1);
        S("GL_OES_blend_func_separate ", blendfunc, 1);
        S("GL_OES_blend_equation_separate ", blendeq, 1);
        S("GL_OES_texture_mirrored_repeat ", mirrored, 1);  
    } else {
        hardext.fbo = 1; 
        SHUT_LOGD("FBO are in core, and so used\n");
        hardext.pointsprite = 1;
        SHUT_LOGD("PointSprite are in core, and so used\n");
        hardext.pointsize = 1;
        SHUT_LOGD("CubeMap are in core, and so used\n");
        hardext.cubemap = 1;
        SHUT_LOGD("BlendColor is in core, and so used\n");
        hardext.blendcolor = 1;
        SHUT_LOGD("Blend Subtract is in core, and so used\n");
        hardext.blendsub = 1;
        SHUT_LOGD("Blend Function and Equation Separation is in core, and so used\n");
        hardext.blendfunc = 1;
        hardext.blendeq = 1;
        SHUT_LOGD("Texture Mirrored Repeat is in core, and so used\n");
        hardext.mirrored = 1;
    }
    S("GL_OES_mapbuffer ", mapbuffer, 0);
    // These are all guaranteed core in ES 3.0 — no extension lookup needed.
    // OES_element_index_uint, OES_packed_depth_stencil, OES_depth24, OES_rgb8_rgba8,
    // EXT_multi_draw_arrays are all promoted to core by the ES 3.0 specification.
    if(hardext.esversion >= 3) {
        hardext.elementuint  = 1;   // 32-bit index buffers
        hardext.depthstencil = 1;   // packed depth+stencil
        hardext.depth24      = 1;   // 24-bit depth
        hardext.rgba8        = 1;   // RGBA8 renderbuffer
        hardext.multidraw    = 1;   // glMultiDrawArrays/Elements
        SHUT_LOGD("ES3 core: elementuint, depthstencil, depth24, rgba8, multidraw enabled\n");
    } else {
        S("GL_OES_element_index_uint ", elementuint, 1);
        S("GL_OES_packed_depth_stencil ", depthstencil, 1);
        S("GL_OES_depth24 ", depth24, 1);
        S("GL_OES_rgb8_rgba8 ", rgba8, 1);
        S("GL_EXT_multi_draw_arrays ", multidraw, 0);
    }
    if(!globals4es.nobgra) {
        S("GL_EXT_texture_format_BGRA8888 ", bgra8888, 1);
    }
    if(!globals4es.nodepthtex) {
        // OES_depth_texture is core in ES 3.0 (depth textures guaranteed)
        // OES_texture_stencil8 is core in ES 3.2
        if(hardext.esversion >= 3) {
            hardext.depthtex = 1;
            if(hardext.esversion >= 3 && hardext.es_minor >= 2)
                hardext.stenciltex = 1;
            SHUT_LOGD("ES3 core: depthtex enabled%s\n",
                      hardext.stenciltex ? ", stenciltex enabled" : "");
        } else {
            S("GL_OES_depth_texture ", depthtex, 1);
            S("GL_OES_texture_stencil8 ", stenciltex, 1);
        }
    }
    S("GL_OES_draw_texture ", drawtex, 1);
    // GL_EXT_texture_rg (R/RG formats) is core in ES 3.0
    if(hardext.esversion >= 3) {
        hardext.rgtex = 1;
        SHUT_LOGD("ES3 core: rgtex (R/RG texture formats) enabled\n");
    } else {
        S("GL_EXT_texture_rg ", rgtex, 1);
    }
    if(globals4es.floattex) {
        // Float/half-float textures are core in ES 3.0 (with caveats on format names)
        // GL_EXT_color_buffer_float is core in ES 3.2
        if(hardext.esversion >= 3) {
            hardext.floattex     = 1;
            hardext.halffloattex = 1;
            if(hardext.esversion >= 3 && hardext.es_minor >= 2) {
                hardext.floatfbo     = 1;
                hardext.halffloatfbo = 1;
                SHUT_LOGD("ES3 core: float/halffloat textures + fbo enabled (ES3.2)\n");
            } else {
                SHUT_LOGD("ES3 core: float/halffloat textures enabled (ES3.0)\n");
                // floatfbo via extension check on ES3.0/3.1 (Exts may be NULL)
                S("GL_EXT_color_buffer_float ", floatfbo, 1);
                S("GL_EXT_color_buffer_half_float ", halffloatfbo, 1);
            }
        } else {
            S("GL_OES_texture_float ", floattex, 1);
            S("GL_OES_texture_half_float ", halffloattex, 1);
            S("GL_EXT_color_buffer_float ", floatfbo, 1);
            S("GL_EXT_color_buffer_half_float ", halffloatfbo, 1);
        }
    }
    S("GL_AOS4_texture_format_RGB332", rgb332, 0);
    S("GL_AOS4_texture_format_RGB332REV", rgb332rev, 0);
    S("GL_AOS4_texture_format_RGBA1555REV", rgba1555rev, 1);
    S("GL_AOS4_texture_format_RGBA8888", rgba8888, 1);
    S("GL_AOS4_texture_format_RGBA8888REV", rgba8888rev, 1);

    if (hardext.esversion>1) {
        // -----------------------------------------------------------------
        // ES 3.0+ promotes many OES extensions to core.
        // Set them unconditionally for ES3 to avoid depending on Exts string.
        // -----------------------------------------------------------------
        if(hardext.esversion >= 3) {
            // highp in fragment is mandatory in ES 3.0 (section 4.5.4 of spec)
            if(!globals4es.nohighp)
                hardext.highp = 2;
            // textureLod / textureGrad are core GLSL ES 3.00 — no extension needed
            if(!globals4es.noshaderlod)
                hardext.shaderlod = 1;
            // gl_FragDepth is core in GLSL ES 3.00
            hardext.fragdepth = 1;
            // dFdx/dFdy are core in ES 3.0
            hardext.derivatives = 1;
            // glMapBufferRange is core in ES 3.0 (replaces OES_mapbuffer)
            hardext.mapbuffer = 1;
            SHUT_LOGD("ES3 core features activated: highp, shaderlod, fragdepth, derivatives, mapbuffer\n");
        } else {
            // ES 2.0 path: check via extension string
            if(!globals4es.nohighp) {
                S("GL_OES_fragment_precision_high ", highp, 1);
                if(!hardext.highp) {
                    LOAD_GLES2(glGetShaderPrecisionFormat);
                    if(gles_glGetShaderPrecisionFormat) {
                        GLint range[2] = {0};
                        GLint precision = 0;
                        gles_glGetShaderPrecisionFormat(GL_FRAGMENT_SHADER, GL_HIGH_FLOAT, range, &precision);
                        if(!(range[0]==0 && range[1]==0 && precision==0)) {
                            hardext.highp = 2;
                            SHUT_LOGD("high precision float in fragment shader available and used\n");
                        }
                    }
                }
            }
            if(!globals4es.noshaderlod)
                S("GL_EXT_shader_texture_lod", shaderlod, 1);
            S("GL_EXT_frag_depth ", fragdepth, 1);
            S("GL_OES_standard_derivatives ", derivatives, 1);
        }
        // cubelod test applies to both ES2 and ES3 (driver quirk detection)
        if(hardext.shaderlod) {
            if(testTextureCubeLod()) {
                hardext.cubelod = 1;
                SHUT_LOGD("textureCubeLod in fragment doesn't need trailing EXT\n");
            }
        }
        gles_glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &hardext.maxvattrib);
        SHUT_LOGD("Max vertex attrib: %d\n", hardext.maxvattrib);
        S("GL_ARM_shader_framebuffer_fetch", shader_fbfetch, 1);
        // Program binary: ES3 always has it; ES2 needs extension
        if(hardext.esversion >= 3) {
            hardext.prgbinary = 1;
        } else {
            S("GL_OES_get_program ", prgbinary, 1);
            if(!hardext.prgbinary)
                S("GL_OES_get_program_binary ", prgbinary, 1);
        }
        if(hardext.prgbinary) {
            gles_glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS_OES, &hardext.prgbin_n);
            SHUT_LOGD("Number of supported Program Binary Format: %d\n", hardext.prgbin_n);
        }
    }
    // Now get some max stuffs
    gles_glGetIntegerv(GL_MAX_TEXTURE_SIZE, &hardext.maxsize);
    SHUT_LOGD("Max texture size: %d\n", hardext.maxsize);
    gles_glGetIntegerv((hardext.esversion==1)?GL_MAX_TEXTURE_UNITS:GL_MAX_TEXTURE_IMAGE_UNITS, &hardext.maxtex);
    if (hardext.esversion==1) {
        gles_glGetIntegerv(GL_MAX_LIGHTS, &hardext.maxlights);
        gles_glGetIntegerv(GL_MAX_CLIP_PLANES, &hardext.maxplanes);
        hardext.maxteximage=hardext.maxtex;
    } else {
        // simulated stuff using the FPE
        hardext.maxlights = 8;
        hardext.maxplanes = 6;
        gles_glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &hardext.maxteximage);
        gles_glGetIntegerv(GL_MAX_VARYING_VECTORS, &hardext.maxvarying);
        SHUT_LOGD("Max Varying Vector: %d\n", hardext.maxvarying);
        if(hardext.maxvattrib<16 && hardext.maxtex>4)
            hardext.maxtex = 4; // with less then 16 vertexattrib, more then 4 textures seems unreasonnable
    }
    int hardmaxtex = hardext.maxtex;
    if(hardext.maxtex>MAX_TEX) hardext.maxtex=MAX_TEX;      // caping, as there are some fixed-sized array...
    if(hardext.maxteximage>MAX_TEX) hardext.maxteximage=MAX_TEX;
    if(hardext.maxlights>MAX_LIGHT) hardext.maxlights=MAX_LIGHT;                // caping lights too
    if(hardext.maxplanes>MAX_CLIP_PLANES) hardext.maxplanes=MAX_CLIP_PLANES;    // caping planes, even 6 should be the max supported anyway
    SHUT_LOGD("Texture Units: %d/%d (hardware: %d), Max lights: %d, Max planes: %d\n", hardext.maxtex, hardext.maxteximage, hardmaxtex, hardext.maxlights, hardext.maxplanes);
    S("GL_EXT_texture_filter_anisotropic ", aniso, 1);
    if(hardext.aniso) {
        gles_glGetIntegerv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &hardext.aniso);
        if(gles_glGetError()!=GL_NO_ERROR)
            hardext.aniso = 0;
        if(hardext.aniso)
            SHUT_LOGD("Max Anisotropic filtering: %d\n", hardext.aniso);
    }
    if(hardext.drawbuffers) {
        gles_glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS_EXT,&hardext.maxcolorattach);
        gles_glGetIntegerv(GL_MAX_DRAW_BUFFERS_ARB, &hardext.maxdrawbuffers);
    }
    if(hardext.maxcolorattach<1)
        hardext.maxcolorattach = 1;
    if(hardext.maxcolorattach>MAX_DRAW_BUFFERS)
        hardext.maxcolorattach=MAX_DRAW_BUFFERS;
    if(hardext.maxdrawbuffers<1)
        hardext.maxdrawbuffers = 1;
    if(hardext.maxdrawbuffers>MAX_DRAW_BUFFERS)
        hardext.maxdrawbuffers=MAX_DRAW_BUFFERS;
    SHUT_LOGD("Max Color Attachments: %d / Draw buffers: %d\n", hardext.maxdrawbuffers, hardext.maxcolorattach);
    // get GLES driver signatures...
    const char *vendor = (const char *) gles_glGetString(GL_VENDOR);
    if (!vendor) vendor = "Unknown";
    SHUT_LOGD("Hardware vendor is %s\n", vendor);
    if(!gl4es_original_vendor) {
        gl4es_original_vendor = strdup(vendor);
    }
    if(strstr(vendor, "ARM"))
        hardext.vendor = VEND_ARM;
    else if(strstr(vendor, "Imagination Technologies"))
        hardext.vendor = VEND_IMGTEC;
    if(hardext.esversion>1) {
        if(testGLSL("#version 120", 1))
            hardext.glsl120 = 1;
        if(testGLSL("#version 300 es", 0))
            hardext.glsl300es = 1;
        if(testGLSL("#version 310 es", 1))
            hardext.glsl310es = 1;
        // ES 3.2 context guarantees GLSL ES 3.20.
        // On confirmed ES 3.2 contexts the flag is set directly (no shader compile needed).
        // On ES 3.0/3.1 contexts we still test in case the driver supports it via extension.
        if(hardext.esversion >= 3 && hardext.es_minor >= 2) {
            hardext.glsl320es = 1;
        } else if(testGLSL("#version 320 es", 0)) {
            hardext.glsl320es = 1;
        }
    }
    if(!gl4es_original_renderer) {
        const char* renderer = (const char *) gles_glGetString(GL_RENDERER);
        gl4es_original_renderer = strdup(renderer);
    }
    if(hardext.glsl120)
        SHUT_LOGD("GLSL 120 supported and used\n");
    if(hardext.glsl300es)
        SHUT_LOGD("GLSL 300 es supported%s\n", (hardext.glsl120||hardext.glsl310es)?"":" and used");
    if(hardext.glsl310es)
        SHUT_LOGD("GLSL 310 es supported%s\n", hardext.glsl120?"":" and used");
    if(hardext.glsl320es)
        SHUT_LOGD("GLSL 320 es supported and used\n");

#ifndef NOEGL
    if(strstr(egl_eglQueryString(eglDisplay, EGL_EXTENSIONS), "EGL_KHR_gl_colorspace")) {
        SHUT_LOGD("sRGB surface supported\n");
        hardext.srgb = 1;
    }
    if(strstr(egl_eglQueryString(eglDisplay, EGL_EXTENSIONS), "EGL_KHR_image_pixmap")) {
        SHUT_LOGD("EGLImage from Pixmap supported\n");
        hardext.khr_pixmap = 1;
    }
    if(strstr(egl_eglQueryString(eglDisplay, EGL_EXTENSIONS), "EGL_KHR_gl_texture_2D_image")) {
        SHUT_LOGD("EGLImage to Texture2D supported\n");
        hardext.khr_texture_2d = 1;
    }
    if(strstr(egl_eglQueryString(eglDisplay, EGL_EXTENSIONS), "EGL_KHR_gl_renderbuffer_image")) {
        SHUT_LOGD("EGLImage to RenderBuffer supported\n");
        hardext.khr_renderbuffer = 1;
    }

    // End, cleanup
    egl_eglMakeCurrent(eglDisplay, 0, 0, EGL_NO_CONTEXT);
    egl_eglDestroySurface(eglDisplay, eglSurface);
    egl_eglDestroyContext(eglDisplay, eglContext);

    egl_eglTerminate(eglDisplay);
#endif
}
