cmake_minimum_required(VERSION 3.16)
project(win-ldac
        LANGUAGES C CXX
        VERSION 0.1.0
        DESCRIPTION "Windows LDAC bluetooth A2DP source")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Force UTF-8 on MSVC so AOSP source (which is ASCII anyway) and our literals behave.
if(MSVC)
    add_compile_options(/utf-8 /MP)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

# ─────────────────────────────────────────────────────────────
#  libldac (Sony, Apache 2.0). We do NOT use EHfive/ldacBT's
#  CMakeLists because it does `target_link_libraries(... m)`
#  which is Linux-only. We compile the .c files directly here.
#  Only two translation units thanks to AOSP's unity-build style:
#  ldaclib.c #includes the rest of src/*.c.
# ─────────────────────────────────────────────────────────────
set(LDAC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/ldacBT/libldac)

if(NOT EXISTS ${LDAC_DIR}/inc/ldacBT.h)
    message(FATAL_ERROR
        "libldac sources not found. Run:\n"
        "  git submodule update --init --recursive")
endif()

add_library(ldac_enc STATIC
    ${LDAC_DIR}/src/ldaclib.c
    ${LDAC_DIR}/src/ldacBT.c
)
target_include_directories(ldac_enc PUBLIC
    ${LDAC_DIR}/inc
)
# Windows porting shims:
#   - <unistd.h> stub for ldacBT_internal.h
#   - __attribute__((unused)) → empty
if(MSVC)
    target_include_directories(ldac_enc PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/win_compat
    )
    target_compile_options(ldac_enc PRIVATE
        /FI${CMAKE_CURRENT_SOURCE_DIR}/cmake/win_compat/msvc_compat.h
        # AOSP C code triggers harmless MSVC warnings; quiet them.
        /wd4244  # conversion, possible loss of data
        /wd4267  # size_t→int
        /wd4146  # unary minus on unsigned
        /wd4018  # signed/unsigned mismatch
        /wd4101  # unreferenced local
        /wd4133  # incompatible types
        /wd4005  # macro redefinition
    )
endif()

add_library(ldac_abr STATIC
    ${LDAC_DIR}/abr/src/ldacBT_abr.c
)
target_include_directories(ldac_abr PUBLIC
    ${LDAC_DIR}/abr/inc
)
target_link_libraries(ldac_abr PUBLIC ldac_enc)
if(MSVC)
    target_compile_options(ldac_abr PRIVATE /wd4244 /wd4267)
endif()

# ─────────────────────────────────────────────────────────────
#  BTstack (user-fetched, not a submodule). Only built if the
#  user has cloned vendor/btstack/. See HANDOFF.md §4.2.
# ─────────────────────────────────────────────────────────────
option(WITH_BTSTACK "Build BTstack-dependent targets if vendor/btstack exists" ON)
set(HAVE_BTSTACK FALSE)
if(WITH_BTSTACK AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/vendor/btstack/src/btstack.h)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/btstack.cmake)
    set(HAVE_BTSTACK TRUE)
    message(STATUS "BTstack found, building Bluetooth targets")
else()
    message(STATUS "BTstack NOT found at vendor/btstack/, skipping Bluetooth targets")
    message(STATUS "  → To enable: git clone https://github.com/bluekitchen/btstack.git vendor/btstack")
endif()

# ─────────────────────────────────────────────────────────────
#  core: the headless engine + CLI tools.
# ─────────────────────────────────────────────────────────────
add_subdirectory(core)

# ─────────────────────────────────────────────────────────────
#  gui: win-ldac.exe — Dear ImGui + DX11 desktop front-end.
#  Built only if BTstack and third_party/imgui+implot are present.
# ─────────────────────────────────────────────────────────────
add_subdirectory(gui)
