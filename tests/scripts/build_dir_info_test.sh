#!/usr/bin/env bash

# 构建目录信息(#122)的行为测试:在临时小工程里用真实 CMakePresets.json、
# cmake/RealmMeshBuildDirInfo.cmake 与 scripts/ 入口跑 configure / build / test,
# 验证各入口按 --preset 解析的是配置期记录的真实 binaryDir,未配置、身份不符
# 或源码树不符时给出明确提示,并且派生用户预设走得通完整链路。
#
# 用法:build_dir_info_test.sh <源码根> <cmake 可执行文件>

set -euo pipefail

source_root="$1"
cmake_bin="$2"

# build.sh / test-watch.sh 从 PATH 找 cmake,固定为与本次构建同一份。
PATH="$(dirname "${cmake_bin}"):${PATH}"
export PATH

work_root="$(mktemp -d)"
fixture="$(cd "${work_root}" && pwd -P)/fixture"
cleanup() {
    rm -rf -- "${work_root}"
}
trap cleanup EXIT

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

expect_contains() {
    local haystack="$1"
    local needle="$2"
    local context="$3"
    # CMake 按宽度折行警告文本，断点随路径长短变化:比较前把空白压成单个空格。
    local flat
    flat="$(printf '%s' "${haystack}" | tr -s '[:space:]' ' ')"
    if [[ "${haystack}" != *"${needle}"* && "${flat}" != *"${needle}"* ]]; then
        printf '%s\n' "${haystack}" >&2
        fail "${context}: expected output to contain '${needle}'"
    fi
}

# 运行命令并要求非零退出;合并后的输出写进 last_output。
expect_failure() {
    local context="$1"
    shift
    if last_output="$("$@" 2>&1)"; then
        printf '%s\n' "${last_output}" >&2
        fail "${context}: expected a non-zero exit"
    fi
}

expect_success() {
    local context="$1"
    shift
    if ! last_output="$("$@" 2>&1)"; then
        printf '%s\n' "${last_output}" >&2
        fail "${context}: expected success"
    fi
}

mkdir -p "${fixture}/cmake" "${fixture}/scripts/lib"
cp "${source_root}/CMakePresets.json" "${fixture}/"
cp "${source_root}/cmake/RealmMeshBuildDirInfo.cmake" "${fixture}/cmake/"
for script in build.sh build-dir.sh test-watch.sh dev-services.sh \
    dev-all-in-one.sh lib/build-dir.sh lib/dev-process.sh; do
    cp "${source_root}/scripts/${script}" "${fixture}/scripts/${script}"
done

# 与根 CMakeLists.txt 同样的接入方式,其余换成一个最小目标。
cat >"${fixture}/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.20)
project(BuildDirInfoFixture LANGUAGES C)
include("${PROJECT_SOURCE_DIR}/cmake/RealmMeshBuildDirInfo.cmake")
realmmesh_write_build_dir_info()
add_library(fixture STATIC fixture.c)
enable_testing()
add_test(NAME FixtureUnit COMMAND "${CMAKE_COMMAND}" -E true)
set_tests_properties(FixtureUnit PROPERTIES LABELS unit)
EOF
printf 'int fixture_value(void) { return 1; }\n' >"${fixture}/fixture.c"

# 派生用户预设:dev-local 照 CMakeUserPresets.example.json 的形状继承 dev;
# dev-shared 故意与 dev 共用 build/dev,用来验证身份冲突能被识别。
cat >"${fixture}/CMakeUserPresets.json" <<'EOF'
{
  "version": 2,
  "configurePresets": [
    {
      "name": "dev-local",
      "inherits": "dev",
      "binaryDir": "${sourceDir}/build/dev-local"
    },
    {
      "name": "dev-shared",
      "inherits": "dev",
      "binaryDir": "${sourceDir}/build/dev"
    }
  ],
  "buildPresets": [
    {
      "name": "dev-local",
      "configurePreset": "dev-local"
    }
  ],
  "testPresets": [
    {
      "name": "dev-local",
      "inherits": "dev",
      "configurePreset": "dev-local"
    }
  ]
}
EOF

cd "${fixture}"

# --- 未配置:每个入口都给出配置提示,且不需要构建目录的动作照常可用 ---
expect_failure "unconfigured build-dir" ./scripts/build-dir.sh
expect_contains "${last_output}" "./scripts/build.sh --preset dev" \
    "unconfigured build-dir"
expect_failure "unconfigured dev-make" ./scripts/build-dir.sh --preset dev-make
expect_contains "${last_output}" "./scripts/build.sh --preset dev-make" \
    "unconfigured dev-make"
expect_failure "invalid preset name" ./scripts/build-dir.sh --preset ../dev
expect_contains "${last_output}" "Invalid preset name" "invalid preset name"
expect_failure "unknown option" ./scripts/build-dir.sh --bogus
expect_failure "unconfigured test-watch" \
    ./scripts/test-watch.sh --preset dev-make --once
expect_contains "${last_output}" "--preset dev-make" "unconfigured test-watch"
expect_failure "unconfigured dev-services start" \
    ./scripts/dev-services.sh --preset dev-make start
expect_contains "${last_output}" "./scripts/build.sh --preset dev-make" \
    "unconfigured dev-services start"
expect_failure "unconfigured dev-all-in-one start" \
    ./scripts/dev-all-in-one.sh start --preset dev-make
expect_contains "${last_output}" "./scripts/build.sh --preset dev-make" \
    "unconfigured dev-all-in-one start"
[[ ! -e .runtime/pids/supervisor.pid && ! -e .runtime/pids/all-in-one.pid ]] ||
    fail "start must not spawn anything for an unconfigured preset"
# status 只看 PID 文件,不依赖构建目录。
last_output="$(./scripts/dev-services.sh status 2>&1 || true)"
expect_contains "${last_output}" "manager stopped" "dev-services status"
[[ "${last_output}" != *"--preset"* ]] ||
    fail "dev-services status must not require a build directory"
expect_success "build.sh --help" ./scripts/build.sh --help
expect_contains "${last_output}" "--preset" "build.sh --help"

# --- dev 与 dev-make:各自记录真实 binaryDir ---
expect_success "configure dev" "${cmake_bin}" --preset dev
expect_contains "${last_output}" "realm_build_dir: preset dev -> ${fixture}/build/dev" \
    "configure dev"
expect_success "configure dev-make" "${cmake_bin}" --preset dev-make
[[ "$(./scripts/build-dir.sh)" == "${fixture}/build/dev" ]] ||
    fail "default preset must resolve to build/dev"
[[ "$(./scripts/build-dir.sh --preset dev-make)" == "${fixture}/build/dev-make" ]] ||
    fail "dev-make must resolve to build/dev-make"
[[ "$(./scripts/build-dir.sh --preset=dev-make)" == "${fixture}/build/dev-make" ]] ||
    fail "--preset=NAME form must be accepted"
identity="$(cat build/dev-make/realmmesh-build-dir.txt)"
expect_contains "${identity}" "preset=dev-make" "dev-make identity"
expect_contains "${identity}" "source_dir=${fixture}" "dev-make identity"
expect_contains "${identity}" "generator=Unix Makefiles" "dev-make identity"

# 内容不变的重复配置不重写记录文件(不该触发依赖它的增量动作)。
registry="build/.build-dirs/dev-make.txt"
touch -t 200001010000 "${registry}" build/dev-make/realmmesh-build-dir.txt
touch -t 200001010000 "${work_root}/reference"
expect_success "reconfigure dev-make" "${cmake_bin}" --preset dev-make
if [[ "${registry}" -nt "${work_root}/reference" ||
    build/dev-make/realmmesh-build-dir.txt -nt "${work_root}/reference" ]]; then
    fail "an unchanged reconfigure must not rewrite the build directory info"
fi

# --- 记录与目录不符时拒绝,并给出重新配置的提示 ---
cp build/dev-make/realmmesh-build-dir.txt "${work_root}/identity.saved"
sed -e 's/^preset=.*/preset=dev-other/' "${work_root}/identity.saved" \
    >build/dev-make/realmmesh-build-dir.txt
expect_failure "identity mismatch" ./scripts/build-dir.sh --preset dev-make
expect_contains "${last_output}" "does not belong to preset \"dev-make\"" \
    "identity mismatch"
sed -e "s|^source_dir=.*|source_dir=${work_root}/elsewhere|" \
    "${work_root}/identity.saved" >build/dev-make/realmmesh-build-dir.txt
expect_failure "source mismatch" ./scripts/build-dir.sh --preset dev-make
expect_contains "${last_output}" "another source tree" "source mismatch"
cp "${work_root}/identity.saved" build/dev-make/realmmesh-build-dir.txt
mv build/dev-make/CMakeCache.txt "${work_root}/cache.saved"
expect_failure "missing cache" ./scripts/build-dir.sh --preset dev-make
expect_contains "${last_output}" "is not configured" "missing cache"
mv "${work_root}/cache.saved" build/dev-make/CMakeCache.txt
./scripts/build-dir.sh --preset dev-make >/dev/null ||
    fail "restored dev-make must resolve again"

# --- build.sh:派生用户预设走完 configure / build / test,编译数据库跟随所选预设 ---
expect_success "build.sh dev-local" ./scripts/build.sh --preset dev-local
[[ "$(./scripts/build-dir.sh --preset dev-local)" == "${fixture}/build/dev-local" ]] ||
    fail "dev-local must resolve to build/dev-local"
[[ -L compile_commands.json &&
    "$(readlink compile_commands.json)" == "build/dev-local/compile_commands.json" ]] ||
    fail "compile_commands.json must link to the dev-local database"
expect_success "build.sh dev-make" ./scripts/build.sh --preset=dev-make
[[ "$(readlink compile_commands.json)" == "build/dev-make/compile_commands.json" ]] ||
    fail "compile_commands.json must follow the selected preset"
rm compile_commands.json
printf '[]\n' >compile_commands.json
expect_success "build.sh keeps a regular file" ./scripts/build.sh --preset dev
[[ ! -L compile_commands.json && "$(cat compile_commands.json)" == "[]" ]] ||
    fail "build.sh must not overwrite a regular compile_commands.json"
expect_contains "${last_output}" "not a symlink" "build.sh keeps a regular file"
rm compile_commands.json
expect_failure "build.sh unknown option" ./scripts/build.sh --bogus

# --- test-watch:单轮跑所选预设,时间戳落在该预设的构建目录 ---
expect_success "test-watch dev-local" \
    ./scripts/test-watch.sh --preset dev-local --once
[[ -f build/dev-local/.test-watch-stamp ]] ||
    fail "test-watch must stamp the selected build directory"

# --- 两个预设共用一个目录:配置期告警,原预设的记录随之失效 ---
expect_success "configure dev-shared" "${cmake_bin}" --preset dev-shared
expect_contains "${last_output}" "previously configured by preset \"dev\"" \
    "configure dev-shared"
expect_failure "dev after dev-shared" ./scripts/build-dir.sh --preset dev
expect_contains "${last_output}" "does not belong to preset \"dev\"" \
    "dev after dev-shared"

printf 'build directory info: all checks passed\n'
