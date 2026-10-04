#!/usr/bin/env bash

# 构建目录信息的读取端(#122)。cmake/RealmMeshBuildDirInfo.cmake 在配置期写出
# build/.build-dirs/<preset>.txt 与 <binaryDir>/realmmesh-build-dir.txt;这里只读
# 这两份结果，不解析 CMakePresets.json —— 预设继承、宏展开与用户预设的规则只有
# CMake 自己算得对。
#
# 只依赖 POSIX 工具与 bash 3.2 语法(macOS 自带 bash)。

realmmesh_default_preset="dev"

realmmesh_is_valid_preset_name() {
    # 与 cmake/RealmMeshBuildDirInfo.cmake 的 REALMMESH_PRESET_NAME_REGEX 一致。
    [[ "$1" =~ ^[A-Za-z0-9][A-Za-z0-9_.+-]*$ ]]
}

realmmesh_require_preset_name() {
    if ! realmmesh_is_valid_preset_name "$1"; then
        printf 'Invalid preset name: "%s" (use letters, digits and _ . + -, starting with a letter or digit).\n' \
            "$1" >&2
        return 1
    fi
}

realmmesh_configure_hint() {
    printf 'Configure it first: ./scripts/build.sh --preset %s (or cmake --preset %s).\n' \
        "$1" "$1" >&2
}

# 读键值文本中某个键的值(取首个匹配)。
realmmesh_build_dir_info_value() {
    sed -n "s/^$2=//p" "$1" | head -n 1
}

realmmesh_physical_dir() {
    (cd "$1" 2>/dev/null && pwd -P)
}

# 输出 <root> 下预设 <preset> 已配置的构建目录。登记缺失、目录未配置、身份
# 不符或来自另一棵源码树时返回非零，并在 stderr 说明如何配置。
realmmesh_resolve_build_dir() {
    local realmmesh_root_dir="$1"
    local realmmesh_preset="$2"
    realmmesh_require_preset_name "${realmmesh_preset}" || return 1

    local realmmesh_registry="${realmmesh_root_dir}/build/.build-dirs/${realmmesh_preset}.txt"
    if [[ ! -f "${realmmesh_registry}" ]]; then
        printf 'Preset "%s" has no recorded build directory (%s is missing).\n' \
            "${realmmesh_preset}" "${realmmesh_registry}" >&2
        printf 'Only presets inheriting realmmesh-base (as dev does) record one at configure time.\n' >&2
        realmmesh_configure_hint "${realmmesh_preset}"
        return 1
    fi

    local realmmesh_binary_dir
    realmmesh_binary_dir="$(realmmesh_build_dir_info_value \
        "${realmmesh_registry}" binary_dir)"
    if [[ -z "${realmmesh_binary_dir}" ||
        ! -f "${realmmesh_binary_dir}/CMakeCache.txt" ]]; then
        printf 'The build directory recorded for preset "%s" is not configured: %s\n' \
            "${realmmesh_preset}" "${realmmesh_binary_dir}" >&2
        realmmesh_configure_hint "${realmmesh_preset}"
        return 1
    fi

    local realmmesh_identity="${realmmesh_binary_dir}/realmmesh-build-dir.txt"
    local realmmesh_identity_preset=""
    if [[ -f "${realmmesh_identity}" ]]; then
        realmmesh_identity_preset="$(realmmesh_build_dir_info_value \
            "${realmmesh_identity}" preset)"
    fi
    if [[ "${realmmesh_identity_preset}" != "${realmmesh_preset}" ]]; then
        printf 'Build directory %s does not belong to preset "%s" (its identity names "%s").\n' \
            "${realmmesh_binary_dir}" "${realmmesh_preset}" \
            "${realmmesh_identity_preset:-none}" >&2
        realmmesh_configure_hint "${realmmesh_preset}"
        return 1
    fi

    # 物理路径比较:macOS 的 /var 是 /private/var 的符号链接。
    local realmmesh_identity_source realmmesh_identity_binary
    realmmesh_identity_source="$(realmmesh_build_dir_info_value \
        "${realmmesh_identity}" source_dir)"
    realmmesh_identity_binary="$(realmmesh_build_dir_info_value \
        "${realmmesh_identity}" binary_dir)"
    if [[ "$(realmmesh_physical_dir "${realmmesh_identity_source}")" != \
        "$(realmmesh_physical_dir "${realmmesh_root_dir}")" ]]; then
        printf 'Build directory %s was configured from another source tree: %s\n' \
            "${realmmesh_binary_dir}" "${realmmesh_identity_source}" >&2
        realmmesh_configure_hint "${realmmesh_preset}"
        return 1
    fi
    if [[ "$(realmmesh_physical_dir "${realmmesh_identity_binary}")" != \
        "$(realmmesh_physical_dir "${realmmesh_binary_dir}")" ]]; then
        printf 'Build directory %s carries the identity of %s.\n' \
            "${realmmesh_binary_dir}" "${realmmesh_identity_binary}" >&2
        realmmesh_configure_hint "${realmmesh_preset}"
        return 1
    fi

    printf '%s\n' "${realmmesh_binary_dir}"
}

# 输出 <root> 下预设 <preset> 构建出的 bin/<name>;构建目录不可用或可执行文件
# 缺失时返回非零并提示构建命令。
realmmesh_resolve_built_binary() {
    local realmmesh_build_dir
    realmmesh_build_dir="$(realmmesh_resolve_build_dir "$1" "$2")" || return 1
    local realmmesh_binary="${realmmesh_build_dir}/bin/$3"
    if [[ ! -x "${realmmesh_binary}" ]]; then
        printf 'Service binary is missing: %s\nRun ./scripts/build.sh --preset %s first.\n' \
            "${realmmesh_binary}" "$2" >&2
        return 1
    fi
    printf '%s\n' "${realmmesh_binary}"
}

# 输出本次使用的 cmake:系统 PATH 优先,.tools/cmake 兜底;ctest 取同一目录下的
# 那份(同一分发)。都没有时返回非零并说明。
realmmesh_find_cmake() {
    if command -v cmake >/dev/null 2>&1; then
        command -v cmake
    elif [[ -x "$1/.tools/cmake/bin/cmake" ]]; then
        printf '%s\n' "$1/.tools/cmake/bin/cmake"
    else
        echo "CMake 3.20 or newer is required." >&2
        echo "Install CMake or place a local distribution in .tools/cmake." >&2
        return 1
    fi
}

# 根目录的 compile_commands.json 是指向所选预设编译数据库的符号链接;切换
# 预设时跟着改指向。用户自己放的普通文件不动。
# 用法:realmmesh_link_compile_commands <root> <构建目录> <preset>
realmmesh_link_compile_commands() {
    local realmmesh_root_dir="$1"
    local realmmesh_database="$2/compile_commands.json"
    local realmmesh_link="${realmmesh_root_dir}/compile_commands.json"
    [[ -f "${realmmesh_database}" ]] || return 0

    local realmmesh_target="${realmmesh_database}"
    if [[ "${realmmesh_database}" == "${realmmesh_root_dir}/"* ]]; then
        realmmesh_target="${realmmesh_database#"${realmmesh_root_dir}/"}"
    fi
    if [[ -L "${realmmesh_link}" || ! -e "${realmmesh_link}" ]]; then
        if [[ "$(readlink "${realmmesh_link}" 2>/dev/null || true)" != \
            "${realmmesh_target}" ]]; then
            ln -sfn "${realmmesh_target}" "${realmmesh_link}"
        fi
    else
        echo "warning: ${realmmesh_link} is not a symlink; leaving it unchanged" \
            "(the $3 database is ${realmmesh_database})." >&2
    fi
}
