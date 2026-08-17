#!/usr/bin/env bash

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if command -v cmake >/dev/null 2>&1; then
    cmake_bin="$(command -v cmake)"
elif [[ -x "${project_root}/.tools/cmake/bin/cmake" ]]; then
    cmake_bin="${project_root}/.tools/cmake/bin/cmake"
else
    echo "CMake 3.20 or newer is required." >&2
    echo "Install CMake or place a local distribution in .tools/cmake." >&2
    exit 1
fi

ctest_bin="$(dirname "${cmake_bin}")/ctest"

cd "${project_root}"
"${cmake_bin}" --preset dev

compile_commands="${project_root}/build/dev/compile_commands.json"
compile_commands_link="${project_root}/compile_commands.json"
if [[ -f "${compile_commands}" && ! -e "${compile_commands_link}" && ! -L "${compile_commands_link}" ]]; then
    ln -s "build/dev/compile_commands.json" "${compile_commands_link}"
fi

"${cmake_bin}" --build --preset dev --parallel
"${ctest_bin}" --preset dev
