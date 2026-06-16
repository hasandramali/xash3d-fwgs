cmake_minimum_required(VERSION 3.14)
project(xash3d C CXX OBJC OBJCXX)

if(NOT IOS)
    message(FATAL_ERROR "This CMakeLists.txt is designed for iOS only. Use waf for other platforms.")
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_OBJC_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

add_definitions(
    -DXASH_SDL=2 -DXASH_SDLMAIN=1 -DXASH_STATIC_GAMELIB=1
    -DXASH_GLES=1 -DXASH_GLES3COMPAT=1 -DXASH_MOBILE_PLATFORM=1 -DXASH_IOS=1
    -D_GNU_SOURCE -DHAVE_TGMATH_H -DHAVE_STRNLEN
    -DHAVE_STRLCPY -DHAVE_STRLCAT -D_XASH_SDLMAIN=1
    -DXASH_MBEDTLS=1 -DHAVE_STRCASECMP=1 -DHAVE_STRCASESTR=1
    -DHAVE_DIRENT_D_TYPE=1
    -DALLOCA_H=<alloca.h>
    -DXASH_ENABLE_MAIN=1
)

set(ENGINE_INCS
    ${CMAKE_SOURCE_DIR}/engine
    ${CMAKE_SOURCE_DIR}/engine/common
    ${CMAKE_SOURCE_DIR}/engine/common/imagelib
    ${CMAKE_SOURCE_DIR}/engine/common/soundlib
    ${CMAKE_SOURCE_DIR}/engine/server
    ${CMAKE_SOURCE_DIR}/engine/client
    ${CMAKE_SOURCE_DIR}/engine/client/vgui
    ${CMAKE_SOURCE_DIR}/engine/platform
    ${CMAKE_SOURCE_DIR}/engine/platform/ios
    ${CMAKE_SOURCE_DIR}/public
    ${CMAKE_SOURCE_DIR}/pm_shared
    ${CMAKE_SOURCE_DIR}/filesystem
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/ios/Xash3D-iOS/model
    ${CMAKE_SOURCE_DIR}/ios/Xash3D-iOS/controller
    ${CMAKE_SOURCE_DIR}/ios/Xash3D-iOS/view
)

# ---- iOS Frameworks ----
foreach(fw Foundation UIKit CoreGraphics QuartzCore GameController
         SystemConfiguration CFNetwork AVFoundation Metal OpenGLES
         CoreAudio AudioToolbox CoreMotion CoreBluetooth CoreHaptics Security)
    find_library(${fw}_LIB ${fw})
    if(NOT ${fw}_LIB)
        message(FATAL_ERROR "Required framework ${fw} not found")
    endif()
    list(APPEND IOS_FRAMEWORKS ${${fw}_LIB})
endforeach()

find_package(Threads REQUIRED)
find_library(Z_LIB z)

# ============================================================================
# Helper: create empty stub libraries for dependencies we don't build from source
# ============================================================================
function(stub_lib name)
    set(stub_file ${CMAKE_BINARY_DIR}/stubs/${name}.c)
    string(MAKE_C_IDENTIFIER "${name}" safe_name)
    file(WRITE ${stub_file} "/* empty stub */\nint xash_stub_${safe_name}_dummy;\n")
    add_library(${name} STATIC ${stub_file})
    set_target_properties(${name} PROPERTIES LINKER_LANGUAGE C OUTPUT_NAME ${name})
endfunction()

# ============================================================================
# 3rd-party libraries with CMake support
# ============================================================================

# mbedtls / TLS
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "Build static mbedtls")
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "Build shared mbedtls")
set(UNSAFE_BUILD ON CACHE BOOL "Allow unsafe mbedtls build")
add_subdirectory(3rdparty/mbedtls/mbedtls EXCLUDE_FROM_ALL)
if(TARGET mbedtls)
    add_library(ssl ALIAS mbedtls)
    set_target_properties(mbedtls PROPERTIES OUTPUT_NAME ssl)
endif()
if(TARGET mbedcrypto)
    add_library(crypto ALIAS mbedcrypto)
    set_target_properties(mbedcrypto PROPERTIES OUTPUT_NAME crypto)
elseif(TARGET tfpsacrypto)
    add_library(crypto ALIAS tfpsacrypto)
    set_target_properties(tfpsacrypto PROPERTIES OUTPUT_NAME crypto)
endif()

# OpenGLES framework (replaces gl4es which doesn't compile on iOS)
add_library(GL INTERFACE)
target_link_libraries(GL INTERFACE ${OpenGLES_LIB})

# opus audio codec
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "Build opus as static")
set(OPUS_BUILD_TESTING OFF CACHE BOOL "Disable opus tests")
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "Disable opus programs")
set(OPUS_CUSTOM_MODES ON CACHE BOOL "Enable custom opus modes")
add_subdirectory(3rdparty/opus/opus EXCLUDE_FROM_ALL)

# libogg
add_subdirectory(3rdparty/libogg/libogg EXCLUDE_FROM_ALL)

# opusfile audio playback (manually defined, its cmake calls find_package which fails in-tree)
add_library(opusfile STATIC
    3rdparty/opusfile/opusfile/src/info.c
    3rdparty/opusfile/opusfile/src/internal.c
    3rdparty/opusfile/opusfile/src/opusfile.c
    3rdparty/opusfile/opusfile/src/stream.c
)
target_include_directories(opusfile PUBLIC 3rdparty/opusfile/opusfile/include)
target_link_libraries(opusfile PUBLIC ogg opus)

# bzip2 compression
set(ENABLE_APP OFF CACHE BOOL "Disable bzip2 app")
set(ENABLE_TESTS OFF CACHE BOOL "Disable bzip2 tests")
set(ENABLE_STATIC_LIB ON CACHE BOOL "Build static bzip2")
set(ENABLE_SHARED_LIB OFF CACHE BOOL "Disable shared bzip2")
add_subdirectory(3rdparty/bzip2/bzip2 EXCLUDE_FROM_ALL)

# ============================================================================
# 3rd-party libraries without CMake (define manually)
# ============================================================================

# vorbis audio
file(GLOB VORBIS_SRC
    3rdparty/vorbis/vorbis-src/lib/mdct.c
    3rdparty/vorbis/vorbis-src/lib/smallft.c
    3rdparty/vorbis/vorbis-src/lib/block.c
    3rdparty/vorbis/vorbis-src/lib/envelope.c
    3rdparty/vorbis/vorbis-src/lib/window.c
    3rdparty/vorbis/vorbis-src/lib/lsp.c
    3rdparty/vorbis/vorbis-src/lib/lpc.c
    3rdparty/vorbis/vorbis-src/lib/analysis.c
    3rdparty/vorbis/vorbis-src/lib/synthesis.c
    3rdparty/vorbis/vorbis-src/lib/psy.c
    3rdparty/vorbis/vorbis-src/lib/info.c
    3rdparty/vorbis/vorbis-src/lib/floor1.c
    3rdparty/vorbis/vorbis-src/lib/floor0.c
    3rdparty/vorbis/vorbis-src/lib/res0.c
    3rdparty/vorbis/vorbis-src/lib/mapping0.c
    3rdparty/vorbis/vorbis-src/lib/registry.c
    3rdparty/vorbis/vorbis-src/lib/codebook.c
    3rdparty/vorbis/vorbis-src/lib/sharedbook.c
    3rdparty/vorbis/vorbis-src/lib/lookup.c
    3rdparty/vorbis/vorbis-src/lib/bitrate.c
)
add_library(vorbis STATIC ${VORBIS_SRC})
target_include_directories(vorbis PUBLIC 3rdparty/vorbis/vorbis-src/include)
target_link_libraries(vorbis PUBLIC ogg)

add_library(vorbisfile STATIC 3rdparty/vorbis/vorbis-src/lib/vorbisfile.c)
target_include_directories(vorbisfile PUBLIC 3rdparty/vorbis/vorbis-src/include)
target_link_libraries(vorbisfile PUBLIC vorbis)

set(XASH_IOS_ENGINE_LINK_LIBS
    backtrace_custom library_suffix
    MultiEmulator bz2_static opus opusfile ogg vorbisfile vorbis
    GL mbedtls mbedx509 crypto
    ${IOS_FRAMEWORKS} Threads::Threads ${Z_LIB}
)

# MultiEmulator (SHA/MD5 hashing)
file(GLOB ME_SRC 3rdparty/MultiEmulator/src/*.c)
add_library(MultiEmulator STATIC ${ME_SRC})
target_include_directories(MultiEmulator PUBLIC
    3rdparty/MultiEmulator/include
    3rdparty/MultiEmulator/src
)

# library_suffix (platform detection)
add_library(library_suffix STATIC 3rdparty/library_suffix/src/library_suffix.c)
target_include_directories(library_suffix PUBLIC 3rdparty/library_suffix/include)

# libbacktrace (crash handler)
file(GLOB BT_SRC
    3rdparty/libbacktrace/libbacktrace/atomic.c
    3rdparty/libbacktrace/libbacktrace/dwarf.c
    3rdparty/libbacktrace/libbacktrace/fileline.c
    3rdparty/libbacktrace/libbacktrace/posix.c
    3rdparty/libbacktrace/libbacktrace/print.c
    3rdparty/libbacktrace/libbacktrace/sort.c
    3rdparty/libbacktrace/libbacktrace/state.c
    3rdparty/libbacktrace/libbacktrace/backtrace.c
    3rdparty/libbacktrace/libbacktrace/simple.c
    3rdparty/libbacktrace/libbacktrace/mmapio.c
    3rdparty/libbacktrace/libbacktrace/mmap.c
    3rdparty/libbacktrace/libbacktrace/macho.c
)
add_library(backtrace_custom STATIC ${BT_SRC})
target_include_directories(backtrace_custom PUBLIC
    3rdparty/libbacktrace
    3rdparty/libbacktrace/libbacktrace
    ios/libbacktrace_config
)
target_compile_definitions(backtrace_custom PRIVATE HAVE_LIBBACKTRACE=1)

# ============================================================================
# Main engine library (includes all engine + public + filesystem + ref sources)
# ============================================================================
file(GLOB_RECURSE ENGINE_COMMON   engine/common/*.c)
file(GLOB_RECURSE ENGINE_IMAGELIB engine/common/imagelib/*.c)
file(GLOB_RECURSE ENGINE_SOUNDLIB engine/common/soundlib/*.c)
file(GLOB_RECURSE ENGINE_HTTP     engine/common/http/*.c)
file(GLOB_RECURSE ENGINE_SERVER   engine/server/*.c)
file(GLOB_RECURSE ENGINE_CLIENT   engine/client/*.c)
file(GLOB ENGINE_POSIX    engine/platform/posix/*.c)
file(GLOB ENGINE_IOS_C    engine/platform/ios/*.c)
file(GLOB ENGINE_IOS_M    engine/platform/ios/*.m)
file(GLOB LAUNCHER_IOS_MM
    ios/Xash3D-iOS/controller/*.mm
    ios/Xash3D-iOS/model/*.mm
    ios/Xash3D-iOS/view/*.mm
)
file(GLOB PUBLIC_SRC      public/*.c)

# Find and link SDL framework on Apple platforms
find_library(SDL2_FRAMEWORK SDL2 PATHS /Library/Frameworks)
find_library(SDL3_FRAMEWORK SDL3 PATHS /Library/Frameworks)
if(SDL3_FRAMEWORK)
    message(STATUS "Found SDL3 framework at: ${SDL3_FRAMEWORK}")
    add_definitions(-DXASH_SDL=3)
    file(GLOB ENGINE_SDL engine/platform/sdl3/*.c)
    get_filename_component(SDL3_PARENT ${SDL3_FRAMEWORK} DIRECTORY)
    set(SDL_INCS "${SDL3_FRAMEWORK}/Headers" ${SDL3_PARENT})
    set(SDL_LIBS ${SDL3_FRAMEWORK})
elseif(SDL2_FRAMEWORK)
    message(STATUS "Found SDL2 framework at: ${SDL2_FRAMEWORK}")
    add_definitions(-DXASH_SDL=2)
    file(GLOB ENGINE_SDL engine/platform/sdl2/*.c)
    get_filename_component(SDL2_PARENT ${SDL2_FRAMEWORK} DIRECTORY)
    set(SDL_INCS "${SDL2_FRAMEWORK}/Headers" ${SDL2_PARENT})
    set(SDL_LIBS ${SDL2_FRAMEWORK})
else()
    message(FATAL_ERROR "SDL2 or SDL3 framework not found at /Library/Frameworks")
endif()

# Exclude old launchdialog.m if using new launcher
list(REMOVE_ITEM ENGINE_IOS_M ${CMAKE_SOURCE_DIR}/engine/platform/ios/launchdialog.m)

# Exclude dedicated.c from common if not building dedicated
list(REMOVE_ITEM ENGINE_COMMON ${CMAKE_SOURCE_DIR}/engine/common/dedicated.c)

# On iOS, we need SDL_uikit_main.c to provide the system main()
# launcher.c provides SDL_main() because XASH_SDLMAIN=1 is defined.

file(GLOB FILESYSTEM_SRC  filesystem/*.c filesystem/*.cpp)
file(GLOB REF_COMMON_SRC  ref/common/*.c)
file(GLOB REF_GL_SRC      ref/gl/*.c ref/gl/gl2_shim/*.c)
file(GLOB REF_SOFT_SRC    ref/soft/*.c)

# Exclude build_vcs.c from public (built separately)
list(REMOVE_ITEM PUBLIC_SRC ${CMAKE_SOURCE_DIR}/public/build_vcs.c)

# Generate VCS build info for build_vcs.c
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
execute_process(
    COMMAND git rev-parse --abbrev-ref HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_BRANCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
execute_process(
    COMMAND git log -1 --format=%ct
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT_DATE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT GIT_COMMIT)
    set(GIT_COMMIT "unknown")
endif()
if(NOT GIT_BRANCH)
    set(GIT_BRANCH "unknown")
endif()
if(NOT GIT_COMMIT_DATE)
    set(GIT_COMMIT_DATE "0")
endif()
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/public/build_vcs.c
    PROPERTIES COMPILE_DEFINITIONS
    "XASH_BUILD_COMMIT=\"${GIT_COMMIT}\";XASH_BUILD_BRANCH=\"${GIT_BRANCH}\";XASH_BUILD_COMMIT_DATE=\"${GIT_COMMIT_DATE}\""
)

# Exclude SDL_uikit_main.c (Xcode app provides its own main())
# list(REMOVE_ITEM ENGINE_IOS_C ${CMAKE_SOURCE_DIR}/engine/platform/ios/SDL_uikit_main.c)

# launchdialog.m uses manual retain/release; disable ARC for this file
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/engine/platform/ios/launchdialog.m
    PROPERTIES COMPILE_FLAGS "-fno-objc-arc"
)

# Define include directories for the renderer components (excluding client/server)
set(REF_INCS
    ${CMAKE_SOURCE_DIR}/ref/common
    ${CMAKE_SOURCE_DIR}/engine
    ${CMAKE_SOURCE_DIR}/engine/common
    ${CMAKE_SOURCE_DIR}/engine/common/imagelib
    ${CMAKE_SOURCE_DIR}/engine/common/soundlib
    ${CMAKE_SOURCE_DIR}/engine/platform
    ${CMAKE_SOURCE_DIR}/engine/platform/ios
    ${CMAKE_SOURCE_DIR}/public
    ${CMAKE_SOURCE_DIR}/pm_shared
    ${CMAKE_SOURCE_DIR}/filesystem
    ${CMAKE_SOURCE_DIR}/common
)

# Compile renderer components as OBJECT libraries to isolate their include directories
add_library(ref_common OBJECT ${REF_COMMON_SRC})
target_include_directories(ref_common PRIVATE ${REF_INCS})
target_compile_definitions(ref_common PRIVATE REF_DLL=1 XASH_GLES3COMPAT=1)
target_link_libraries(ref_common PRIVATE
    ${XASH_IOS_ENGINE_LINK_LIBS}
)

add_library(ref_gl OBJECT ${REF_GL_SRC})
target_include_directories(ref_gl PRIVATE ${CMAKE_SOURCE_DIR}/ref/gl ${REF_INCS})
target_compile_definitions(ref_gl PRIVATE REF_DLL=1 XASH_GLES3COMPAT=1)
target_link_libraries(ref_gl PRIVATE
    ${XASH_IOS_ENGINE_LINK_LIBS}
)

add_library(ref_soft OBJECT ${REF_SOFT_SRC})
target_include_directories(ref_soft PRIVATE ${CMAKE_SOURCE_DIR}/ref/soft ${REF_INCS})
target_compile_definitions(ref_soft PRIVATE REF_DLL=1 XASH_GLES3COMPAT=1)
target_link_libraries(ref_soft PRIVATE
    ${XASH_IOS_ENGINE_LINK_LIBS}
)

add_library(xash STATIC
    ${ENGINE_COMMON} ${ENGINE_IMAGELIB} ${ENGINE_SOUNDLIB}
    ${ENGINE_HTTP} ${ENGINE_SERVER} ${ENGINE_CLIENT}
    ${ENGINE_POSIX} ${ENGINE_IOS_C} ${ENGINE_IOS_M} ${ENGINE_SDL}
    ${LAUNCHER_IOS_MM}
    ${PUBLIC_SRC} public/build_vcs.c
    ${FILESYSTEM_SRC}
    $<TARGET_OBJECTS:ref_common>
    $<TARGET_OBJECTS:ref_gl>
    $<TARGET_OBJECTS:ref_soft>
    ref/null/r_context.c
)
target_include_directories(xash PUBLIC ${ENGINE_INCS} ${SDL_INCS})
target_include_directories(xash PRIVATE public filesystem ref/common ref/gl ref/soft)

target_link_libraries(xash PUBLIC ${SDL_LIBS})

target_compile_definitions(xash PRIVATE
    ENGINE_DLL=1
    XASH_GLES3COMPAT=1
)
set_source_files_properties(
    ref/null/r_context.c
    PROPERTIES COMPILE_DEFINITIONS REF_DLL=1
)

target_link_libraries(xash PUBLIC
    ${XASH_IOS_ENGINE_LINK_LIBS}
)

# ============================================================================
# Stub libraries (empty, to satisfy Xcode project references)
# These libraries don't exist in FWGS but the Xcode project expects them.
# They are loaded at runtime by the engine, not linked at compile time.
# ============================================================================
foreach(stub
    tier0 tier1 vstdlib vinterface server client mathlib
    vgui2 vgui2_support vgui_controls vgui_surfacelib vguimatsurface
    imgui imgui_ext dead_wrapper luash lua cqmiao cso_controls GameUI
    freetype mpg123 png16
    LinearMath Bullet2FileLoader Bullet3Collision Bullet3Common
    Bullet3Dynamics Bullet3Geometry Bullet3OpenCL_clew
    BulletCollision BulletDynamics BulletInverseDynamics BulletSoftBody
    Boost_atomic Boost_chrono Boost_container Boost_date_time
    Boost_exception Boost_filesystem Boost_graph
    Boost_iostreams Boost_locale Boost_log Boost_math
    Boost_math_c99 Boost_math_c99f Boost_math_c99l
    Boost_math_tr1 Boost_math_tr1f Boost_math_tr1l
    Boost_prg_exec_monitor Boost_program_options Boost_random
    Boost_regex Boost_serialization Boost_signals Boost_system
    Boost_test_exec_monitor Boost_thread Boost_timer Boost_type_erasure
    Boost_unit_test_framework Boost_wave Boost_wserialization
    astcenc-native-static)
    stub_lib(${stub})
endforeach()
