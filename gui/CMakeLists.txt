# gui: win-ldac.exe — Dear ImGui + DirectX 11 front-end for the
# core_engine library. Single-exe model (see STATUS.md M7 Phase B):
# WinMain spawns a worker thread that runs the engine; the main thread
# drives the GUI at ~60 Hz and reads engine_get_status_snapshot().
#
# Only built when BTstack is available (the engine library transitively
# depends on it).

if(NOT HAVE_BTSTACK)
    message(STATUS "gui/: skipped (requires BTstack)")
    return()
endif()

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
set(IMPLOT_DIR ${CMAKE_SOURCE_DIR}/third_party/implot)

if(NOT EXISTS ${IMGUI_DIR}/imgui.h)
    message(STATUS "gui/: skipped (third_party/imgui not present)")
    return()
endif()
if(NOT EXISTS ${IMPLOT_DIR}/implot.h)
    message(STATUS "gui/: skipped (third_party/implot not present)")
    return()
endif()

# ── Dear ImGui (MIT) ───────────────────────────────────────────
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)
target_link_libraries(imgui PUBLIC d3d11 d3dcompiler dxgi)
if(MSVC)
    target_compile_options(imgui PRIVATE /wd4267 /wd4244)
endif()

# ── ImPlot (MIT) ───────────────────────────────────────────────
add_library(implot STATIC
    ${IMPLOT_DIR}/implot.cpp
    ${IMPLOT_DIR}/implot_items.cpp
    ${IMPLOT_DIR}/implot_demo.cpp
)
target_include_directories(implot PUBLIC ${IMPLOT_DIR})
target_link_libraries(implot PUBLIC imgui)
if(MSVC)
    target_compile_options(implot PRIVATE /wd4267 /wd4244)
endif()

# ── win-ldac.exe ───────────────────────────────────────────────
# Embed the tray + window icon as a Win32 resource. The .rc.in template
# gets the absolute path to assets/win-ldac.ico substituted at configure
# time so the build is independent of the current working directory at
# RC compile time. If the .ico is missing (fresh checkout, user hasn't
# placed assets yet) we still configure the .rc with a stale path — RC
# will fail loudly so the user knows what to add.
set(WIN_LDAC_ICO_PATH "${CMAKE_SOURCE_DIR}/assets/win-ldac.ico")
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/win-ldac.rc.in
    ${CMAKE_CURRENT_BINARY_DIR}/win-ldac.rc
    @ONLY)

add_executable(win-ldac WIN32
    src/main.cpp
    src/ui/status_window.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/win-ldac.rc
)
target_include_directories(win-ldac PRIVATE src)
target_link_libraries(win-ldac PRIVATE
    core_engine
    config_file
    imgui
    implot
    windowscodecs   # WIC, for monkey.png loading
)
set_target_properties(win-ldac PROPERTIES OUTPUT_NAME "win-ldac")
