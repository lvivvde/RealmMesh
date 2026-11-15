#!/usr/bin/env bash

# 供脚本定位 mongod / mongosh(ADR-0011)。查找顺序:
#   1. REALMMESH_MONGOD_BINARY / REALMMESH_MONGOSH_BINARY 显式覆盖;
#   2. PATH —— 系统已装的 mongod / mongosh(macOS 即 Homebrew 的 mongodb-community);
#   3. ./scripts/install-mongodb.sh 装到 .tools/ 的 Linux 固定版本(Linux 开发机与 CI 用)。
# 与 tests/cpp/support 下 MongodProcess 夹具的查找顺序一致。
#
# 约定：调用方先设置 realmmesh_mongodb_root(仓库根)。找不到时输出为空并返回 1。

realmmesh_mongodb_tool() {
    local realmmesh_override="$1" realmmesh_tool_dir="$2" realmmesh_name="$3"
    if [[ -n "${realmmesh_override}" ]]; then
        printf '%s\n' "${realmmesh_override}"
        return 0
    fi
    if command -v "${realmmesh_name}" >/dev/null 2>&1; then
        command -v "${realmmesh_name}"
        return 0
    fi
    local realmmesh_in_tree="${realmmesh_mongodb_root}/.tools/${realmmesh_tool_dir}/bin/${realmmesh_name}"
    if [[ -x "${realmmesh_in_tree}" ]]; then
        printf '%s\n' "${realmmesh_in_tree}"
        return 0
    fi
    return 1
}

realmmesh_mongod_binary() {
    realmmesh_mongodb_tool "${REALMMESH_MONGOD_BINARY:-}" mongodb-8.0.32 mongod
}

realmmesh_mongosh_binary() {
    realmmesh_mongodb_tool "${REALMMESH_MONGOSH_BINARY:-}" mongosh-2.12.0 mongosh
}

realmmesh_mongodb_install_hint() {
    printf 'MongoDB is required: on Linux run ./scripts/install-mongodb.sh; on macOS run "brew tap mongodb/brew && brew trust mongodb/brew && brew install mongodb-community mongosh"\n'
}
