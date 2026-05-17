# =============================================================================
#  GL4ES — Root CMakeLists.txt (GLES 3.2 Edition)
#
#  Perubahan dari original:
#  - CMake minimum: 2.8.12 → 3.16 (mendukung target_compile_options, Ninja, LTO)
#  - Android DEFAULT_ES: 2 → 3  (sesuai dengan init.c baru)
#  - Tambah opsi: GLES32_TARGET, LTO_MODE
#  - Perbaiki DEFAULT_ES logic: mendukung nilai 3
#  - Perbaiki Android block: tidak hardcode DEFAULT_ES di definitions
#  - Tambah LTO support via CMake (bukan via -flg hardcode)
#  - Tambah IPO/LTO toggle
# =============================================================================

cmake_minimum_required(VERSION 3.16)

# Import modules
include(CheckCompilerFlag)
include(CheckIPOSupported)

project(gl4es LANGUAGES C VERSION 1.1.7)

# ── Output directories ────────────────────────────────────────────────────────
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)
link_directories(${CMAKE_LIBRARY_OUTPUT_DIRECTORY})

# =============================================================================
#  Options
# =============================================================================
option(PANDORA        "Targeting OpenPandora device"                           ${PANDORA})
option(PYRA          "Targeting Dragonbox Pyra device"                        ${PYRA})
option(BCMHOST        "Targeting RPi(2) device"                                ${BCMHOST})
option(ODROID         "Targeting ODroid device"                                ${ODROID})
option(GOA_CLONE      "Targeting GO Advance clones (RG351p/v, Gameforce, ...)" ${GOA_CLONE})
option(ANDROID        "Targeting Android device"                               ${ANDROID})
option(CHIP           "Targeting C.H.I.P. device"                              ${CHIP})
option(AMIGAOS4       "Targeting AmigaOS4/Warp3D (forces NOEGL + NOX11)"      ${AMIGAOS4})
option(NOX11          "Don't use X11"                                          ${NOX11})
option(NOEGL          "Don't use EGL"                                          ${NOEGL})
option(STATICLIB      "Build a static version of gl4es"                        ${STATICLIB})
option(GBM            "Build GBM interface"                                    ${GBM})
option(USE_CCACHE     "Use ccache if present"                                  ${USE_CCACHE})
option(USE_CLOCK      "Use clock_gettime instead of gettimeofday for LIBGL_FPS" ${USE_CLOCK})
option(NO_LOADER      "Disable library loader"                                 ${NO_LOADER})
option(NO_INIT_CONSTRUCTOR "Disable automatic initialization"                  ${NO_INIT_CONSTRUCTOR})
option(DEFAULT_GLES   "Default GLES library path"                              ${DEFAULT_GLES})
option(DEFAULT_EGL    "Default EGL library path"                               ${DEFAULT_EGL})
option(DEFAULT_FB     "Default FB value (1/2/3)"                               ${DEFAULT_FB})
option(USE_ANDROID_LOG "Use Android log instead of stdio"                      ${USE_ANDROID_LOG})
option(EGL_WRAPPER    "Build EGL wrapper"                                      ${EGL_WRAPPER})
option(GLX_STUBS      "Build GLX function stubs"                               ${GLX_STUBS})

# ── Opsi baru ─────────────────────────────────────────────────────────────────
# GLES32_TARGET: aktifkan path ES 3.2 secara eksplisit (default ON untuk Android)
option(GLES32_TARGET  "Enable OpenGL ES 3.2 backend target"                    ${GLES32_TARGET})

# LTO_MODE: "none" | "thin" | "full"
set(LTO_MODE "thin" CACHE STRING "LTO mode: none, thin, or full")

# =============================================================================
#  Backtrace
# =============================================================================
include(CheckSymbolExists)
check_symbol_exists(backtrace "execinfo.h" HAS_BACKTRACE)
if(HAS_BACKTRACE)
    add_definitions(-DHAS_BACKTRACE)
endif()

# =============================================================================
#  Windows
# =============================================================================
if(CMAKE_SYSTEM_NAME MATCHES "Windows" OR CMAKE_SYSTEM_NAME MATCHES "MSYS")
    set(NOX11 ON)
    set(NO_GBM ON)
    set(WIN32_PLATFORM ON)
    if(CMAKE_C_COMPILER_ID MATCHES "MSVC" OR "x${CMAKE_C_SIMULATE_ID}" STREQUAL "xMSVC")
        set(WIN32_MSVC ON)
    endif()
endif()

# =============================================================================
#  DEFAULT_ES — Backend default
#
#  Nilai valid: 1 (ES 1.1) | 2 (ES 2.0) | 3 (ES 3.x)
#  Android: default 3, semua perangkat arm64 modern mendukung ES 3.2
#  Pandora: default 1 (SGX530, hanya ES 1.1)
#  Lainnya: default 2
# =============================================================================
if(NOT DEFAULT_ES)
    if(ANDROID)
        set(DEFAULT_ES 3)
    elseif(PANDORA)
        set(DEFAULT_ES 1)
    else()
        set(DEFAULT_ES 2)
    endif()
endif()

# Pass nilai DEFAULT_ES ke preprocessor
if(DEFAULT_ES EQUAL 3)
    add_definitions(-DDEFAULT_ES=3)
elseif(DEFAULT_ES EQUAL 2)
    add_definitions(-DDEFAULT_ES=2)
elseif(DEFAULT_ES EQUAL 1)
    add_definitions(-DDEFAULT_ES=1)
endif()
message(STATUS "Default ES backend: ${DEFAULT_ES}")

# =============================================================================
#  GBM
# =============================================================================
if((NOT GBM) OR PANDORA)
    set(NO_GBM ON)
else()
    set(NO_GBM OFF)
endif()

# =============================================================================
#  Platform-specific flags
# =============================================================================

# ── Pandora ───────────────────────────────────────────────────────────────────
if(PANDORA)
    add_definitions(-DPANDORA -DTEXSTREAM)
    add_compile_options(-mcpu=cortex-a8 -mfpu=neon -mfloat-abi=softfp
                        -ftree-vectorize -fsingle-precision-constant -ffast-math)
    set(NO_GBM ON)
endif()

# ── Pyra ──────────────────────────────────────────────────────────────────────
if(PYRA)
    add_definitions(-DPYRA)
    set(USE_CLOCK ON)
    add_compile_options(-mcpu=cortex-a15 -mfpu=neon-vfpv4 -mfloat-abi=hard
                        -ftree-vectorize -fsingle-precision-constant -ffast-math)
    set(NO_GBM ON)
endif()

# ── Raspberry Pi ──────────────────────────────────────────────────────────────
if(BCMHOST)
    include_directories(/opt/vc/include
                        /opt/vc/include/interface/vcos/pthreads
                        /opt/vc/include/interface/vmcs_host/linux)
    link_directories(/opt/vc/lib)
    add_definitions(-DBCMHOST)
endif()

# ── ODROID ────────────────────────────────────────────────────────────────────
if(ODROID)
    add_definitions(-DODROID)
endif()

# ── GOA_CLONE ─────────────────────────────────────────────────────────────────
if(GOA_CLONE)
    add_definitions(-DGOA_CLONE)
    if(NOT ${CMAKE_SYSTEM_PROCESSOR} MATCHES "aarch64")
        add_compile_options(-mfpu=neon-vfpv3 -mfloat-abi=hard)
    endif()
    add_compile_options(-mcpu=cortex-a35 -march=armv8-a+crc+simd+crypto
                        -ftree-vectorize -fsingle-precision-constant -ffast-math)
    set(NO_GBM OFF)
    set(NOX11 ON)
    set(EGL_WRAPPER ON)
    set(GLX_STUBS ON)
endif()

# ── Android ───────────────────────────────────────────────────────────────────
# Perubahan dari original:
# - Hapus -DDEFAULT_ES=2 hardcoded (sekarang dikontrol oleh blok DEFAULT_ES di atas)
# - Aktifkan GLES32_TARGET secara default untuk Android
# - Tambah -DANDROID_API=26 sebagai informasi untuk hardext
if(ANDROID)
    add_definitions(-DANDROID -DNOX11 -DNO_GBM)

    # API 26 = minimal untuk GLES 3.2 solid di semua perangkat
    add_definitions(-DANDROID_API=26)

    # Aktifkan GLES 3.2 target secara default di Android
    if(NOT DEFINED GLES32_TARGET)
        set(GLES32_TARGET ON)
    endif()

    set(NO_GBM ON)
    set(NOX11 ON)

    message(STATUS "Android build: DEFAULT_ES=${DEFAULT_ES}, GLES32_TARGET=${GLES32_TARGET}")
endif()

# ── Android Log ───────────────────────────────────────────────────────────────
if(USE_ANDROID_LOG)
    add_definitions(-DUSE_ANDROID_LOG)
    find_library(log-lib log)
endif()

# ── PocketCHIP ────────────────────────────────────────────────────────────────
if(CHIP)
    add_definitions(-DCHIP)
    add_compile_options(-mcpu=cortex-a8 -mfpu=neon -mfloat-abi=hard
                        -ftree-vectorize -fsingle-precision-constant -ffast-math)
endif()

# ── AmigaOS4 ──────────────────────────────────────────────────────────────────
if(AMIGAOS4)
    set(CMAKE_C_COMPILER   "ppc-amigaos-gcc")
    set(CMAKE_CXX_COMPILER "ppc-amigaos-g++")
    set(CMAKE_LINKER       "ppc-amigaos-ld")
    set(CMAKE_AR           "ppc-amigaos-ar")
    set(CMAKE_RANLIB       "ppc-amigaos-ranlib")
    add_definitions(-DAMIGAOS4)
    set(NOX11 ON)
    set(NOEGL ON)
    set(NO_GBM ON)
    set(CMAKE_C_FLAGS_DEBUG         "-O1 -gstabs")
    set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -gstabs")
endif()

# =============================================================================
#  Feature flags dari opsi
# =============================================================================

if(GLES32_TARGET)
    add_definitions(-DGLES32_TARGET)
    message(STATUS "GLES 3.2 target: ON")
endif()

if(STATICLIB)
    add_definitions(-DSTATICLIB)
endif()

if(NOX11)
    add_definitions(-DEGL_NO_X11 -DNOX11)
endif()

if(NOEGL)
    add_definitions(-DNOEGL -DNOX11)
endif()

if(NO_GBM)
    add_definitions(-DNO_GBM)
endif()

if(USE_CLOCK)
    add_definitions(-DUSE_CLOCK)
endif()

if(NO_LOADER)
    add_definitions(-DNO_LOADER)
endif()

if(NO_INIT_CONSTRUCTOR)
    add_definitions(-DNO_INIT_CONSTRUCTOR)
endif()

if(DEFAULT_GLES)
    add_definitions(-DDEFAULT_GLES="${DEFAULT_GLES}")
endif()

if(DEFAULT_EGL)
    add_definitions(-DDEFAULT_EGL="${DEFAULT_EGL}")
endif()

if(DEFAULT_FB)
    add_definitions(-DDEFAULT_FB="${DEFAULT_FB}")
endif()

if(GLX_STUBS)
    add_definitions(-DGLX_STUBS)
endif()

# =============================================================================
#  GBM — pkg-config
# =============================================================================
if(NOT NO_GBM)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(KMSDRM REQUIRED libdrm gbm egl)
    link_directories(${KMSDRM_LIBRARY_DIRS})
    include_directories(${KMSDRM_INCLUDE_DIRS})
endif()

# =============================================================================
#  Compiler flags — C standard dan warning suppression
# =============================================================================
if(NOT WIN32_MSVC)
    check_compiler_flag(C -std=gnu11 SUPPORTS_GNU11)
    check_compiler_flag(C -funwind-tables SUPPORTS_UNWIND_TABLES)

    if(SUPPORTS_GNU11)
        add_compile_options(-std=gnu11)
    else()
        check_compiler_flag(C -std=gnu99 SUPPORTS_GNU99)
        if(SUPPORTS_GNU99)
            add_compile_options(-std=gnu99)
        endif()
    endif()

    if(SUPPORTS_UNWIND_TABLES)
        add_compile_options(-funwind-tables)
    endif()

    if(NOT WIN32_PLATFORM)
        add_definitions(-fvisibility=hidden)
    else()
        set(CMAKE_SHARED_LINKER_FLAGS "-Wl,--exclude-all-symbols,--kill-at")
    endif()

elseif(CMAKE_C_COMPILER_ID MATCHES "Clang")
    add_definitions(-Wno-deprecated-declarations)
    add_definitions(-Wno-unused-function -Wno-unused-variable -Wno-dangling-else)
    add_definitions(-Wno-implicit-const-int-float-conversion)
    add_definitions(-Wno-visibility)
else()
    if(CMAKE_C_COMPILER_ID MATCHES "Intel")
        add_definitions(-wd1786 -wd589 -wd537 -wd118 -wd2722)
    else()
        add_definitions(-wd4996 -wd4244 -wd4267 -wd4098 -wd4018)
    endif()
    add_definitions("-Dinline=__inline" "-D__func__=__FUNCTION__")
endif()

if(CMAKE_CC_COMPILER_ID MATCHES "Clang" OR CMAKE_SYSTEM_NAME MATCHES "Emscripten")
    add_definitions(-Wno-pointer-sign -Wno-dangling-else)
endif()

# =============================================================================
#  LTO (Link-Time Optimization)
#
#  Dikendalikan via LTO_MODE cache variable: "none" | "thin" | "full"
#  NDK r28c (Clang LLVM r563880c) mendukung ThinLTO secara native.
# =============================================================================
if(NOT LTO_MODE STREQUAL "none")
    check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_OUTPUT LANGUAGES C)
    if(IPO_SUPPORTED)
        message(STATUS "LTO mode: ${LTO_MODE}")
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
        if(LTO_MODE STREQUAL "thin")
            # Clang ThinLTO — lebih cepat build, hampir sama efektifnya dengan full LTO
            add_compile_options(-flto=thin)
            add_link_options(-flto=thin)
        endif()
    else()
        message(STATUS "LTO tidak didukung compiler ini: ${IPO_OUTPUT}")
    endif()
endif()

# =============================================================================
#  CCache
# =============================================================================
if(USE_CCACHE)
    find_program(CCACHE_FOUND ccache)
    if(CCACHE_FOUND)
        set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE ccache)
        set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK    ccache)
        message(STATUS "CCache: ON (${CCACHE_FOUND})")
    endif()
endif()

# =============================================================================
#  Subdirektori & include
# =============================================================================
include_directories(include)
link_directories(${CMAKE_SOURCE_DIR}/lib)
add_subdirectory(src)

# =============================================================================
#  Tests (dipertahankan identik dengan original)
# =============================================================================
enable_testing()

macro(create_test test_name test_filename calls_count tolerance)
    if(${ARGC} EQUAL 5)
        add_test(${test_name}
            ${CMAKE_COMMAND}
            -D LIBRARY_FOLDER=${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
            -D TESTS_DIRECTORY=${CMAKE_SOURCE_DIR}/tests
            -D TEST_FILENAME=${test_filename}
            -D CALLS=${calls_count}
            -D TOLERANCE=${tolerance}
            -D EXTRACT_RANGE=${ARGV4}
            -P ${CMAKE_SOURCE_DIR}/test.cmake)
    elseif(${ARGC} EQUAL 6)
        if(${ARGV4} STREQUAL "NOEXTRACT_RANGE")
            add_test(${test_name}
                ${CMAKE_COMMAND}
                -D LIBRARY_FOLDER=${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
                -D TESTS_DIRECTORY=${CMAKE_SOURCE_DIR}/tests
                -D TEST_FILENAME=${test_filename}
                -D CALLS=${calls_count}
                -D TOLERANCE_GLES1=${tolerance}
                -D TOLERANCE_GLES2=${ARGV5}
                -P ${CMAKE_SOURCE_DIR}/test.cmake)
        else()
            add_test(${test_name}
                ${CMAKE_COMMAND}
                -D LIBRARY_FOLDER=${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
                -D TESTS_DIRECTORY=${CMAKE_SOURCE_DIR}/tests
                -D TEST_FILENAME=${test_filename}
                -D CALLS=${calls_count}
                -D TOLERANCE_GLES1=${tolerance}
                -D TOLERANCE_GLES2=${ARGV5}
                -D EXTRACT_RANGE=${ARGV4}
                -P ${CMAKE_SOURCE_DIR}/test.cmake)
        endif()
    else()
        add_test(${test_name}
            ${CMAKE_COMMAND}
            -D LIBRARY_FOLDER=${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
            -D TESTS_DIRECTORY=${CMAKE_SOURCE_DIR}/tests
            -D TEST_FILENAME=${test_filename}
            -D CALLS=${calls_count}
            -D TOLERANCE=${tolerance}
            -P ${CMAKE_SOURCE_DIR}/test.cmake)
    endif()
endmacro()

macro(create_test_GLES test_name GLES test_filename calls_count tolerance)
    if(${ARGC} EQUAL 5)
        add_test(${test_name}_GLES${GLES}
            ${CMAKE_COMMAND}
            -D LIBRARY_FOLDER=${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
            -D TESTS_DIRECTORY=${CMAKE_SOURCE_DIR}/tests
            -D TEST_FILENAME=${test_filename}
            -D CALLS=${calls_count}
            -D TOLERANCE=${tolerance}
            -D EXTRACT_RANGE=${ARGV4}
            -D GLES_FORCED=${GLES}
            -P ${CMAKE_SOURCE_DIR}/test.cmake)
    else()
        add_test(${test_name}_GLES${GLES}
            ${CMAKE_COMMAND}
            -D LIBRARY_FOLDER=${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
            -D TESTS_DIRECTORY=${CMAKE_SOURCE_DIR}/tests
            -D TEST_FILENAME=${test_filename}
            -D CALLS=${calls_count}
            -D TOLERANCE=${tolerance}
            -D GLES_FORCED=${GLES}
            -P ${CMAKE_SOURCE_DIR}/test.cmake)
    endif()
endmacro()

create_test(GLXgears        glxgears        "0000008203" 25 "NOEXTRACT_RANGE" 700)
create_test(StuntCarRacer   stuntcarracer   "0000118817" 20 "638x478+1+1")
create_test(Neverball        neverball       "0000078750" 20 "798x478+1+1" 200)
create_test(FoobillardPlus  foobillardplus  "0000014748" 50 "798x478+1+1" 60)
create_test(Descent3         descent3        "0000284192" 20 "638x478+1+1")
create_test(PointSprite      pointsprite     "0000248810" 20)
create_test_GLES(OpenRA       2 openra       "0000031249" 20 "638x478+1+1")
create_test_GLES(GLSL_lighting 2 glsl_lighting "0000505393" 20)
