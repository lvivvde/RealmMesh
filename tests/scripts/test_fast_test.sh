#!/usr/bin/env bash

# 快速入口(#125)的行为测试：在临时小工程里用真实的 tests/cmake/test_helpers.cmake、
# CMakePresets.json 与 scripts/ 入口(test-fast.sh、test-watch.sh、build.sh)跑
# 配置、构建与测试，验证 Unit 聚合与精确目标身份、--target / --test-regex /
# --test-jobs / --jobs 的选择与失败契约、资源预算公式，以及 watch 的首轮、期间、
# 增删移变更与不重叠。GTest 换成一个只认 --gtest_list_tests / --gtest_filter
# 的小 main,Lua 解释器换成一个读脚本的小程序，整个工程几秒内构建完。
#
# 用法:test_fast_test.sh <源码根> <cmake 可执行文件>

set -euo pipefail

source_root="$1"
cmake_bin="$2"
ctest_real="$(dirname "${cmake_bin}")/ctest"

work_root="$(mktemp -d)"
work_root="$(cd "${work_root}" && pwd -P)"
fixture="${work_root}/fixture"
watch_pid=""
cleanup() {
    if [[ -n "${watch_pid}" ]]; then
        kill "${watch_pid}" 2>/dev/null || true
        wait "${watch_pid}" 2>/dev/null || true
    fi
    rm -rf -- "${work_root}"
}
trap cleanup EXIT

# 入口从 PATH 找 cmake,ctest 取同目录那份：两者都换成记录参数后转交真实
# 程序的包装，用来断言入口实际传给 ctest 的选项。
ctest_log="${work_root}/ctest-args.log"
mkdir -p "${work_root}/bin"
cat >"${work_root}/bin/cmake" <<EOF
#!/usr/bin/env bash
exec "${cmake_bin}" "\$@"
EOF
cat >"${work_root}/bin/ctest" <<EOF
#!/usr/bin/env bash
printf '%s\n' "\$*" >>"${ctest_log}"
exec "${ctest_real}" "\$@"
EOF
chmod +x "${work_root}/bin/cmake" "${work_root}/bin/ctest"
PATH="${work_root}/bin:${PATH}"
export PATH
# 测量环境里可能带着这些变量;各用例按需显式设置。
unset CTEST_PARALLEL_LEVEL CMAKE_BUILD_PARALLEL_LEVEL MAKEFLAGS || true

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

# CMake 按宽度折行，断点随路径长短变化:比较前把空白压成单个空格。
expect_contains() {
    local flat
    flat="$(printf '%s' "$1" | tr -s '[:space:]' ' ')"
    if [[ "$1" != *"$2"* && "${flat}" != *"$2"* ]]; then
        printf '%s\n' "$1" >&2
        fail "$3: expected output to contain '$2'"
    fi
}

expect_not_contains() {
    if [[ "$1" == *"$2"* ]]; then
        printf '%s\n' "$1" >&2
        fail "$3: expected output not to contain '$2'"
    fi
}

# 运行命令，要求退出码为 <期望>;合并后的输出写进 last_output。
expect_exit() {
    local expected="$1"
    local context="$2"
    shift 2
    local status=0
    last_output="$("$@" 2>&1)" || status=$?
    if [[ "${status}" -ne "${expected}" ]]; then
        printf '%s\n' "${last_output}" >&2
        fail "${context}: expected exit ${expected}, got ${status}"
    fi
}

# 上一次 ctest 调用的参数行。
last_ctest_args() {
    tail -n 1 "${ctest_log}"
}

mkdir -p "${fixture}/cmake" "${fixture}/scripts/lib" "${fixture}/tests/cmake" \
    "${fixture}/tests/fixture/watch"
cp "${source_root}/CMakePresets.json" "${fixture}/"
cp "${source_root}/cmake/RealmMeshBuildDirInfo.cmake" "${fixture}/cmake/"
cp "${source_root}/tests/cmake/test_helpers.cmake" "${fixture}/tests/cmake/"
for script in build.sh build-dir.sh test-fast.sh test-watch.sh \
    lib/build-dir.sh lib/build-jobs.sh; do
    cp "${source_root}/scripts/${script}" "${fixture}/scripts/${script}"
done

cat >"${fixture}/CMakeUserPresets.json" <<'EOF'
{
  "version": 2,
  "configurePresets": [
    { "name": "dev-local", "inherits": "dev", "binaryDir": "${sourceDir}/build/dev-local" }
  ],
  "buildPresets": [
    { "name": "dev-local", "configurePreset": "dev-local" }
  ],
  "testPresets": [
    { "name": "dev-local", "inherits": "dev", "configurePreset": "dev-local" }
  ]
}
EOF

# 与根 CMakeLists.txt 同样的接入方式:注册帮助函数 + 最后写登记清单。
cat >"${fixture}/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.20)
project(TestFastFixture LANGUAGES CXX)
include("${PROJECT_SOURCE_DIR}/cmake/RealmMeshBuildDirInfo.cmake")
realmmesh_write_build_dir_info()
enable_testing()

option(FIXTURE_BREAK_ALPHA "Make alpha_test fail to compile" OFF)
option(FIXTURE_REMOVE_BETA_BINARY "Delete the beta.x_test binary after linking" OFF)
option(FIXTURE_ADD_GAMMA "Register one more Unit target" OFF)
option(FIXTURE_BAD_LABELS "Register a test without a classification label" OFF)

set(fixture_dir "${PROJECT_SOURCE_DIR}/tests/fixture")
add_library(fixture_gtest_main STATIC "${fixture_dir}/gtest_main.cpp")
target_compile_features(fixture_gtest_main PUBLIC cxx_std_20)
add_library(GTest::gtest_main ALIAS fixture_gtest_main)
add_executable(realm_lua_cli "${fixture_dir}/lua_cli.cpp")
set(luaunit_SOURCE_DIR "${fixture_dir}")
set(REALMMESH_TEST_WORKING_DIRECTORY "${PROJECT_BINARY_DIR}")

include("${PROJECT_SOURCE_DIR}/tests/cmake/test_helpers.cmake")

set(alpha_definitions "")
if(FIXTURE_BREAK_ALPHA)
    set(alpha_definitions FIXTURE_BREAK_ALPHA)
endif()
realm_add_gtest(alpha_test SOURCES "${fixture_dir}/alpha.cpp"
    DEFINITIONS ${alpha_definitions})
# 名字里的点在身份标签里必须按字面匹配，不能顺带选中 betaxx_test。
realm_add_gtest(beta.x_test SOURCES "${fixture_dir}/beta.cpp")
realm_add_gtest(betaxx_test SOURCES "${fixture_dir}/betaxx.cpp")
# 跳过走退出码约定(3.20 的发现脚本没有 SKIPPED 输出匹配)。
realm_add_gtest(skip_test SOURCES "${fixture_dir}/skip.cpp"
    DISCOVER_PROPERTIES SKIP_RETURN_CODE 77)
realm_add_gtest(slow_integration_test SOURCES "${fixture_dir}/integration.cpp"
    LABELS integration
    DISCOVER_PROPERTIES RUN_SERIAL TRUE)
realm_add_lua_test(LuaUnitSuite "${fixture_dir}/unit_suite.lua")
realm_add_lua_test(LuaIntegrationSuite "${fixture_dir}/integration_suite.lua"
    LABELS integration lua)
if(FIXTURE_ADD_GAMMA)
    realm_add_gtest(gamma_test SOURCES "${fixture_dir}/gamma.cpp")
endif()
if(FIXTURE_BAD_LABELS)
    realm_add_gtest(bad_test SOURCES "${fixture_dir}/gamma.cpp" LABELS lua)
endif()
if(FIXTURE_REMOVE_BETA_BINARY)
    add_custom_command(TARGET beta.x_test POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E rm -f "$<TARGET_FILE:beta.x_test>")
endif()

realmmesh_write_unit_test_registry()
EOF

cat >"${fixture}/tests/fixture/fixture_case.h" <<'EOF'
struct FixtureCase {
    const char* name;
    int (*run)();
};
extern const char* const fixture_suite;
extern const FixtureCase fixture_cases[];
extern const int fixture_case_count;
EOF
cat >"${fixture}/tests/fixture/gtest_main.cpp" <<'EOF'
#include "fixture_case.h"

#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--gtest_list_tests") == 0) {
            std::printf("%s.\n", fixture_suite);
            for (int c = 0; c < fixture_case_count; ++c) {
                std::printf("  %s\n", fixture_cases[c].name);
            }
            return 0;
        }
    }
    const char* prefix = "--gtest_filter=";
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], prefix, std::strlen(prefix)) != 0) {
            continue;
        }
        const std::string wanted = argv[i] + std::strlen(prefix);
        for (int c = 0; c < fixture_case_count; ++c) {
            if (wanted == std::string(fixture_suite) + "." + fixture_cases[c].name) {
                return fixture_cases[c].run();
            }
        }
        return 1;
    }
    int status = 0;
    for (int c = 0; c < fixture_case_count; ++c) {
        status |= fixture_cases[c].run();
    }
    return status;
}
EOF
cat >"${fixture}/tests/fixture/alpha.cpp" <<'EOF'
#include "fixture_case.h"

#include <cstdio>
#include <cstdlib>

#ifdef FIXTURE_BREAK_ALPHA
#error "alpha_test is broken on purpose"
#endif

namespace {
// 一次性触发器：存在就删掉并改一个受监听文件，模拟「测试期间保存了变更」。
int first() {
    if (const char* trigger = std::getenv("FIXTURE_TRIGGER")) {
        if (std::remove(trigger) == 0) {
            if (const char* touched = std::getenv("FIXTURE_TOUCHED")) {
                if (std::FILE* file = std::fopen(touched, "a")) {
                    std::fputs("changed during a round\n", file);
                    std::fclose(file);
                }
            }
        }
    }
    return 0;
}
int second() { return std::getenv("FIXTURE_FAIL") != nullptr ? 1 : 0; }
}  // namespace

const char* const fixture_suite = "Alpha";
const FixtureCase fixture_cases[] = {{"First", first}, {"Second", second}};
const int fixture_case_count = 2;
EOF
for spec in "beta:BetaDot" "betaxx:BetaXx" "integration:Integration" "gamma:Gamma"; do
    cat >"${fixture}/tests/fixture/${spec%%:*}.cpp" <<EOF
#include "fixture_case.h"

namespace {
int only() { return 0; }
}  // namespace

const char* const fixture_suite = "${spec#*:}";
const FixtureCase fixture_cases[] = {{"Only", only}};
const int fixture_case_count = 1;
EOF
done
cat >"${fixture}/tests/fixture/skip.cpp" <<'EOF'
#include "fixture_case.h"

#include <cstdio>

namespace {
int skipped() {
    std::printf("precondition missing\n");
    return 77;
}
}  // namespace

const char* const fixture_suite = "Skip";
const FixtureCase fixture_cases[] = {{"Precondition", skipped}};
const int fixture_case_count = 1;
EOF
cat >"${fixture}/tests/fixture/lua_cli.cpp" <<'EOF'
#include <fstream>
#include <iterator>
#include <string>

// 假解释器：脚本里出现 FAIL 即失败。
int main(int argc, char** argv) {
    if (argc < 2) {
        return 2;
    }
    std::ifstream script(argv[1]);
    const std::string text{std::istreambuf_iterator<char>(script), {}};
    return text.find("FAIL") == std::string::npos ? 0 : 1;
}
EOF
printf -- '-- unit suite\n' >"${fixture}/tests/fixture/unit_suite.lua"
printf -- '-- integration suite\n' >"${fixture}/tests/fixture/integration_suite.lua"
printf 'watched data\n' >"${fixture}/tests/fixture/watch/data.txt"

cd "${fixture}"

# --- 资源预算公式与 jobs 优先级(纯函数，不依赖本机资源) ---
budget_case() {
    local cpus="$1" gib_hundredths="$2" expected="$3"
    local bytes=$((gib_hundredths * 1073741824 / 100))
    local actual
    actual="$(bash -c 'source scripts/lib/build-jobs.sh; realmmesh_compute_jobs_budget "$1" "$2"' \
        _ "${cpus}" "${bytes}")"
    [[ "${actual}" == "${expected}" ]] ||
        fail "budget(${cpus} CPUs, ${gib_hundredths}/100 GiB): expected ${expected}, got ${actual}"
}
budget_case 15 4800 8    # 开发 Mac
budget_case 8 773 2      # Lima VM
budget_case 4 1600 4     # CI runner 形状:受 CPU 限
budget_case 4 6400 4
budget_case 16 600 2     # 受内存限
budget_case 2 300 1      # 内存不足 4GiB 也保底 1 路
budget_case 1 100 1
[[ "$(bash -c 'source scripts/lib/build-jobs.sh; realmmesh_compute_jobs_budget "" 8589934592')" == 1 ]] ||
    fail "unknown CPU count must budget 1 job"
[[ "$(bash -c 'source scripts/lib/build-jobs.sh; realmmesh_compute_jobs_budget 8 ""')" == 1 ]] ||
    fail "unknown memory must budget 1 job"

# --- 首次使用：未配置也能直接启动，默认只构建并运行 Unit 范围 ---
[[ ! -e build/dev ]] || fail "fixture must start unconfigured"
expect_exit 0 "first test-fast" ./scripts/test-fast.sh
expect_contains "${last_output}" "all registered Unit targets" "first test-fast"
expect_contains "${last_output}" "build jobs:" "first test-fast"
expect_contains "${last_output}" "--target realmmesh_unit_tests" "first test-fast"
expect_contains "${last_output}" \
    "passed with skips: 5 passed, 0 failed, 1 skipped of 6 selected; the skipped tests were not verified." \
    "first test-fast"
expect_contains "${last_output}" "time configure" "first test-fast"
expect_not_contains "${last_output}" "PASSED" "skips are not a full pass"
expect_not_contains "${last_output}" "Integration" "default selection"
[[ ! -e build/dev/slow_integration_test ]] ||
    fail "the Unit aggregate must not build integration targets"
[[ -x build/dev/alpha_test && -x build/dev/realm_lua_cli ]] ||
    fail "the Unit aggregate must build Unit targets and realm_lua_cli"
[[ "$(readlink compile_commands.json)" == "build/dev/compile_commands.json" ]] ||
    fail "test-fast must link compile_commands.json to the selected preset"
expect_contains "$(last_ctest_args)" "-L ^unit$ -j 4 --no-tests=error" "default ctest arguments"

# 登记清单与标签：分类标签与身份标签是同一个列表里的独立元素。
registry="$(cat build/dev/realmmesh-unit-targets.txt)"
expect_contains "${registry}" "alpha_test=${fixture}/build/dev/alpha_test" "registry"
expect_contains "${registry}" "realm_lua_cli=" "registry"
expect_not_contains "${registry}" "slow_integration_test" "registry"
labels="$("${ctest_real}" --test-dir build/dev -N --print-labels)"
expect_contains "${labels}" "target=alpha_test" "labels"
expect_contains "${labels}" "target=realm_lua_cli" "labels"
if printf '%s\n' "${labels}" | grep -q ';'; then
    printf '%s\n' "${labels}" >&2
    fail "labels must be separate list elements"
fi
count_tests() {
    "${ctest_real}" --test-dir build/dev -N "$@" | sed -n 's/^Total Tests: //p'
}
[[ "$(count_tests -L '^unit$')" == 6 ]] || fail "6 tests must carry the unit label"
[[ "$(count_tests -L '^target=alpha_test$')" == 2 ]] || fail "alpha_test identity"
[[ "$(count_tests -L '^lua$')" == 2 ]] || fail "both Lua suites keep the lua label"
[[ "$(count_tests -L '^integration$')" == 2 ]] || fail "integration label"

# --- CTest 并行：默认 4,可退回 1;环境变量不改变入口语义 ---
expect_exit 0 "inherited CTEST_PARALLEL_LEVEL" \
    env CTEST_PARALLEL_LEVEL=1 ./scripts/test-fast.sh --target alpha_test
expect_contains "$(last_ctest_args)" "-j 4" "inherited CTEST_PARALLEL_LEVEL"
expect_exit 0 "--test-jobs 1" ./scripts/test-fast.sh --target alpha_test --test-jobs 1
expect_contains "$(last_ctest_args)" "-j 1" "--test-jobs 1"
expect_exit 64 "--test-jobs 0" ./scripts/test-fast.sh --test-jobs 0
expect_exit 64 "--test-jobs x" ./scripts/test-fast.sh --test-jobs=x

# --- 编译 jobs:--jobs > CMAKE_BUILD_PARALLEL_LEVEL > 预算;非法值失败 ---
expect_exit 0 "--jobs 3" env CMAKE_BUILD_PARALLEL_LEVEL=5 \
    ./scripts/test-fast.sh --target alpha_test --jobs 3
expect_contains "${last_output}" "build jobs: 3 (from --jobs" "--jobs 3"
expect_contains "${last_output}" "--parallel 3" "--jobs 3"
expect_exit 0 "CMAKE_BUILD_PARALLEL_LEVEL" env CMAKE_BUILD_PARALLEL_LEVEL=5 \
    ./scripts/test-fast.sh --target alpha_test
expect_contains "${last_output}" "build jobs: 5 (from CMAKE_BUILD_PARALLEL_LEVEL" \
    "CMAKE_BUILD_PARALLEL_LEVEL"
expect_exit 0 "empty CMAKE_BUILD_PARALLEL_LEVEL" env CMAKE_BUILD_PARALLEL_LEVEL= \
    ./scripts/test-fast.sh --target alpha_test
expect_contains "${last_output}" "(from budget; budget" "empty CMAKE_BUILD_PARALLEL_LEVEL"
expect_exit 64 "--jobs 0" ./scripts/test-fast.sh --jobs 0
expect_exit 64 "invalid CMAKE_BUILD_PARALLEL_LEVEL" env CMAKE_BUILD_PARALLEL_LEVEL=many \
    ./scripts/test-fast.sh
expect_exit 2 "build.sh --jobs 0" ./scripts/build.sh --jobs 0

# --- 聚焦：只接受已登记 Unit 目标，身份按字面匹配 ---
expect_exit 0 "--target beta.x_test" ./scripts/test-fast.sh --target beta.x_test
expect_contains "${last_output}" "1 passed, 0 failed, 0 skipped of 1 selected" \
    "--target beta.x_test"
expect_contains "$(last_ctest_args)" '-L ^target=beta\.x_test$ -LE ^integration$' \
    "--target beta.x_test"
expect_not_contains "${last_output}" "BetaXx" "--target beta.x_test"
expect_exit 0 "--target realm_lua_cli" ./scripts/test-fast.sh --target realm_lua_cli
expect_contains "${last_output}" "LuaUnitSuite" "--target realm_lua_cli"
expect_not_contains "${last_output}" "LuaIntegrationSuite" "--target realm_lua_cli"
expect_exit 0 "--target alpha_test --test-regex" \
    ./scripts/test-fast.sh --target alpha_test --test-regex 'Alpha\.Second'
expect_contains "${last_output}" "1 passed, 0 failed, 0 skipped of 1 selected" \
    "--target alpha_test --test-regex"
expect_exit 64 "integration target" ./scripts/test-fast.sh --target slow_integration_test
expect_contains "${last_output}" "is not a registered Unit target" "integration target"
expect_exit 64 "unknown target" ./scripts/test-fast.sh --target no_such_test
expect_exit 64 "unknown option" ./scripts/test-fast.sh --rerun-failed
expect_exit 2 "empty selection" ./scripts/test-fast.sh --test-regex 'NoSuchTest'
expect_contains "${last_output}" "no tests matched the selection" "empty selection"
expect_exit 64 "empty regex" ./scripts/test-fast.sh --test-regex ''

# --- 失败不产生假绿 ---
expect_exit 2 "test failure" env FIXTURE_FAIL=1 ./scripts/test-fast.sh --target alpha_test
expect_contains "${last_output}" "FAILED: 1 passed, 1 failed, 0 skipped of 2 selected." \
    "test failure"

"${cmake_bin}" --preset dev -DFIXTURE_BREAK_ALPHA=ON >/dev/null
: >"${ctest_log}"
expect_exit 1 "build failure" ./scripts/test-fast.sh
expect_contains "${last_output}" "build failed; tests were not run" "build failure"
[[ ! -s "${ctest_log}" ]] || fail "a failed build must not run the old test binaries"
"${cmake_bin}" --preset dev -DFIXTURE_BREAK_ALPHA=OFF >/dev/null

"${cmake_bin}" --preset dev -DFIXTURE_REMOVE_BETA_BINARY=ON >/dev/null
rm -f build/dev/beta.x_test   # 让它重新链接，从而执行删除产物的 POST_BUILD
expect_exit 1 "missing binary" ./scripts/test-fast.sh --target beta.x_test
expect_contains "${last_output}" "Missing test binary after the build: beta.x_test" \
    "missing binary"
[[ ! -s "${ctest_log}" ]] || fail "a missing binary must not run tests"
"${cmake_bin}" --preset dev -DFIXTURE_REMOVE_BETA_BINARY=OFF >/dev/null

"${cmake_bin}" --preset dev -DFIXTURE_BAD_LABELS=ON >/dev/null 2>&1 || true
expect_exit 1 "unclassified labels" ./scripts/test-fast.sh
expect_contains "${last_output}" "must carry exactly one of unit / integration" \
    "unclassified labels"
expect_contains "${last_output}" "configure failed" "unclassified labels"
"${cmake_bin}" --preset dev -DFIXTURE_BAD_LABELS=OFF >/dev/null

# --- 新 Unit 目标自动进入聚合与登记 ---
"${cmake_bin}" --preset dev -DFIXTURE_ADD_GAMMA=ON >/dev/null
expect_exit 0 "new Unit target" ./scripts/test-fast.sh
expect_contains "${last_output}" "Gamma.Only" "new Unit target"
expect_exit 0 "focus the new target" ./scripts/test-fast.sh --target gamma_test

# --- 完整入口:ALL + 完整 CTest,测试并行固定 1 ---
: >"${ctest_log}"
expect_exit 0 "build.sh" env CTEST_PARALLEL_LEVEL=8 ./scripts/build.sh --jobs 2
expect_contains "$(last_ctest_args)" "--preset dev -j 1" "build.sh"
expect_contains "${last_output}" "build jobs: 2 (from --jobs" "build.sh"
[[ -x build/dev/slow_integration_test ]] || fail "build.sh must build ALL"

# --- 派生用户预设:test-fast 与 watch 走同一预设 ---
expect_exit 0 "test-fast dev-local" ./scripts/test-fast.sh --preset dev-local --target alpha_test
[[ "$(readlink compile_commands.json)" == "build/dev-local/compile_commands.json" ]] ||
    fail "compile_commands.json must follow the selected preset"
expect_exit 0 "test-watch --once" ./scripts/test-watch.sh --once --preset dev-local \
    --target alpha_test
expect_contains "${last_output}" "round 1 end" "test-watch --once"
expect_contains "${last_output}" "Unit target alpha_test" "test-watch --once"
expect_exit 2 "test-watch --once failure" env FIXTURE_FAIL=1 \
    ./scripts/test-watch.sh --once --target alpha_test
expect_exit 64 "test-watch unknown option" ./scripts/test-watch.sh --bogus
expect_exit 64 "test-watch usage passes through" ./scripts/test-watch.sh --once --test-jobs 0

# --- watch:首轮与期间变更、修改、新增、移动、删除，不重叠也不自触发 ---
watch_log="${work_root}/watch.log"
wait_for_rounds() {
    local want="$1"
    local context="$2"
    local deadline=$((SECONDS + 120))
    while [[ "$(grep -c 'test-watch round [0-9]* end' "${watch_log}" || true)" -lt "${want}" ]]; do
        if [[ ${SECONDS} -ge ${deadline} ]] || ! kill -0 "${watch_pid}" 2>/dev/null; then
            cat "${watch_log}" >&2
            fail "watch (${context}): expected ${want} finished rounds"
        fi
        sleep 0.2
    done
}
rounds_finished() {
    grep -c 'test-watch round [0-9]* end' "${watch_log}" || true
}

watch_tools=(poll)
command -v fswatch >/dev/null 2>&1 && watch_tools+=(fswatch)
command -v inotifywait >/dev/null 2>&1 && watch_tools+=(inotify)
data="tests/fixture/watch/data.txt"
for tool in "${watch_tools[@]}"; do
    : >"${watch_log}"
    trigger="${fixture}/tests/fixture/watch/trigger"
    : >"${trigger}"
    REALMMESH_WATCH_TOOL="${tool}" REALMMESH_WATCH_POLL_INTERVAL=0.2 \
        REALMMESH_WATCH_RECHECK_INTERVAL=1 REALMMESH_WATCH_DEBOUNCE=0.2 \
        FIXTURE_TRIGGER="${trigger}" FIXTURE_TOUCHED="${fixture}/${data}" \
        ./scripts/test-watch.sh --target alpha_test >"${watch_log}" 2>&1 &
    watch_pid=$!

    # 第一轮里测试删掉触发器并改写受监听文件：不需要外部动作就该接着跑第二轮。
    wait_for_rounds 2 "${tool}: change during the first round"
    [[ ! -e "${trigger}" ]] || fail "watch (${tool}): the first round must run the tests"
    expect_contains "$(cat "${watch_log}")" "~ ./${data}" "watch (${tool}) first round"

    printf 'edit\n' >>"${data}"
    wait_for_rounds 3 "${tool}: modify"
    printf 'new\n' >tests/fixture/watch/added.txt
    wait_for_rounds 4 "${tool}: add"
    expect_contains "$(cat "${watch_log}")" "+ ./tests/fixture/watch/added.txt" \
        "watch (${tool}) add"
    mv tests/fixture/watch/added.txt tests/fixture/watch/moved.txt
    wait_for_rounds 5 "${tool}: move"
    expect_contains "$(cat "${watch_log}")" "- ./tests/fixture/watch/added.txt" \
        "watch (${tool}) move"
    rm tests/fixture/watch/moved.txt
    wait_for_rounds 6 "${tool}: delete"
    expect_contains "$(cat "${watch_log}")" "- ./tests/fixture/watch/moved.txt" \
        "watch (${tool}) delete"
    # 编辑器临时文件与构建输出都不触发新一轮。
    printf 'swap\n' >tests/fixture/watch/.data.txt.swp
    sleep 3
    [[ "$(rounds_finished)" -eq 6 ]] || {
        cat "${watch_log}" >&2
        fail "watch (${tool}): editor temp files or its own build must not trigger rounds"
    }
    rm -f tests/fixture/watch/.data.txt.swp

    # 后台作业在非交互 shell 里忽略 SIGINT,这里用 TERM(同一个停止处理)。
    kill -TERM "${watch_pid}"
    watch_status=0
    wait "${watch_pid}" 2>/dev/null || watch_status=$?
    watch_pid=""
    [[ "${watch_status}" -eq 0 ]] || fail "watch (${tool}): stopping must exit 0, got ${watch_status}"
    expect_contains "$(cat "${watch_log}")" "test-watch stopped." "watch (${tool}) stop"

    # 轮次严格交替 start/end:同一时刻最多一轮。
    markers="$(grep -oE 'round [0-9]+ (start|end)' "${watch_log}" | awk '{ print $3 }' |
        tr '\n' ' ')"
    [[ "${markers}" == "start end start end start end start end start end start end " ]] || {
        cat "${watch_log}" >&2
        fail "watch (${tool}): rounds must not overlap (${markers})"
    }
    printf 'watch tool %s: first-round, modify, add, move and delete covered\n' "${tool}"
done

printf 'test-fast entry: all checks passed\n'
