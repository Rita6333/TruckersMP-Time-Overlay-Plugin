cmake_minimum_required(VERSION 3.24)
project(tmp_time_overlay LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "tmp_time_overlay supports Windows only")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/imgui")
set(MINHOOK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/minhook")

add_library(tmp_time_overlay SHARED
    src/tmp_time_overlay.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${MINHOOK_DIR}/src/buffer.c
    ${MINHOOK_DIR}/src/hook.c
    ${MINHOOK_DIR}/src/trampoline.c
    ${MINHOOK_DIR}/src/hde/hde64.c
)
target_include_directories(tmp_time_overlay PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${MINHOOK_DIR}/include
)
target_compile_definitions(tmp_time_overlay PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(tmp_time_overlay PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:/W4>
    $<$<COMPILE_LANGUAGE:CXX>:/utf-8>
    $<$<COMPILE_LANGUAGE:C>:/W3>
)
target_link_libraries(tmp_time_overlay PRIVATE d3d11 dxgi d3dcompiler dwmapi user32 gdi32)
set_property(TARGET tmp_time_overlay PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(tmp_time_overlay_smoke tests/smoke_test.cpp)
target_compile_definitions(tmp_time_overlay_smoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_link_libraries(tmp_time_overlay_smoke PRIVATE d3d11 dxgi user32)
add_dependencies(tmp_time_overlay_smoke tmp_time_overlay)

enable_testing()
add_test(NAME dx11_overlay_smoke COMMAND tmp_time_overlay_smoke)
set_tests_properties(dx11_overlay_smoke PROPERTIES
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:tmp_time_overlay>"
)
