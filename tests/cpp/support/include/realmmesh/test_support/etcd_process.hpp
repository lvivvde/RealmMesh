#pragma once

/// 真实单节点 etcd 的测试夹具。网关的准入消费存储是线性一致存储
/// (ADR-0009),attach 路径真实依赖它:用自己写的假 CAS 替身去证明外部
/// 系统的契约,缺陷无法归因,所以跨进程用例自带一个真 etcd 进程。
///
/// 二进制由 ./scripts/install-etcd.sh 安装到 .tools/(可用
/// REALMMESH_ETCD_BINARY 覆盖);缺失即抛错而不是静默跳过 —— 少测一条
/// 链路应当是一次可见的失败。

#include <arpa/inet.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace realm::test_support {

/// 同时绑定全部探测套接字后再回读端口,保证返回值互不相同:逐个取端口时
/// 内核可能把刚释放的同一个临时端口再分回来。
[[nodiscard]] inline std::vector<std::uint16_t> unused_loopback_ports(
    std::size_t count) {
    std::vector<int> descriptors;
    std::vector<std::uint16_t> ports;
    descriptors.reserve(count);
    ports.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
        if (descriptor < 0) {
            throw std::runtime_error("cannot open probe socket");
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = 0;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(
                descriptor,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) != 0) {
            ::close(descriptor);
            throw std::runtime_error("cannot bind probe socket");
        }
        socklen_t length = sizeof(address);
        if (::getsockname(
                descriptor,
                reinterpret_cast<sockaddr*>(&address),
                &length) != 0) {
            ::close(descriptor);
            throw std::runtime_error("cannot read probe socket port");
        }
        descriptors.push_back(descriptor);
        ports.push_back(ntohs(address.sin_port));
    }
    for (const int descriptor : descriptors) ::close(descriptor);
    return ports;
}

/// 裸 connect 探活:端口是否已被监听。
[[nodiscard]] inline bool loopback_port_open(std::uint16_t port) {
    const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool open =
        ::connect(
            descriptor,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) == 0;
    ::close(descriptor);
    return open;
}

class EtcdProcess final {
public:
    EtcdProcess() {
        const auto binary = etcd_binary();
        if (!std::filesystem::is_regular_file(binary)) {
            throw std::runtime_error(
                "etcd binary not found at " + binary.string() +
                "; run ./scripts/install-etcd.sh first");
        }
        std::error_code error;
        data_dir_ = std::filesystem::temp_directory_path() /
            ("realmmesh-etcd-" +
             std::to_string(static_cast<long long>(::getpid())));
        std::filesystem::remove_all(data_dir_, error);
        std::filesystem::create_directories(data_dir_, error);
        if (error) throw std::runtime_error("cannot create etcd data dir");

        const auto ports = unused_loopback_ports(2);
        client_port_ = ports.at(0);
        peer_port_ = ports.at(1);
        const std::string client_url =
            "http://127.0.0.1:" + std::to_string(client_port_);
        const std::string peer_url =
            "http://127.0.0.1:" + std::to_string(peer_port_);
        const std::string cluster = "realmmesh-test=" + peer_url;

        pid_ = ::fork();
        if (pid_ < 0) throw std::runtime_error("fork failed");
        if (pid_ == 0) {
            ::execl(
                binary.c_str(),
                binary.c_str(),
                "--name",
                "realmmesh-test",
                "--data-dir",
                data_dir_.c_str(),
                "--listen-client-urls",
                client_url.c_str(),
                "--advertise-client-urls",
                client_url.c_str(),
                "--listen-peer-urls",
                peer_url.c_str(),
                "--initial-advertise-peer-urls",
                peer_url.c_str(),
                "--initial-cluster",
                cluster.c_str(),
                "--log-level",
                "error",
                static_cast<char*>(nullptr));
            _exit(127);
        }
    }
    ~EtcdProcess() { stop(); }
    EtcdProcess(const EtcdProcess&) = delete;
    EtcdProcess& operator=(const EtcdProcess&) = delete;

    void stop() noexcept {
        if (pid_ <= 0) return;
        static_cast<void>(::kill(pid_, SIGTERM));
        int status = 0;
        static_cast<void>(::waitpid(pid_, &status, 0));
        pid_ = -1;
        std::error_code error;
        std::filesystem::remove_all(data_dir_, error);
    }

    [[nodiscard]] std::string endpoint() const {
        return "http://127.0.0.1:" + std::to_string(client_port_);
    }

    /// 客户端 URL 可连接后,单节点 etcd 仍可能在选举窗口内返回瞬时错误;
    /// 留一小段稳定期,免得服务启动时的就绪探测踩在窗口里。
    void wait_ready(std::chrono::milliseconds timeout =
                        std::chrono::milliseconds{10'000}) {
        using namespace std::chrono_literals;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (loopback_port_open(client_port_)) {
                std::this_thread::sleep_for(200ms);
                return;
            }
            std::this_thread::sleep_for(10ms);
        }
        throw std::runtime_error("etcd did not open its client port");
    }

private:
    [[nodiscard]] static std::filesystem::path etcd_binary() {
        const char* override_path = std::getenv("REALMMESH_ETCD_BINARY");
        if (override_path != nullptr && *override_path != '\0') {
            return override_path;
        }
        // 用例目标通常注入 REALMMESH_TEST_SOURCE_DIR 指向仓库根;未注入时
        // 退回 PATH 上的 etcd,仍找不到就在构造处带安装提示失败。
#ifdef REALMMESH_TEST_SOURCE_DIR
        const std::filesystem::path in_tree =
            std::filesystem::path(REALMMESH_TEST_SOURCE_DIR) / ".tools" /
            "etcd-v3.6.14" / "etcd";
        if (std::filesystem::is_regular_file(in_tree)) {
            return in_tree;
        }
#endif
        return "etcd";
    }

    pid_t pid_{-1};
    std::uint16_t client_port_{0};
    std::uint16_t peer_port_{0};
    std::filesystem::path data_dir_;
};

}  // namespace realm::test_support
