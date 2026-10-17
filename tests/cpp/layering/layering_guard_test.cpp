// 四层依赖方向守卫(ADR-0014):扫描源码树的 #include "realmmesh/…" 与各模块
// CMakeLists.txt 引用的本仓目标，任何反向依赖都失败。ADR-0014 第 5 步后
// 过渡期倒挂白名单已清空，不再保留。
// 归属判定见 docs/agents/code-layout.md。

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef REALMMESH_SOURCE_DIR
#error "REALMMESH_SOURCE_DIR must point at the repository root"
#endif

namespace {

namespace fs = std::filesystem;

enum class Layer {
    Framework,
    Proto,
    Common,
    Client,
    Service,
    Composition,
    Unknown,
};

Layer layer_of(std::string_view module) {
    if (module.starts_with("framework/")) {
        return Layer::Framework;
    }
    if (module == "proto") {
        return Layer::Proto;
    }
    if (module == "game/common") {
        return Layer::Common;
    }
    if (module == "game/client") {
        return Layer::Client;
    }
    if (module.starts_with("game/")) {
        return Layer::Service;
    }
    if (module.starts_with("apps/") || module.starts_with("tools/")) {
        return Layer::Composition;
    }
    return Layer::Unknown;
}

// 依赖只能自上而下：装配层 → 服务层 → 共享领域层 → 框架层，服务之间互不
// 依赖，服务也不依赖客户端库 game/client。
bool allowed(std::string_view from, std::string_view to) {
    if (from == to) {
        return true;
    }
    const Layer target = layer_of(to);
    switch (layer_of(from)) {
    case Layer::Composition:
        return true;
    case Layer::Framework:
    case Layer::Proto:
        return target == Layer::Framework;
    case Layer::Common:
        return target == Layer::Framework || target == Layer::Proto;
    case Layer::Client:
    case Layer::Service:
        return target == Layer::Framework || target == Layer::Proto ||
               target == Layer::Common;
    case Layer::Unknown:
        return false;
    }
    return false;
}

// include 前缀 → 模块。proto 生成头与 framework 模块共用 realmmesh/ 前缀
// (例如 realmmesh/edge/v1/edge.pb.h),.pb.h 一律归 proto。
using PrefixMap = std::map<std::string, std::string, std::less<>>;

std::string module_of_include(const PrefixMap& prefixes, std::string_view path) {
    if (path.ends_with(".pb.h")) {
        return "proto";
    }
    std::string best;
    std::size_t best_length = 0;
    for (const auto& [prefix, module] : prefixes) {
        if (path.starts_with(prefix) && prefix.size() > best_length) {
            best = module;
            best_length = prefix.size();
        }
    }
    return best;
}

using Edge = std::pair<std::string, std::string>;

struct SourceTree {
    std::vector<std::string> modules;
    PrefixMap include_prefixes;
    // 每条模块间依赖附一处证据(文件与引用),便于定位。
    std::map<Edge, std::string> edges;
};

std::string repo_path(const fs::path& root, const fs::path& path) {
    return fs::relative(path, root).generic_string();
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream content;
    content << in.rdbuf();
    return content.str();
}

bool is_cpp_source(const fs::path& path) {
    const auto extension = path.extension().string();
    return extension == ".hpp" || extension == ".cpp" || extension == ".h" ||
           extension == ".cc" || extension == ".ipp";
}

// `#include "x"` 的引号内容;不是该形式返回空。
std::string quoted_include(std::string_view line) {
    auto position = line.find_first_not_of(" \t");
    if (position == std::string_view::npos || line[position] != '#') {
        return {};
    }
    line.remove_prefix(position + 1);
    position = line.find_first_not_of(" \t");
    if (position == std::string_view::npos ||
        !line.substr(position).starts_with("include")) {
        return {};
    }
    line.remove_prefix(position + std::string_view("include").size());
    const auto open = line.find('"');
    if (open == std::string_view::npos) {
        return {};
    }
    const auto close = line.find('"', open + 1);
    if (close == std::string_view::npos) {
        return {};
    }
    return std::string(line.substr(open + 1, close - open - 1));
}

bool is_cmake_token_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' ||
           c == ':';
}

std::vector<std::string> cmake_tokens(std::string_view content) {
    std::vector<std::string> tokens;
    std::istringstream lines{std::string(content)};
    std::string line;
    while (std::getline(lines, line)) {
        if (const auto comment = line.find('#'); comment != std::string::npos) {
            line.erase(comment);
        }
        std::string token;
        for (const char c : line + ' ') {
            if (is_cmake_token_char(c)) {
                token.push_back(c);
            } else if (!token.empty()) {
                tokens.push_back(std::move(token));
                token.clear();
            }
        }
    }
    return tokens;
}

std::vector<fs::path> files_under(const fs::path& directory, auto&& keep) {
    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(directory)) {
        if (entry.is_regular_file() && keep(entry.path())) {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);
    return files;
}

SourceTree scan(const fs::path& root) {
    SourceTree tree;
    for (const std::string_view top : {"framework", "game", "apps", "tools"}) {
        for (const auto& entry : fs::directory_iterator(root / top)) {
            if (entry.is_directory()) {
                tree.modules.push_back(repo_path(root, entry.path()));
            }
        }
    }
    tree.modules.emplace_back("proto");
    std::ranges::sort(tree.modules);

    for (const auto& module : tree.modules) {
        const auto include_root = root / module / "include" / "realmmesh";
        if (!fs::is_directory(include_root)) {
            continue;
        }
        for (const auto& entry : fs::directory_iterator(include_root)) {
            if (!entry.is_directory()) {
                continue;
            }
            const auto name = entry.path().filename().string();
            if (name != "game") {
                tree.include_prefixes["realmmesh/" + name + "/"] = module;
                continue;
            }
            for (const auto& game : fs::directory_iterator(entry.path())) {
                if (game.is_directory()) {
                    tree.include_prefixes
                        ["realmmesh/game/" + game.path().filename().string() +
                         "/"] = module;
                }
            }
        }
    }

    // 本仓目标名(含 RealmMesh:: 别名)→ 定义它的模块。
    std::map<std::string, std::string, std::less<>> targets;
    std::map<std::string, std::vector<fs::path>> cmake_files;
    for (const auto& module : tree.modules) {
        cmake_files[module] = files_under(root / module, [](const fs::path& p) {
            return p.filename() == "CMakeLists.txt";
        });
        for (const auto& file : cmake_files[module]) {
            const auto tokens = cmake_tokens(read_file(file));
            for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
                if (tokens[i] == "add_library" ||
                    tokens[i] == "add_executable") {
                    targets[tokens[i + 1]] = module;
                }
            }
        }
    }

    const auto record = [&](const std::string& from, const std::string& to,
                            std::string evidence) {
        if (to.empty() || to == from) {
            return;
        }
        tree.edges.try_emplace({from, to}, std::move(evidence));
    };

    for (const auto& module : tree.modules) {
        for (const auto& file : files_under(root / module, is_cpp_source)) {
            std::istringstream lines(read_file(file));
            std::string line;
            while (std::getline(lines, line)) {
                const auto path = quoted_include(line);
                if (!path.starts_with("realmmesh/")) {
                    continue;
                }
                record(module, module_of_include(tree.include_prefixes, path),
                       repo_path(root, file) + ": #include \"" + path + "\"");
            }
        }
        for (const auto& file : cmake_files[module]) {
            for (const auto& token : cmake_tokens(read_file(file))) {
                if (const auto target = targets.find(token);
                    target != targets.end()) {
                    record(module, target->second,
                           repo_path(root, file) + ": " + token);
                }
            }
        }
    }
    return tree;
}

const SourceTree& source_tree() {
    static const SourceTree tree = scan(fs::path(REALMMESH_SOURCE_DIR));
    return tree;
}

TEST(LayeringRulesTest, DependenciesFlowDownward) {
    EXPECT_TRUE(allowed("apps/mesh_host", "game/realm"));
    EXPECT_TRUE(allowed("tools/loadgen", "game/client"));
    EXPECT_TRUE(allowed("game/realm", "game/common"));
    EXPECT_TRUE(allowed("game/realm", "proto"));
    EXPECT_TRUE(allowed("game/realm", "framework/network"));
    EXPECT_TRUE(allowed("game/common", "proto"));
    EXPECT_TRUE(allowed("game/client", "game/common"));
    EXPECT_TRUE(allowed("framework/cluster", "framework/network"));

    EXPECT_FALSE(allowed("game/realm", "game/gateway"));
    EXPECT_FALSE(allowed("game/realm", "game/client"));
    EXPECT_FALSE(allowed("game/realm", "tools/loadgen"));
    EXPECT_FALSE(allowed("game/common", "game/gateway"));
    EXPECT_FALSE(allowed("game/common", "game/client"));
    EXPECT_FALSE(allowed("framework/service_host", "game/queue"));
    EXPECT_FALSE(allowed("framework/network", "game/common"));
    EXPECT_FALSE(allowed("framework/network", "proto"));
    EXPECT_FALSE(allowed("framework/network", "apps/mesh_host"));
    EXPECT_FALSE(allowed("proto", "game/common"));
}

TEST(LayeringRulesTest, IncludesResolveByLongestPrefixAndProtoSuffix) {
    const PrefixMap prefixes{
        {"realmmesh/edge/", "framework/edge"},
        {"realmmesh/game/common/", "game/common"},
        {"realmmesh/game/realm/", "game/realm"},
    };
    EXPECT_EQ(module_of_include(prefixes, "realmmesh/edge/v1/edge.pb.h"),
              "proto");
    EXPECT_EQ(module_of_include(prefixes, "realmmesh/edge/edge_runtime.hpp"),
              "framework/edge");
    EXPECT_EQ(
        module_of_include(prefixes, "realmmesh/game/realm/realm_sessions.hpp"),
        "game/realm");
    EXPECT_EQ(module_of_include(prefixes, "realmmesh/unknown/x.hpp"), "");
}

TEST(LayeringRulesTest, IncludeLinesAreRecognized) {
    EXPECT_EQ(quoted_include("#include \"realmmesh/a/b.hpp\""),
              "realmmesh/a/b.hpp");
    EXPECT_EQ(quoted_include("  #  include \"realmmesh/a/b.hpp\"  // x"),
              "realmmesh/a/b.hpp");
    EXPECT_EQ(quoted_include("#include <vector>"), "");
    EXPECT_EQ(quoted_include("// #include \"realmmesh/a/b.hpp\""), "");
}

TEST(LayeringRulesTest, CmakeCommentsAreIgnored) {
    const auto tokens = cmake_tokens(
        "target_link_libraries(x PUBLIC RealmMesh::Network)  # realm_gateway\n");
    EXPECT_NE(std::ranges::find(tokens, "RealmMesh::Network"), tokens.end());
    EXPECT_EQ(std::ranges::find(tokens, "realm_gateway"), tokens.end());
}

// 扫描本身有效：找到了模块、include 前缀与跨模块依赖，避免路径错误时空转通过。
TEST(LayeringGuardTest, ScanSeesTheSourceTree) {
    const auto& tree = source_tree();
    EXPECT_NE(std::ranges::find(tree.modules, "framework/network"),
              tree.modules.end());
    EXPECT_NE(std::ranges::find(tree.modules, "game/common"),
              tree.modules.end());
    EXPECT_EQ(tree.include_prefixes.count("realmmesh/game/common/"), 1U);
    EXPECT_TRUE(tree.edges.contains({"game/gateway", "framework/network"}));
    EXPECT_TRUE(tree.edges.contains({"game/common", "proto"}));
}

TEST(LayeringGuardTest, NoInversions) {
    for (const auto& [edge, evidence] : source_tree().edges) {
        if (allowed(edge.first, edge.second)) {
            continue;
        }
        ADD_FAILURE() << edge.first << " 依赖 " << edge.second
                      << ",违反 ADR-0014 的依赖方向(" << evidence
                      << ")。归属判定见 docs/agents/code-layout.md。";
    }
}

}  // namespace
