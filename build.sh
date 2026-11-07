#!/usr/bin/env bash
#
# Builds tmp_time_overlay.dll for Windows x64 with Zig as a portable C++ toolchain.
#
#   ./build.sh          build the plugin DLL
#   ./build.sh --test   build the plugin DLL, then build and run the smoke test
#
# Zig is expected at tools/zig-x86_64-windows-<version>/zig.exe. Get it from
# https://ziglang.org/download/ and unpack it there, or set ZIG to the executable.

set -euo pipefail

cd "$(dirname "$0")"

ROOT="$(pwd)"
OUT_DIR="${ROOT}/build"
SDK_INCLUDE="${ROOT}/external/GameClientSDK/include"
IMGUI_INCLUDE="${ROOT}/external/imgui"

if [[ -z "${ZIG:-}" ]]; then
    ZIG="$(find "${ROOT}/tools" -maxdepth 2 -name 'zig.exe' -print -quit 2>/dev/null || true)"
fi
if [[ -z "${ZIG:-}" || ! -x "${ZIG}" ]]; then
    echo "error: zig not found. Unpack it under tools/ or set ZIG=/path/to/zig" >&2
    exit 1
fi
echo "using zig: ${ZIG}"

if [[ ! -f "${SDK_INCLUDE}/TruckersMP/TruckersMP.hxx" ]]; then
    echo "error: TruckersMP Client SDK missing at ${SDK_INCLUDE}" >&2
    echo "       git clone https://github.com/TruckersMP/GameClientSDK.git" >&2
    echo "       then copy its include/ folder to external/GameClientSDK/include" >&2
    exit 1
fi

mkdir -p "${OUT_DIR}"

# Dear ImGui translation units.
IMGUI_SOURCES=(
    "${IMGUI_INCLUDE}/imgui.cpp"
    "${IMGUI_INCLUDE}/imgui_draw.cpp"
    "${IMGUI_INCLUDE}/imgui_tables.cpp"
    "${IMGUI_INCLUDE}/imgui_widgets.cpp"
    "${IMGUI_INCLUDE}/backends/imgui_impl_dx11.cpp"
)

COMMON_FLAGS=(
    -target x86_64-windows-gnu
    -O2
    -std=c++17
    -DUNICODE -D_UNICODE
    "-I${IMGUI_INCLUDE}"
    "-I${IMGUI_INCLUDE}/backends"
    "-I${SDK_INCLUDE}"
)

# d3dcompiler has no import library in Zig's mingw sysroot, so build/d3dcompiler.def
# is turned into build/libd3dcompiler.a by tools/make-d3dcompiler-lib.sh.
if [[ ! -f "${OUT_DIR}/libd3dcompiler.a" ]]; then
    "${ROOT}/tools/make-d3dcompiler-lib.sh"
fi

echo "--- building ${OUT_DIR}/tmp_time_overlay.dll"
"${ZIG}" c++ "${COMMON_FLAGS[@]}" -DNDEBUG -shared \
    "${ROOT}/src/tmp_time_overlay.cpp" \
    "${IMGUI_SOURCES[@]}" \
    -o "${OUT_DIR}/tmp_time_overlay.dll" \
    "-L${OUT_DIR}" -ld3dcompiler -luser32 -lgdi32

echo "--- built ${OUT_DIR}/tmp_time_overlay.dll"

if [[ "${1:-}" == "--test" ]]; then
    echo "--- building ${OUT_DIR}/plugin_smoke.exe"
    "${ZIG}" c++ "${COMMON_FLAGS[@]}" \
        "${ROOT}/tests/plugin_smoke.cpp" \
        "${ROOT}/src/tmp_time_overlay.cpp" \
        "${IMGUI_SOURCES[@]}" \
        -o "${OUT_DIR}/plugin_smoke.exe" \
        "-L${OUT_DIR}" -ld3dcompiler -luser32 -lgdi32
    echo "--- running smoke test"
    ( cd "${OUT_DIR}" && ./plugin_smoke.exe )
fi
