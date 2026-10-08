#pragma once

/// 真实单节点 MongoDB 副本集的测试夹具。Player Data 的权威来源是
/// MongoDB(ADR-0011),majority 读写与事务只有在真实副本集上才有意义:
/// 用内存替身证明外部系统的契约，缺陷无法归因，所以存储层与跨进程用例
/// 自带一个真 mongod。
///
/// 二进制按 REALMMESH_MONGOD_BINARY / REALMMESH_MONGOSH_BINARY 覆盖、
/// PATH(macOS 上即 Homebrew 安装的 mongodb-community 与 mongosh)、
/// ./scripts/install-mongodb.sh 装到 .tools/ 的 Linux 固定版本依次查找;
/// 缺失即抛错而不是静默跳过。初始化可交给独立辅助进程，回读仍经 mongosh 完成；
/// 测试二进制不链接额外驱动，也就不会与被测代码争用进程唯一的驱动实例。

#include "realmmesh/test_support/etcd_process.hpp"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace realm::test_support {

class MongodProcess final {
public:
    MongodProcess() {
        const auto binary = tool_binary(
            "REALMMESH_MONGOD_BINARY", "mongodb-8.0.32", "mongod");
        std::error_code error;
        data_dir_ = std::filesystem::temp_directory_path() /
            ("realmmesh-mongod-" +
             std::to_string(static_cast<long long>(::getpid())) + "-" +
             std::to_string(next_instance()));
        std::filesystem::remove_all(data_dir_, error);
        std::filesystem::create_directories(data_dir_, error);
        if (error) throw std::runtime_error("cannot create mongod data dir");

        port_ = unused_loopback_ports(1).at(0);
        const auto port = std::to_string(port_);
        const auto log = (data_dir_ / "mongod.log").string();
        pid_ = ::fork();
        if (pid_ < 0) throw std::runtime_error("fork failed");
        if (pid_ == 0) {
            ::execlp(
                binary.c_str(),
                binary.c_str(),
                "--replSet",
                "rs0",
                "--bind_ip",
                "127.0.0.1",
                "--port",
                port.c_str(),
                "--dbpath",
                data_dir_.c_str(),
                "--logpath",
                log.c_str(),
                "--nounixsocket",
                "--wiredTigerCacheSizeGB",
                "0.25",
                static_cast<char*>(nullptr));
            _exit(127);
        }
        try {
            initiate_replica_set();
        } catch (...) {
            stop();
            throw;
        }
    }
    ~MongodProcess() { stop(); }
    MongodProcess(const MongodProcess&) = delete;
    MongodProcess& operator=(const MongodProcess&) = delete;

    /// 同一测试二进制内共享一个 mongod:启动加选举要数秒，用例之间改用
    /// fresh_database() 隔离。
    [[nodiscard]] static MongodProcess& shared() {
        static MongodProcess process;
        return process;
    }

    void stop() noexcept {
        if (pid_ <= 0) return;
        static_cast<void>(::kill(pid_, SIGTERM));
        int status = 0;
        static_cast<void>(::waitpid(pid_, &status, 0));
        pid_ = -1;
        std::error_code error;
        std::filesystem::remove_all(data_dir_, error);
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    [[nodiscard]] std::string uri() const {
        return "mongodb://127.0.0.1:" + std::to_string(port_) +
               "/?replicaSet=rs0";
    }

    /// 进程内唯一的库名，用例之间互不可见。
    [[nodiscard]] static std::string fresh_database() {
        return "realmmesh_test_" +
               std::to_string(static_cast<long long>(::getpid())) + "_" +
               std::to_string(next_instance());
    }

    /// 在 database 上执行一段 mongosh 脚本并返回其标准输出(去掉末尾换行);
    /// 用 print(...) 输出需要回读的值。脚本失败时抛错。
    [[nodiscard]] std::string eval(
        const std::string& database, const std::string& script) const {
        const auto [status, output] = run_mongosh(
            "mongodb://127.0.0.1:" + std::to_string(port_) + "/" + database +
                "?replicaSet=rs0",
            script);
        if (status != 0) {
            throw std::runtime_error("mongosh eval failed: " + output);
        }
        return output;
    }

private:
    struct Captured final {
        int status{-1};
        std::string output;
    };

    [[nodiscard]] static std::uint64_t next_instance() {
        static std::atomic<std::uint64_t> counter{0};
        return ++counter;
    }

    [[nodiscard]] static std::string tool_binary(
        const char* environment,
        [[maybe_unused]] const char* tool_directory,
        const char* name) {
        const char* override_path = std::getenv(environment);
        if (override_path != nullptr && *override_path != '\0') {
            return override_path;
        }
        if (const char* path = std::getenv("PATH"); path != nullptr) {
            std::string_view remaining(path);
            while (!remaining.empty()) {
                const auto separator = remaining.find(':');
                const auto directory = remaining.substr(0, separator);
                if (!directory.empty()) {
                    const auto candidate =
                        std::filesystem::path(directory) / name;
                    if (::access(candidate.c_str(), X_OK) == 0) {
                        return candidate.string();
                    }
                }
                if (separator == std::string_view::npos) break;
                remaining.remove_prefix(separator + 1);
            }
        }
#ifdef REALMMESH_TEST_SOURCE_DIR
        const auto in_tree = std::filesystem::path(REALMMESH_TEST_SOURCE_DIR) /
            ".tools" / tool_directory / "bin" / name;
        if (std::filesystem::is_regular_file(in_tree)) return in_tree.string();
#endif
        // 都找不到时 exec 失败，由调用方带安装提示报错。
        return name;
    }

    [[nodiscard]] static Captured run_mongosh(
        const std::string& connection, const std::string& script) {
        const auto binary = tool_binary(
            "REALMMESH_MONGOSH_BINARY", "mongosh-2.12.0", "mongosh");
        auto captured = run_process(binary, {connection, "--quiet", "--norc", "--eval", script});
        if (captured.status == 127) {
            throw std::runtime_error(
                "mongosh not found at " + binary +
                "; install MongoDB first (see tests/README.md)");
        }
        return captured;
    }

    [[nodiscard]] static Captured run_process(
        const std::string& binary, const std::vector<std::string>& arguments) {
        std::vector<char*> argv{const_cast<char*>(binary.c_str())};
        for (const auto& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        std::array<int, 2> pipe_fds{};
        if (::pipe(pipe_fds.data()) != 0) {
            throw std::runtime_error("cannot create mongosh pipe");
        }
        const pid_t child = ::fork();
        if (child < 0) {
            ::close(pipe_fds[0]);
            ::close(pipe_fds[1]);
            throw std::runtime_error("fork failed");
        }
        if (child == 0) {
            ::dup2(pipe_fds[1], STDOUT_FILENO);
            ::dup2(pipe_fds[1], STDERR_FILENO);
            ::close(pipe_fds[0]);
            ::close(pipe_fds[1]);
            ::execvp(binary.c_str(), argv.data());
            _exit(127);
        }
        ::close(pipe_fds[1]);
        Captured captured;
        std::array<char, 4096> buffer{};
        while (true) {
            const auto count = ::read(pipe_fds[0], buffer.data(), buffer.size());
            if (count <= 0) break;
            captured.output.append(
                buffer.data(), static_cast<std::size_t>(count));
        }
        ::close(pipe_fds[0]);
        int status = 0;
        static_cast<void>(::waitpid(child, &status, 0));
        captured.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        while (!captured.output.empty() && captured.output.back() == '\n') {
            captured.output.pop_back();
        }
        return captured;
    }

    void initiate_replica_set() {
        using namespace std::chrono_literals;
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (!loopback_port_open(port_)) {
            int status = 0;
            if (::waitpid(pid_, &status, WNOHANG) == pid_) {
                pid_ = -1;
                throw std::runtime_error(
                    "mongod exited during startup; install MongoDB first "
                    "(see tests/README.md)");
            }
            if (std::chrono::steady_clock::now() > deadline) {
                throw std::runtime_error("mongod did not open its port");
            }
            std::this_thread::sleep_for(20ms);
        }

        const char* initializer = std::getenv("REALMMESH_TEST_MONGODB_INITIALIZER");
#ifdef REALMMESH_TEST_MONGODB_INITIALIZER_PATH
        if (initializer == nullptr || *initializer == '\0') {
            initializer = REALMMESH_TEST_MONGODB_INITIALIZER_PATH;
        }
#endif
        if (initializer != nullptr && *initializer != '\0') {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) throw std::runtime_error("mongod initialization deadline exceeded");
            const auto result = run_process(initializer, {
                "127.0.0.1:" + std::to_string(port_), std::to_string(remaining)});
            if (result.status != 0) {
                throw std::runtime_error("MongoDB fixture initializer failed: " + result.output);
            }
            return;
        }

        const auto direct = "mongodb://127.0.0.1:" + std::to_string(port_) +
                            "/?directConnection=true";
        const auto member = "127.0.0.1:" + std::to_string(port_);
        const auto initiated = run_mongosh(
            direct,
            "rs.initiate({_id: 'rs0', members: [{_id: 0, host: '" + member +
                "'}]}); print('ok')");
        if (initiated.status != 0) {
            throw std::runtime_error(
                "rs.initiate failed: " + initiated.output);
        }
        while (std::chrono::steady_clock::now() < deadline) {
            const auto primary =
                run_mongosh(direct, "print(db.hello().isWritablePrimary)");
            if (primary.status == 0 && primary.output == "true") return;
            std::this_thread::sleep_for(100ms);
        }
        throw std::runtime_error("mongod did not become a writable primary");
    }

    pid_t pid_{-1};
    std::uint16_t port_{0};
    std::filesystem::path data_dir_;
};

/// 把拷贝出的配置树里 common/player_data.lua 改指 mongod 上的 database:
/// 拷贝真实 configs/ 并拉起 realm/gateway 的用例用它，免得连到开发机的
/// 27017。同时删掉 uri_environment,免得 shell 里的私有 URI 把用例导向共享
/// 开发库。找不到要替换的默认值即抛错，配置改名时不会静默连错库。
inline void point_player_data_at(
    const std::filesystem::path& config_root,
    const MongodProcess& mongod,
    const std::string& database) {
    const auto file = config_root / "common" / "player_data.lua";
    std::string contents;
    {
        std::ifstream input(file);
        contents.assign(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
    }
    const auto replace = [&](std::string_view from, const std::string& to) {
        const auto position = contents.find(from);
        if (position == std::string::npos) {
            throw std::runtime_error(
                "player_data.lua lacks " + std::string(from));
        }
        contents.replace(position, from.size(), to);
    };
    replace(
        R"(uri = "mongodb://127.0.0.1:27017/?replicaSet=rs0")",
        "uri = \"" + mongod.uri() + "\"");
    replace(R"(database = "realmmesh")", "database = \"" + database + "\"");
    if (const auto start = contents.find("uri_environment");
        start != std::string::npos) {
        const auto line_start = contents.rfind('\n', start);
        const auto line_end = contents.find('\n', start);
        contents.erase(
            line_start == std::string::npos ? 0 : line_start + 1,
            line_end == std::string::npos ? std::string::npos
                                          : line_end - line_start);
    }
    std::ofstream output(file, std::ios::trunc);
    output << contents;
    if (!output) {
        throw std::runtime_error("cannot rewrite " + file.string());
    }
}

}  // namespace realm::test_support
