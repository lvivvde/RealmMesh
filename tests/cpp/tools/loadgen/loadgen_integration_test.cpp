#include "realmmesh/client/wire_login_transport.hpp"
#include "realmmesh/loadgen/loadgen.hpp"
#include "realmmesh/loadgen/login_chain_adapter.hpp"
#include "realmmesh/loadgen/login_chain_metrics.hpp"
#include "realmmesh/loadgen/metrics_scrape.hpp"
#include "realmmesh/network/tcp/tcp_listener.hpp"
#include "realmmesh/service_host/mesh_host.hpp"
#include "realmmesh/cluster/etcd_service_registry.hpp"
#include "realmmesh/game/common/admission_grant.hpp"
#include "realmmesh/game/common/compact_jws.hpp"
#include "realmmesh/game/common/queue_number_v2.hpp"
#include "realmmesh/test_support/etcd_process.hpp"
#include "realmmesh/test_support/mongod_process.hpp"
#include "realmmesh/test_support/temporary_directory.hpp"

#include <gtest/gtest.h>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <execinfo.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/fcntl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace realm::loadgen {
namespace {

namespace service_host = ::realm::service_host;
namespace common = ::realm::game::common;

/// terminate 处理器(静态安装):裸 std::terminate(macOS 的 noexcept
/// 违约等场景)不打印异常类型,CI 上只剩一行 "libc++abi: terminating",
/// 无从定位;落一份 backtrace 到 stderr(无缓冲,随 abort 保留)。
struct TerminateBacktraceInstaller final {
    TerminateBacktraceInstaller() {
        std::set_terminate([] {
            void* frames[64];
            const int depth = ::backtrace(frames, 64);
            std::fprintf(stderr, "terminate backtrace (depth=%d):\n", depth);
            std::fflush(stderr);
            ::backtrace_symbols_fd(frames, depth, 2);
            std::abort();
        });
    }
};

const TerminateBacktraceInstaller terminate_backtrace_installer;

// 固定测试种子与凭据材料:身份 Token、Queue Number v2、Admission Grant
// 各一把种子(角色不共材,Queue 侧 validate() 会拒绝共享签名密钥),外加
// Grant 公钥(网关键环按 kid 索引)与准入消费摘要键。测试侧用同一份构造
// 各 codec 做验签断言;外部服务组模式由 scripts/dev_services_test.sh 注入
// 同一组值,改动必须两边同步。
constexpr std::string_view kTestSeedHex =
    "0102030405060708090a0b0c0d0e0f10"
    "1112131415161718191a1b1c1d1e1f20";
constexpr std::string_view kQueueNumberSeedHex = kTestSeedHex;
constexpr std::string_view kGrantSeedHex =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr std::string_view kGrantPublicKeyHex =
    "207a067892821e25d770f1fba0c47c11ff4b813e54162ece9eb839e076231ab6";
constexpr std::string_view kConsumptionDigestKeyHex =
    "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
constexpr std::string_view kQueueNumberKid = "queue-number-v2";
constexpr std::string_view kGrantKid = "admission-grant-v1";

/// TLS 证书/会话票据/签名种子与准入材料的环境变量守护:指向 CMake 预生成
/// 的自签证书与固定测试密钥,析构还原。
class ScopedLoadgenEnvironment final {
public:
    ScopedLoadgenEnvironment() {
        EXPECT_EQ(
            ::setenv(
                "REALMMESH_TLS_CERTIFICATE_FILE",
                REALMMESH_TEST_TLS_CERTIFICATE,
                1),
            0);
        EXPECT_EQ(
            ::setenv(
                "REALMMESH_TLS_PRIVATE_KEY_FILE",
                REALMMESH_TEST_TLS_PRIVATE_KEY,
                1),
            0);
        EXPECT_EQ(
            ::setenv(
                "REALMMESH_SESSION_TICKET_KEY",
                "0102030405060708090a0b0c0d0e0f10"
                "1112131415161718191a1b1c1d1e1f20",
                1),
            0);
        EXPECT_EQ(
            ::setenv("REALMMESH_IDENTITY_KEY_SEED", kTestSeedHex.data(), 1),
            0);
        EXPECT_EQ(
            ::setenv(
                "REALMMESH_QUEUE_NUMBER_KEY_SEED",
                kQueueNumberSeedHex.data(),
                1),
            0);
        EXPECT_EQ(
            ::setenv(
                "REALMMESH_ADMISSION_GRANT_KEY_SEED", kGrantSeedHex.data(), 1),
            0);
        EXPECT_EQ(
            ::setenv(
                "REALMMESH_ADMISSION_GRANT_PUBLIC_KEY",
                kGrantPublicKeyHex.data(),
                1),
            0);
        EXPECT_EQ(
            ::setenv(
                "REALMMESH_ADMISSION_CONSUMPTION_DIGEST_KEY",
                kConsumptionDigestKeyHex.data(),
                1),
            0);
    }
    ~ScopedLoadgenEnvironment() {
        static_cast<void>(::unsetenv("REALMMESH_TLS_CERTIFICATE_FILE"));
        static_cast<void>(::unsetenv("REALMMESH_TLS_PRIVATE_KEY_FILE"));
        static_cast<void>(::unsetenv("REALMMESH_SESSION_TICKET_KEY"));
        static_cast<void>(::unsetenv("REALMMESH_IDENTITY_KEY_SEED"));
        static_cast<void>(::unsetenv("REALMMESH_QUEUE_NUMBER_KEY_SEED"));
        static_cast<void>(::unsetenv("REALMMESH_ADMISSION_GRANT_KEY_SEED"));
        static_cast<void>(::unsetenv("REALMMESH_ADMISSION_GRANT_PUBLIC_KEY"));
        static_cast<void>(
            ::unsetenv("REALMMESH_ADMISSION_CONSUMPTION_DIGEST_KEY"));
    }
};

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

[[nodiscard]] bool write_file(
    const std::filesystem::path& path, std::string_view contents) {
    std::ofstream output(path, std::ios::trunc);
    output << contents;
    return static_cast<bool>(output);
}

[[nodiscard]] std::string replace_all(
    std::string contents, std::string_view from, std::string_view to) {
    for (auto position = contents.find(from); position != std::string::npos;
         position = contents.find(from, position + to.size())) {
        contents.replace(position, from.size(), to);
    }
    return contents;
}

/// 同时打开 count 个监听套接字后再取端口,保证端口互不相同(逐个取时
/// 操作系统可能把刚释放的同一个临时端口再分回来)。
[[nodiscard]] std::vector<std::uint16_t> unused_tcp_ports(std::size_t count) {
    std::vector<std::unique_ptr<network::TcpListener>> listeners;
    std::vector<std::uint16_t> ports;
    listeners.reserve(count);
    ports.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        listeners.push_back(
            std::make_unique<network::TcpListener>("127.0.0.1", 0));
        ports.push_back(listeners.back()->local_port());
    }
    return ports;
}

/// 单连接测试代理:先接住客户端 TCP,延迟后再连真实 Gateway 并双向转发。
/// 延迟落在 TLS 握手之前,因此确定属于 dial 而非 attach。
class DelayedTcpProxy final {
public:
    DelayedTcpProxy(
        std::uint16_t upstream_port, std::chrono::milliseconds delay)
        : listener_("127.0.0.1", 0), upstream_port_(upstream_port),
          delay_(delay), thread_([this] { run(); }) {}

    ~DelayedTcpProxy() {
        stopping_.store(true, std::memory_order_relaxed);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    DelayedTcpProxy(const DelayedTcpProxy&) = delete;
    DelayedTcpProxy& operator=(const DelayedTcpProxy&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept {
        return listener_.local_port();
    }

private:
    static bool send_all(int descriptor, const char* data, std::size_t size) {
        std::size_t sent = 0;
        while (sent < size) {
            const auto written = ::send(descriptor, data + sent, size - sent, 0);
            if (written > 0) {
                sent += static_cast<std::size_t>(written);
                continue;
            }
            if (written < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        return true;
    }

    void relay(int client, int upstream) {
        std::array<char, 4096> buffer{};
        std::array<pollfd, 2> descriptors{{
            {.fd = client, .events = POLLIN, .revents = 0},
            {.fd = upstream, .events = POLLIN, .revents = 0},
        }};
        while (!stopping_.load(std::memory_order_relaxed)) {
            const int ready = ::poll(descriptors.data(), descriptors.size(), 20);
            if (ready < 0) {
                if (errno == EINTR) continue;
                return;
            }
            if (ready == 0) continue;
            for (std::size_t index = 0; index < descriptors.size(); ++index) {
                const auto events = descriptors[index].revents;
                if ((events & POLLIN) != 0) {
                    const int source = descriptors[index].fd;
                    const int destination = descriptors[1U - index].fd;
                    const auto count = ::recv(
                        source, buffer.data(), buffer.size(), 0);
                    if (count <= 0 ||
                        !send_all(destination, buffer.data(),
                                  static_cast<std::size_t>(count))) {
                        return;
                    }
                } else if ((events & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                    return;
                }
            }
        }
    }

    void run() {
        std::optional<network::TcpSocket> client;
        while (!stopping_.load(std::memory_order_relaxed) &&
               !client.has_value()) {
            client = listener_.accept();
            if (!client.has_value()) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        }
        if (!client.has_value()) return;

        std::this_thread::sleep_for(delay_);
        const int upstream = ::socket(AF_INET, SOCK_STREAM, 0);
        if (upstream < 0) return;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(upstream_port_);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(
                upstream, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == 0) {
            relay(client->native_handle(), upstream);
        }
        ::close(upstream);
    }

    network::TcpListener listener_;
    std::uint16_t upstream_port_{0};
    std::chrono::milliseconds delay_{0};
    std::atomic_bool stopping_{false};
    std::thread thread_;
};

/// 拷贝真实 configs 到临时目录并确保服务发现关闭(本环境无 etcd)。
[[nodiscard]] bool copy_configs_with_discovery_disabled(
    const std::filesystem::path& source, const std::filesystem::path& target) {
    std::error_code error;
    std::filesystem::create_directories(target / "common", error);
    if (error) return false;
    std::filesystem::create_directories(target / "services", error);
    if (error) return false;
    std::filesystem::copy(
        source / "common",
        target / "common",
        std::filesystem::copy_options::recursive,
        error);
    if (error) return false;
    std::filesystem::copy(
        source / "services",
        target / "services",
        std::filesystem::copy_options::recursive,
        error);
    if (error) return false;
    std::filesystem::copy_file(
        source / "main.config", target / "main.config", error);
    if (error) return false;

    auto contents = read_file(target / "common" / "discovery.lua");
    constexpr std::string_view enabled_true = "enabled = true";
    for (auto position = contents.find(enabled_true);
         position != std::string::npos;
         position = contents.find(enabled_true)) {
        contents.replace(position, enabled_true.size(), "enabled = false");
    }
    if (contents.find("enabled = false") == std::string::npos) return false;
    return write_file(target / "common" / "discovery.lua", contents);
}

/// 生成压测账号表:count 个白名单机器人账号(credential 与 loadgen
/// 默认一致;account_id 缺省按账号名派生)。账号经 player_data 空库导入
/// 进入用例独占的 MongoDB 库(ADR-0011);机器人口令以最低 Argon2 成本
/// 哈希,否则万级账号导入与单线程验票会把用例时长变成口令哈希的耗时
/// 而非登录链路本身。
void write_robot_accounts(
    const std::filesystem::path& root, std::size_t count) {
    const auto& mongod = test_support::MongodProcess::shared();
    ASSERT_TRUE(write_file(
        root / "common" / "player_data.lua",
        "return {\n    player_data = {\n"
        "        uri = \"" + mongod.uri() + "\",\n"
        "        database = \"" + test_support::MongodProcess::fresh_database() +
            "\",\n"
        "        bootstrap_accounts_file = \"common/accounts.lua\",\n"
        "        credential_hash_cost = \"minimum\",\n    },\n}\n"));
    std::string contents;
    contents.reserve(96 * count + 128);
    contents += "-- 压测账号集(loadgen 集成测试生成):白名单机器人账号。\n";
    contents += "return {\n    accounts = {\n";
    for (std::size_t index = 0; index < count; ++index) {
        contents += "        { account = \"robot-" + std::to_string(index) +
                    "\", credential = \"loadgen-credential\", "
                    "whitelisted = true },\n";
    }
    contents += "    },\n}\n";
    ASSERT_TRUE(write_file(root / "common" / "accounts.lua", contents));
}

/// base64(RFC 4648):fake etcd 的 KV 编解码(与 queue_store 内助手同
/// 语义,注册中心助手未导出,测试内自备)。
[[nodiscard]] std::string base64_encode_text(std::string_view input) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((input.size() + 2U) / 3U) * 4U);
    for (std::size_t offset = 0; offset < input.size(); offset += 3U) {
        const auto first = static_cast<std::uint32_t>(input[offset]);
        const auto second = offset + 1U < input.size()
                                ? static_cast<std::uint32_t>(input[offset + 1U])
                                : 0U;
        const auto third = offset + 2U < input.size()
                               ? static_cast<std::uint32_t>(input[offset + 2U])
                               : 0U;
        const std::uint32_t value = (first << 16U) | (second << 8U) | third;
        output.push_back(alphabet[(value >> 18U) & 0x3FU]);
        output.push_back(alphabet[(value >> 12U) & 0x3FU]);
        output.push_back(
            offset + 1U < input.size() ? alphabet[(value >> 6U) & 0x3FU] : '=');
        output.push_back(
            offset + 2U < input.size() ? alphabet[(value >> 0U) & 0x3FU] : '=');
    }
    return output;
}


/// 用例自带的真实单节点 etcd:网关的准入消费存储要求线性一致 CAS
/// (ADR-0009),进程内假 KV 证明不了这条契约,而"用假替身证明外部系统的
/// 契约"会让缺陷无法归因。放行阀门的额度键没有预算 publisher 供血(发现
/// 关闭的拓扑),由夹具在启动时写进同一实例。
class TestEtcd final {
public:
    TestEtcd()
        : endpoint_(process_.endpoint()) {
        process_.wait_ready();
        const auto client = cluster::make_etcd_http_client(
            endpoint_, std::chrono::milliseconds{500});
        // 客户端端口可连 ≠ 单节点 etcd 已能服务:选举窗口内它会直接关闭
        // 连接(实测 "Failed to read connection")。用真实写入做就绪探测,
        // 而不是靠 sleep 猜;超时即抛错,绝不带着空额度继续往下跑——否则
        // 症状会漂成"机器人全部超时",与根因相距甚远。
        wait_accepting_writes(*client);
        seed(
            *client,
            "/realmmesh/budgets/service/gateway/gateway-dev-01",
            R"({"conn_free":100000,"fetch_free":100000})");
        seed(
            *client,
            "/realmmesh/budgets/service/realm/realm-dev-01",
            R"({"conn_free":100000})");
    }

    TestEtcd(const TestEtcd&) = delete;
    TestEtcd& operator=(const TestEtcd&) = delete;

    [[nodiscard]] const std::string& endpoint() const noexcept {
        return endpoint_;
    }

    void pause() { process_.pause(); }
    void resume() {
        process_.resume();
        process_.wait_ready();
    }

private:
    static bool write_key(
        cluster::IEtcdHttpClient& client,
        std::string_view key,
        std::string_view value,
        std::string* error) {
        const auto response = client.post(
            "/v3/kv/put",
            nlohmann::json{{"key", base64_encode_text(key)},
                           {"value", base64_encode_text(value)}}.dump(),
            error);
        return response.has_value() &&
            response->find("\"error\"") == std::string::npos;
    }

    static void wait_accepting_writes(cluster::IEtcdHttpClient& client) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds{15};
        std::string error;
        while (std::chrono::steady_clock::now() < deadline) {
            if (write_key(client, "/realmmesh/test/readiness", "1", &error)) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
        throw std::runtime_error("etcd did not accept writes: " + error);
    }

    static void seed(
        cluster::IEtcdHttpClient& client,
        std::string_view key,
        std::string_view value) {
        std::string error;
        if (!write_key(client, key, value, &error)) {
            throw std::runtime_error(
                "failed to seed etcd key " + std::string{key} + ": " + error);
        }
    }

    test_support::EtcdProcess process_;
    std::string endpoint_;
};

/// 改写 scratch 配置端口:
/// - login_verify/queue 的 listen_port = 0 是内核分配(开发语义);压测
///   机器人须拨显式端口,改为测试预选的空闲端口(部署语义:显式指定)。
/// - queue etcd_endpoint 指向夹具的 fake etcd(额度阀门供血)。
/// - queue release_step 抬到 100000:即时放行,把 CI 缩减档的机器人
///   时间花在链路本身而不是等放行节拍。
/// - gateway listen_port = 8000 出现两次(TCP 与伴随传输),都指向同一
///   空闲端口;downstream 是静态兜底出向端点(1303 grant 报文用,机器
///   人不拨它);handoff_grace 抬长使 soak 保持段的会话不被宽限回收。
/// - relax_ingress_limits:把 credential_ingress 的单来源速率/突发抬到
///   合成负载之上(仍在协议硬上限内)。默认 20/s + burst 40 是**真实的
///   生产护栏**,单来源的机器人负载(同一 127.0.0.1)必须先越过它才谈
///   得上测水位;不抬的话压测测的是限流器而不是网关管线(#88)。
void use_loadgen_free_ports(
    const std::filesystem::path& root,
    std::uint16_t login_verify_port,
    std::uint16_t queue_port,
    std::uint16_t gateway_port,
    std::uint16_t grant_endpoint_port,
    std::string_view etcd_endpoint,
    bool fast_release,
    bool fast_release_frames,
    bool long_handoff_grace,
    bool relax_ingress_limits = false) {
    const auto login_verify_path = root / "services" / "login_verify.lua";
    auto login_verify = read_file(login_verify_path);
    login_verify = replace_all(
        login_verify,
        "listen_port = 0,",
        "listen_port = " + std::to_string(login_verify_port) + ",");
    login_verify =
        replace_all(login_verify, "metrics_port = 9104", "metrics_port = 0");
    ASSERT_TRUE(write_file(login_verify_path, login_verify));

    const auto queue_path = root / "services" / "queue.lua";
    auto queue = read_file(queue_path);
    queue = replace_all(
        queue,
        "listen_port = 0,",
        "listen_port = " + std::to_string(queue_port) + ",");
    queue = replace_all(queue, "metrics_port = 9105", "metrics_port = 0");
    if (!etcd_endpoint.empty()) {
        queue = replace_all(
            queue, "http://127.0.0.1:2379", std::string{etcd_endpoint});
    }
    if (fast_release) {
        queue = replace_all(queue, "release_step = 3000",
                            "release_step = 100000");
    }
    if (fast_release_frames) {
        // 放行帧收紧到 1s(解析走整数秒):轮询机器人的 admission 等待
        // ≈ 一个放行帧间隔,收紧帧间隔即"放行节拍更快的部署档"建模。
        queue = replace_all(queue, "release_interval_seconds = 2",
                            "release_interval_seconds = 1");
    }
    ASSERT_TRUE(write_file(queue_path, queue));

    const auto gateway_path = root / "services" / "gateway.lua";
    auto gateway = read_file(gateway_path);
    gateway = replace_all(
        gateway,
        "listen_port = 8000",
        "listen_port = " + std::to_string(gateway_port));
    gateway =
        replace_all(gateway, "metrics_port = 9103", "metrics_port = 0");
    gateway = replace_all(
        gateway,
        "downstream_port = 7100",
        "downstream_port = " + std::to_string(grant_endpoint_port));
    if (long_handoff_grace) {
        gateway = replace_all(gateway, "handoff_grace_ms = 5000",
                              "handoff_grace_ms = 30000");
    }
    if (relax_ingress_limits) {
        // 硬上限:rate ≤ 10000、burst ≤ 20000(gateway_ingress.hpp)。
        gateway = replace_all(
            gateway, "source_rate_per_second = 20",
            "source_rate_per_second = 4000");
        gateway = replace_all(
            gateway, "source_burst = 40", "source_burst = 4000");
        gateway = replace_all(
            gateway,
            "source_throttle_close_after = 3",
            "source_throttle_close_after = 8");
    }
    ASSERT_TRUE(write_file(gateway_path, gateway));

    // 网关的准入消费存储(ADR-0009)与 realm/queue 的服务发现共用
    // common/discovery.lua 的 endpoint:必须换成用例自己的 etcd,否则会
    // 落到开发者 127.0.0.1:2379 上,消费记录跨用例存活。
    if (!etcd_endpoint.empty()) {
        const auto discovery_path = root / "common" / "discovery.lua";
        auto discovery = read_file(discovery_path);
        discovery = replace_all(
            discovery, "http://127.0.0.1:2379", std::string{etcd_endpoint});
        ASSERT_TRUE(write_file(discovery_path, discovery));
    }

    if (grant_endpoint_port != 0) {
        const auto realm_path = root / "services" / "realm.lua";
        auto realm = read_file(realm_path);
        realm = replace_all(
            realm,
            "listen_port = 7100",
            "listen_port = " + std::to_string(grant_endpoint_port));
        realm = replace_all(realm, "metrics_port = 9102", "metrics_port = 0");
        ASSERT_TRUE(write_file(realm_path, realm));
    }
}

/// MeshHost 不自转线程(帧尾指标发布与 HTTPS poll 循环都靠外部 tick
/// 驱动);测试期间 2ms 节拍持续驱动。tick 线程里的异常不允许逃逸成
/// 裸 std::terminate(CI 上缓冲的日志会随 abort 丢光):捕获后落
/// unbuffered stderr 并停摆驱动,让后续断言失败时还能看到 what()。
class TickDriver final {
public:
    /// after_tick 在每次 mesh.tick() 之后、同一线程上调用:网关水位 gauge
    /// 只在 tick 内的 advance() 里发布,这里观察能看到每一个发布过的状态。
    explicit TickDriver(
        service_host::MeshHost& mesh,
        std::function<void()> after_tick = {})
        : thread_([this, &mesh, after_tick = std::move(after_tick)] {
              try {
                  while (running_.load(std::memory_order_relaxed)) {
                      mesh.tick();
                      if (after_tick) {
                          after_tick();
                      }
                      std::this_thread::sleep_for(
                          std::chrono::milliseconds{2});
                  }
              } catch (const std::exception& error) {
                  std::fprintf(
                      stderr, "tick thread exception: %s\n", error.what());
                  std::fflush(stderr);
                  running_.store(false, std::memory_order_relaxed);
              }
          }) {}
    ~TickDriver() {
        running_.store(false, std::memory_order_relaxed);
        thread_.join();
    }

    TickDriver(const TickDriver&) = delete;
    TickDriver& operator=(const TickDriver&) = delete;

private:
    std::atomic_bool running_{true};
    std::thread thread_;
};

[[nodiscard]] LoadgenEndpoints loadgen_endpoints(
    std::uint16_t login_verify_port,
    std::uint16_t queue_port,
    std::uint16_t gateway_port) {
    LoadgenEndpoints endpoints;
    endpoints.login_verify = ServiceAddress{"127.0.0.1", login_verify_port};
    endpoints.queue = ServiceAddress{"127.0.0.1", queue_port};
    endpoints.gateway = ServiceAddress{"127.0.0.1", gateway_port};
    return endpoints;
}

[[nodiscard]] MetricsSnapshot parse_metrics_text(std::string text) {
    MetricsSnapshot snapshot;
    while (true) {
        const auto position = text.find('\n');
        if (position == std::string::npos) {
            parse_metrics_line(text, snapshot);
            return snapshot;
        }
        parse_metrics_line(std::string_view{text}.substr(0, position), snapshot);
        text.erase(0, position + 1);
    }
}

/// 取带标签序列值(名 + 标签子串匹配);序列缺席视为 0(gauge 在帧尾
/// 发布后恒在,缺席只在帧尾未跑过时出现)。
[[nodiscard]] double series_value(
    const MetricsSnapshot& snapshot, std::string_view key) {
    const auto found = std::find_if(
        snapshot.series.begin(),
        snapshot.series.end(),
        [key](const auto& entry) {
            return entry.first.find(key) != std::string::npos;
        });
    return found == snapshot.series.end() ? 0 : found->second;
}

/// 对端端口是 port 的已连接 TCP socket(IPv4/IPv6)。
[[nodiscard]] bool connected_to_port(int descriptor, std::uint16_t port) {
    sockaddr_storage peer{};
    socklen_t length = sizeof(peer);
    if (::getpeername(
            descriptor, reinterpret_cast<sockaddr*>(&peer), &length) != 0) {
        return false;
    }
    if (peer.ss_family == AF_INET) {
        return ntohs(reinterpret_cast<const sockaddr_in&>(peer).sin_port) ==
               port;
    }
    if (peer.ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<const sockaddr_in6&>(peer).sin6_port) ==
               port;
    }
    return false;
}

/// 进程当前打开的 fd 数(0..rlim_cur 逐个 F_GETFD):fd 不泄漏断言的
/// 探针。基线口径两次一致即可,不求绝对完备。连到 excluded_peer_port
/// 的 socket 不计:MongoDB 连接池按并发需求懒增长并常驻,上限由
/// maxPoolSize 封顶,不随会话数增长;CPU 紧张时预热撑不满池,主跑补
/// 上的那条连接会被误判为泄漏(#117)。
[[nodiscard]] std::uint64_t count_open_fds(std::uint16_t excluded_peer_port) {
    rlimit limits{};
    rlim_t ceiling = 1024;
    if (::getrlimit(RLIMIT_NOFILE, &limits) == 0) {
        ceiling = std::min<rlim_t>(limits.rlim_cur, 65536);
    } else {
        ADD_FAILURE();
    }
    std::uint64_t count = 0;
    for (int descriptor = 0; descriptor < static_cast<int>(ceiling);
         ++descriptor) {
        if (::fcntl(descriptor, F_GETFD) != -1 &&
            !connected_to_port(descriptor, excluded_peer_port)) {
            ++count;
        }
    }
    return count;
}

struct WaterSample final {
    double pending{0};
    double fetching{0};
    double handed_off{0};
    double conn_free{0};
};

[[nodiscard]] WaterSample sample_water(const MetricsSnapshot& snapshot) {
    return WaterSample{
        .pending =
            series_value(snapshot, "edge_sessions{stage=\"pending\"}"),
        .fetching =
            series_value(snapshot, "edge_sessions{stage=\"fetching\"}"),
        .handed_off =
            series_value(snapshot, "edge_sessions{stage=\"handed_off\"}"),
        .conn_free =
            series_value(snapshot, "edge_budget{kind=\"conn_free\"}"),
    };
}

/// L1 verify 定向:200 机器人打满 login_verify,断言零失败、计数一致、
/// 宽松吞吐下限。CI 缩减档 ≤ 20s(本地参考:数百请求/秒;下限 10/秒
/// 只是数量级守门,防服务端空转或链路断裂)。
TEST(LoadgenIntegrationTest, L1VerifyDirectsTrafficAndCountersAgree) {
    const ScopedLoadgenEnvironment environment;
    const std::filesystem::path source = REALMMESH_SOURCE_DIR "/configs";
    const test_support::TemporaryDirectory scratch("loadgen-it-verify-");
    ASSERT_TRUE(copy_configs_with_discovery_disabled(source, scratch.path()));
    const auto ports = unused_tcp_ports(1);
    const auto login_verify_port = ports.at(0);
    use_loadgen_free_ports(
        scratch.path(),
        login_verify_port, 0, 0, 0, std::string_view{}, false, false, false);
    write_robot_accounts(scratch.path(), 200);

    service_host::MeshHost mesh(
        scratch.path(),
        {{"login_verify", {}, false}});
    ASSERT_TRUE(mesh.start_all());
    const TickDriver driver(mesh);

    LoadgenConfig config;
    config.target = LoadgenLoginTarget::Verify;
    config.robots = 200;
    config.concurrency = 50;
    config.duration_seconds = 20;
    config.endpoints = loadgen_endpoints(login_verify_port, 0, 0);

    const auto started = std::chrono::steady_clock::now();
    const auto report = run_loadgen(config);
    const auto wall = std::chrono::steady_clock::now() - started;

    EXPECT_EQ(report.completed, 200);
    EXPECT_EQ(report.verify.attempts, 200);
    EXPECT_EQ(report.verify.failures, 0);
    EXPECT_EQ(report.tickets.attempts, 0);
    EXPECT_EQ(report.poll.attempts, 0);
    EXPECT_EQ(report.attach.attempts, 0);
    EXPECT_EQ(report.handoff.attempts, 0);
    // 宽松吞吐下限:200 请求 20s 内完成即 ≥ 10/秒。
    EXPECT_LT(wall, std::chrono::seconds{20});

    // 服务侧计数一致(#34 口径):verify_requests_total == 机器人数。
    const auto metrics =
        parse_metrics_text(mesh.service("login_verify").prometheus_metrics());
    EXPECT_EQ(metrics.total("verify_requests_total"), 200);
}

/// L1 取号定向:300 机器人走 verify → tickets,断言零失败、签发计数
/// 一致、号码牌全部验签通过(spec:号牌须能被同源键校验)。
TEST(LoadgenIntegrationTest, L1TicketsIssueTokensAllValidate) {
    const ScopedLoadgenEnvironment environment;
    const std::filesystem::path source = REALMMESH_SOURCE_DIR "/configs";
    const test_support::TemporaryDirectory scratch("loadgen-it-tickets-");
    ASSERT_TRUE(copy_configs_with_discovery_disabled(source, scratch.path()));
    const auto ports = unused_tcp_ports(2);
    const auto login_verify_port = ports.at(0);
    const auto queue_port = ports.at(1);
    TestEtcd etcd;
    use_loadgen_free_ports(
        scratch.path(), login_verify_port, queue_port, 0, 0, etcd.endpoint(),
        true, false, false);
    write_robot_accounts(scratch.path(), 300);

    service_host::MeshHost mesh(
        scratch.path(),
        {{"login_verify", {}, false}, {"queue", {}, false}});
    ASSERT_TRUE(mesh.start_all());
    const TickDriver driver(mesh);

    LoadgenConfig config;
    config.target = LoadgenLoginTarget::Tickets;
    config.robots = 300;
    config.concurrency = 60;
    config.duration_seconds = 20;
    config.collect_number_tokens = true;
    config.endpoints = loadgen_endpoints(login_verify_port, queue_port, 0);

    const auto started = std::chrono::steady_clock::now();
    const auto report = run_loadgen(config);
    const auto wall_seconds = std::chrono::duration<double>(
                                  std::chrono::steady_clock::now() - started)
                                  .count();

    EXPECT_EQ(report.completed, 300);
    EXPECT_EQ(report.verify.attempts, 300);
    EXPECT_EQ(report.tickets.attempts, 300);
    EXPECT_EQ(report.tickets.failures, 0);
    EXPECT_EQ(report.poll.attempts, 0);
    EXPECT_EQ(report.attach.attempts, 0);
    EXPECT_EQ(report.handoff.attempts, 0);
    // 吞吐下限(spec L1):300 号牌 30s 上限 ≈ 10/s 数量级守门;本机
    // 参考亚秒。压在计数断言之后,失败时输出顺序不误导。
    EXPECT_LT(wall_seconds, 30);

    const auto metrics =
        parse_metrics_text(mesh.service("queue").prometheus_metrics());
    EXPECT_EQ(metrics.total("tickets_issued_total"), 300);

    // 号牌验签:取号接口签发的是 Queue Number v2,kid 与 queue.lua 的
    // queue_number_kid 一致;测试侧用同源种子构造签发方同款 codec。v2 与
    // v1 的关键差别是身份绑定——载荷必须带 identity_jti(#79),而准入
    // 凭据另有独立的 Admission Grant(由网关验证,不在这里)。
    const auto queue_number_seed =
        common::parse_identity_seed_hex(kQueueNumberSeedHex);
    common::QueueNumberV2Codec codec(
        common::QueueNumberV2SigningKey{
            .kid = std::string{kQueueNumberKid}, .seed = queue_number_seed},
        {{.kid = std::string{kQueueNumberKid},
          .public_key =
              common::ed25519_public_key_from_seed(queue_number_seed)}},
        std::chrono::seconds{3600});
    const auto now = std::chrono::system_clock::now();
    ASSERT_EQ(report.number_tokens.size(), 300);
    for (const auto& token : report.number_tokens) {
        const auto claims = codec.validate(token, now);
        ASSERT_TRUE(claims.has_value());
        EXPECT_GE(claims->number, 1);
        // v2 的身份绑定:jti 必须是 32 位小写 hex(v1 载荷根本没有它)。
        EXPECT_EQ(claims->identity_jti.size(), 32U);
        EXPECT_TRUE(std::ranges::all_of(
            claims->identity_jti, [](char character) {
                return (character >= '0' && character <= '9') ||
                    (character >= 'a' && character <= 'f');
            }));
    }
}

/// L1 网关 soak 缩减版:预热 → fd 基线 → 基线跑(attach p50 对照)→
/// 主跑(100 机器人 handed-off 保持水位,150ms 采样)→ 排空 → fd 回归。
/// 断言:三段水位快照与额度账自洽(段和 + conn_free 恒等管线容量)、
/// handed-off 水位真实存在、attach 时延与边缘拉取时延(edge_fetch 增量
/// 均值)相对自身基线均无漂移、fd 不泄漏。
TEST(LoadgenIntegrationTest, L1GatewaySoakHoldsWaterLevelWithoutFdLeak) {
    const ScopedLoadgenEnvironment environment;
    const std::filesystem::path source = REALMMESH_SOURCE_DIR "/configs";
    const test_support::TemporaryDirectory scratch("loadgen-it-soak-");
    ASSERT_TRUE(copy_configs_with_discovery_disabled(source, scratch.path()));
    const auto ports = unused_tcp_ports(4);
    const auto login_verify_port = ports.at(0);
    const auto queue_port = ports.at(1);
    const auto gateway_port = ports.at(2);
    TestEtcd etcd;
    use_loadgen_free_ports(
        scratch.path(), login_verify_port, queue_port, gateway_port,
        ports.at(3), etcd.endpoint(), true, false, true,
        /*relax_ingress_limits=*/true);
    write_robot_accounts(scratch.path(), 150);

    service_host::MeshHost mesh(
        scratch.path(),
        {{"login_verify", {}, false},
         {"queue", {}, false},
         {"gateway", {"login_verify"}, true}});
    ASSERT_TRUE(mesh.start_all());
    // 水位采样挂在 tick 线程上:每次 advance() 发布后取一次快照,不会像
    // 独立定时线程那样在慢机器上整段错过阶段重叠。基线跑与主跑都采样,
    // 采样开销两边相同,不污染时延对照;断言只看主跑的样本。
    std::vector<WaterSample> samples;
    std::mutex samples_mutex;
    std::atomic_bool sampling{false};
    const TickDriver driver(mesh, [&] {
        if (!sampling.load(std::memory_order_relaxed)) {
            return;
        }
        auto snapshot = sample_water(parse_metrics_text(
            mesh.service("gateway").prometheus_metrics()));
        std::scoped_lock lock{samples_mutex};
        samples.push_back(snapshot);
    });
    const auto endpoints =
        loadgen_endpoints(login_verify_port, queue_port, gateway_port);

    // 预热:吸收一次性开销(OpenSSL/Lua/日志句柄),fd 基线从这之后取。
    // MongoDB 连接池的增长不计入 fd 探针(见 count_open_fds)。
    constexpr std::uint64_t warmup_robots = 16;
    LoadgenConfig warmup;
    warmup.target = LoadgenLoginTarget::Gateway;
    warmup.robots = warmup_robots;
    warmup.concurrency = warmup_robots;
    warmup.duration_seconds = 5;
    warmup.poll_interval = std::chrono::milliseconds{50};
    warmup.endpoints = endpoints;
    const auto warmup_report = run_loadgen(warmup);
    if (warmup_report.completed != warmup_robots) {
        std::cout << warmup_report.render();
    }
    EXPECT_EQ(warmup_report.completed, warmup_robots);
    // 预热会话排空后再取基线:在途关闭会把基线虚高,放过真泄漏。
    for (int attempt = 0; attempt < 150; ++attempt) {
        const auto snapshot = sample_water(parse_metrics_text(
            mesh.service("gateway").prometheus_metrics()));
        if (snapshot.handed_off == 0 && snapshot.pending == 0 &&
            snapshot.fetching == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    const auto mongod_port = test_support::MongodProcess::shared().port();
    const auto fd_before = count_open_fds(mongod_port);

    // 基线跑:取 attach p99 + 服务侧 fetch 均值,作无漂移对照。
    // 对照口径必须与主跑**同并发**:准入现在是一次真实存储往返(etcd
    // 线性一致消费记录,ADR-0009),attach 时延随并发排队增长是预期行为,
    // 把 30 并发的 p99 拿去和 100 并发的 p99 比,量的是负载曲线而不是
    // 漂移。同并发对照仍能抓住"病态劣化"(5 倍守门不变)。
    sampling.store(true, std::memory_order_relaxed);
    LoadgenConfig baseline;
    baseline.target = LoadgenLoginTarget::GatewaySoak;
    baseline.robots = 100;
    baseline.concurrency = 100;
    baseline.duration_seconds = 5;
    baseline.poll_interval = std::chrono::milliseconds{50};
    baseline.endpoints = endpoints;
    const auto base_report = run_loadgen(baseline);
    const auto base_latency = base_report.attach.latency.summary();
    if (base_report.completed != 100) {
        std::cout << base_report.render();
    }
    ASSERT_EQ(base_report.completed, 100);
    ASSERT_EQ(base_report.attach.failures, 0);
    ASSERT_EQ(base_report.handoff.failures, 0);
    ASSERT_EQ(base_report.attach.latency.samples(),
              base_report.attach.attempts);
    ASSERT_GT(base_report.attach.latency.samples(), 0);
    const auto base_metrics =
        parse_metrics_text(mesh.service("gateway").prometheus_metrics());
    const auto base_fetch_sum =
        base_metrics.total("edge_fetch_duration_seconds_sum");
    const auto base_fetch_count =
        base_metrics.total("edge_fetch_duration_seconds_count");

    {
        std::scoped_lock lock{samples_mutex};
        samples.clear();
    }

    LoadgenConfig main_run;
    main_run.target = LoadgenLoginTarget::GatewaySoak;
    main_run.robots = 100;
    main_run.concurrency = 100;
    main_run.duration_seconds = 5;
    main_run.poll_interval = std::chrono::milliseconds{50};
    main_run.endpoints = endpoints;
    const auto report = run_loadgen(main_run);
    sampling.store(false, std::memory_order_relaxed);
    std::vector<WaterSample> main_samples;
    {
        std::scoped_lock lock{samples_mutex};
        main_samples = std::move(samples);
    }

    EXPECT_EQ(report.completed, 100);
    EXPECT_EQ(report.attach.failures, 0);
    EXPECT_EQ(report.handoff.failures, 0);
    EXPECT_EQ(report.attach.latency.samples(), report.attach.attempts);
    EXPECT_EQ(report.handoff.latency.samples(), report.handoff.attempts);

    // 水位断言:三段计数与额度账自洽(段和 + conn_free 恒等于管线连接
    // 容量,任何时刻快照都成立);handed-off 水位真实存在(hold 语义下
    // 100 个机器人全持,取 ≥ 50 宽松)。
    ASSERT_FALSE(main_samples.empty());
    double capacity = 0;
    double max_pending = 0;
    double max_fetching = 0;
    double max_handed_off = 0;
    bool saw_mixed_water = false;
    for (const auto& sample : main_samples) {
        const auto total =
            sample.pending + sample.fetching + sample.handed_off +
            sample.conn_free;
        if (capacity == 0) {
            capacity = total;
        }
        EXPECT_DOUBLE_EQ(total, capacity);
        max_pending = std::max(max_pending, sample.pending);
        max_fetching = std::max(max_fetching, sample.fetching);
        max_handed_off = std::max(max_handed_off, sample.handed_off);
        const auto active_stages = static_cast<int>(sample.pending > 0) +
                                   static_cast<int>(sample.fetching > 0) +
                                   static_cast<int>(sample.handed_off > 0);
        saw_mixed_water = saw_mixed_water || active_stages >= 2;
    }
    EXPECT_GT(capacity, 0);
    EXPECT_GT(max_fetching, 0);
    EXPECT_GE(max_handed_off, 50);
    EXPECT_TRUE(saw_mixed_water);

    // 时延无漂移:主跑 attach p99 相对自身基线不劣化超过 5 倍。1ms
    // 是计时精度下限,避免极快环境把 0ms 基线变成关闭门禁的特例。
    const auto main_latency = report.attach.latency.summary();
    const auto base_p99_gate = std::max(base_latency.p99_ms, 1.0);
    EXPECT_LT(main_latency.p99_ms, base_p99_gate * 5);

    // 服务侧拉取时延同口径对照(spec:soak 看的是服务时延,不只是客户
    // 端 attach 代理):主跑相对基线的 edge_fetch 增量均值不劣化超过
    // 5 倍 + 50ms 绝对余量(小样本噪声防护;增量口径排除基线摊薄)。
    const auto main_metrics =
        parse_metrics_text(mesh.service("gateway").prometheus_metrics());
    const auto main_fetch_sum =
        main_metrics.total("edge_fetch_duration_seconds_sum");
    const auto main_fetch_count =
        main_metrics.total("edge_fetch_duration_seconds_count");
    const auto fetch_delta_count = main_fetch_count - base_fetch_count;
    if (base_fetch_count > 0 && fetch_delta_count > 0) {
        const auto base_mean = base_fetch_sum / base_fetch_count;
        const auto main_mean =
            (main_fetch_sum - base_fetch_sum) / fetch_delta_count;
        EXPECT_LT(main_mean, base_mean * 5.0 + 0.05);
    }

    // 排空:机器人已断连,帧尾把会话清干净(宽限回收兜底 ≤ 30s,这里
    // 15s 内应到位);conn_free 回到满容量。
    bool drained = false;
    for (int attempt = 0; attempt < 150; ++attempt) {
        const auto snapshot = sample_water(parse_metrics_text(
            mesh.service("gateway").prometheus_metrics()));
        if (snapshot.handed_off == 0 && snapshot.pending == 0 &&
            snapshot.fetching == 0 && snapshot.conn_free == capacity) {
            drained = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    EXPECT_TRUE(drained);

    // fd 不泄漏:压测前后进程 fd 数只许回落不许增长(keep-alive 短连
    // 的关闭会让 fd 数合法减少,故查方向而非求等)。异步关闭有抖动
    // (对端 RST 的回收落在计数之后),给 2s 让在途关闭落定;真泄漏
    // 不会随等待消失。
    bool fds_settled = false;
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (count_open_fds(mongod_port) <= fd_before) {
            fds_settled = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    EXPECT_TRUE(fds_settled);
    const auto fd_after = count_open_fds(mongod_port);
    std::cout << "acceptance_m1 gateway_soak completed=" << report.completed
              << " attach_failures=" << report.attach.failures
              << " handoff_failures=" << report.handoff.failures
              << " capacity=" << capacity
              << " max_pending=" << max_pending
              << " max_fetching=" << max_fetching
              << " max_handed_off=" << max_handed_off
              << " mixed_water_sample=" << saw_mixed_water
              << " water_samples=" << main_samples.size()
              << " baseline_attach_p50_ms=" << base_latency.p50_ms
              << " main_attach_p50_ms=" << main_latency.p50_ms
              << " baseline_attach_p99_ms=" << base_latency.p99_ms
              << " main_attach_p99_ms=" << main_latency.p99_ms
              << " fd_before=" << fd_before
              << " fd_after=" << fd_after << '\n';
}

TEST(LoadgenIntegrationTest, AdapterAndDecoratorDriveRealGatewayChain) {
    const ScopedLoadgenEnvironment environment;
    const std::filesystem::path source = REALMMESH_SOURCE_DIR "/configs";
    const test_support::TemporaryDirectory scratch("loadgen-it-adapter-");
    ASSERT_TRUE(copy_configs_with_discovery_disabled(source, scratch.path()));
    const auto ports = unused_tcp_ports(4);
    TestEtcd etcd;
    use_loadgen_free_ports(
        scratch.path(), ports.at(0), ports.at(1), ports.at(2), ports.at(3),
        etcd.endpoint(), true, false, true);
    write_robot_accounts(scratch.path(), 1);

    service_host::MeshHost mesh(
        scratch.path(),
        {{"login_verify", {}, false},
         {"queue", {}, false},
         {"gateway", {"login_verify"}, true}});
    ASSERT_TRUE(mesh.start_all());
    const TickDriver driver(mesh);

    LoadgenLoginOptions options;
    options.target = LoadgenLoginTarget::Gateway;
    options.endpoints =
        loadgen_endpoints(ports.at(0), ports.at(1), ports.at(2));
    options.account = "robot-0";
    options.credential = "loadgen-credential";
    options.poll_interval = std::chrono::milliseconds{50};
    options.deadline = client::Clock::now() + std::chrono::seconds{10};

    auto adaptation = adapt_login_run(options);
    auto* adapted = std::get_if<AdaptedLoginRun>(&adaptation);
    ASSERT_NE(adapted, nullptr);

    LoadgenReport report;
    client::WireEnterRealmRedeemer redeemer;
    client::WireLoginTransport wire(
        adapted->wire, redeemer, adapted->transport);
    MetricsLoginChainTransport measured(
        wire,
        {report.verify, report.tickets, report.poll, report.attach,
         report.handoff, report.realm});
    client::LoginChain chain(measured, std::move(adapted->chain));
    const auto result = chain.run(std::move(adapted->run));

    ASSERT_TRUE(result.succeeded());
    ASSERT_NE(result.success(), nullptr);
    EXPECT_NE(std::get_if<client::GatewaySuccess>(result.success()), nullptr);
    EXPECT_EQ(report.verify.attempts, 1U);
    EXPECT_EQ(report.verify.failures, 0U);
    EXPECT_EQ(report.tickets.attempts, 1U);
    EXPECT_EQ(report.tickets.failures, 0U);
    EXPECT_GE(report.poll.attempts, 1U);
    EXPECT_EQ(report.poll.failures, 0U);
    EXPECT_EQ(report.attach.attempts, 1U);
    EXPECT_EQ(report.attach.failures, 0U);
    EXPECT_EQ(report.handoff.attempts, 1U);
    EXPECT_EQ(report.handoff.failures, 0U);
}

TEST(LoadgenIntegrationTest, FullTargetRedeemsRealmSession) {
    const ScopedLoadgenEnvironment environment;
    const std::filesystem::path source = REALMMESH_SOURCE_DIR "/configs";
    const test_support::TemporaryDirectory scratch("loadgen-it-full-");
    ASSERT_TRUE(copy_configs_with_discovery_disabled(source, scratch.path()));
    const auto ports = unused_tcp_ports(4);
    TestEtcd etcd;
    use_loadgen_free_ports(
        scratch.path(), ports.at(0), ports.at(1), ports.at(2), ports.at(3),
        etcd.endpoint(), true, false, true);
    write_robot_accounts(scratch.path(), 1);

    service_host::MeshHost mesh(
        scratch.path(),
        {{"login_verify", {}, false},
         {"queue", {}, false},
         {"realm", {}, false},
         {"gateway", {"login_verify", "realm"}, true}});
    ASSERT_TRUE(mesh.start_all());
    const TickDriver driver(mesh);

    LoadgenLoginOptions options;
    options.target = LoadgenLoginTarget::Full;
    options.endpoints =
        loadgen_endpoints(ports.at(0), ports.at(1), ports.at(2));
    options.account = "robot-0";
    options.credential = "loadgen-credential";
    options.poll_interval = std::chrono::milliseconds{50};
    options.deadline = client::Clock::now() + std::chrono::seconds{10};

    auto adaptation = adapt_login_run(options);
    auto* adapted = std::get_if<AdaptedLoginRun>(&adaptation);
    ASSERT_NE(adapted, nullptr);

    LoadgenReport report;
    report.robots = 1;
    client::WireEnterRealmRedeemer redeemer;
    client::WireLoginTransport wire(
        adapted->wire, redeemer, adapted->transport);
    MetricsLoginChainTransport measured(
        wire,
        {report.verify, report.tickets, report.poll, report.attach,
         report.handoff, report.realm});
    client::LoginChain chain(measured, std::move(adapted->chain));
    auto result = chain.run(std::move(adapted->run));
    ASSERT_TRUE(result.succeeded());
    auto* success = std::get_if<client::FullSuccess>(result.success());
    ASSERT_NE(success, nullptr);
    ASSERT_NE(success->session, nullptr);
    ASSERT_TRUE(success->session->heartbeat(
        client::Clock::now() + std::chrono::seconds{2}));
    success->session->close();
    EXPECT_FALSE(success->session->heartbeat(
        client::Clock::now() + std::chrono::milliseconds{20}));
    report.completed = 1;

    std::cout << report.render();

    EXPECT_EQ(report.completed, 1U);
    EXPECT_EQ(report.verify.failures, 0U);
    EXPECT_EQ(report.tickets.failures, 0U);
    EXPECT_EQ(report.poll.failures, 0U);
    EXPECT_EQ(report.attach.failures, 0U);
    EXPECT_EQ(report.handoff.failures, 0U);
    EXPECT_EQ(report.handoff.attempts, 1U);
    EXPECT_EQ(report.realm.failures, 0U);
    EXPECT_EQ(report.realm.attempts, 1U);
}

TEST(LoadgenIntegrationTest, GatewayReadinessRecoversAfterEtcdOutage) {
    const ScopedLoadgenEnvironment environment;
    const std::filesystem::path source = REALMMESH_SOURCE_DIR "/configs";
    const test_support::TemporaryDirectory scratch("loadgen-it-recovery-");
    ASSERT_TRUE(copy_configs_with_discovery_disabled(source, scratch.path()));
    const auto ports = unused_tcp_ports(4);
    TestEtcd etcd;
    use_loadgen_free_ports(
        scratch.path(), ports.at(0), ports.at(1), ports.at(2), ports.at(3),
        etcd.endpoint(), true, false, true);
    write_robot_accounts(scratch.path(), 1);

    service_host::MeshHost mesh(
        scratch.path(),
        {{"login_verify", {}, false},
         {"queue", {}, false},
         {"realm", {}, false},
         {"gateway", {"login_verify", "realm"}, true}});
    ASSERT_TRUE(mesh.start_all());
    const TickDriver driver(mesh);
    ASSERT_TRUE(mesh.service("gateway").ready());

    etcd.pause();
    bool unavailable = false;
    for (int attempt = 0; attempt < 60; ++attempt) {
        if (!mesh.service("gateway").ready()) {
            unavailable = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    ASSERT_TRUE(unavailable);

    etcd.resume();
    bool recovered = false;
    for (int attempt = 0; attempt < 80; ++attempt) {
        if (mesh.service("gateway").ready()) {
            recovered = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    ASSERT_TRUE(recovered);

    LoadgenConfig config;
    config.target = LoadgenLoginTarget::Full;
    config.robots = 1;
    config.concurrency = 1;
    config.duration_seconds = 10;
    config.poll_interval = std::chrono::milliseconds{50};
    config.endpoints =
        loadgen_endpoints(ports.at(0), ports.at(1), ports.at(2));
    const auto report = run_loadgen(config);

    EXPECT_EQ(report.completed, 1U);
    EXPECT_EQ(report.realm.attempts, 1U);
    EXPECT_EQ(report.realm.failures, 0U);
}

/// M2 缩减版:250 机器人单趟全链路(verify → handed-off),断言完成率
/// ≥ 99%、拉取失败率 < 1%(#34 口径 retry/(retry+count);延迟桩恒成
/// 功,失败率应为 0)。CI 缩减档 ≤ 25s。
TEST(LoadgenIntegrationTest, M2ReducedChainCompletesWithLowFetchFailure) {
    const ScopedLoadgenEnvironment environment;
    const std::filesystem::path source = REALMMESH_SOURCE_DIR "/configs";
    const test_support::TemporaryDirectory scratch("loadgen-it-m2-");
    ASSERT_TRUE(copy_configs_with_discovery_disabled(source, scratch.path()));
    const auto ports = unused_tcp_ports(4);
    const auto login_verify_port = ports.at(0);
    const auto queue_port = ports.at(1);
    const auto gateway_port = ports.at(2);
    TestEtcd etcd;
    use_loadgen_free_ports(
        scratch.path(), login_verify_port, queue_port, gateway_port,
        ports.at(3), etcd.endpoint(), true, false, false,
        /*relax_ingress_limits=*/true);
    write_robot_accounts(scratch.path(), 250);

    service_host::MeshHost mesh(
        scratch.path(),
        {{"login_verify", {}, false},
         {"queue", {}, false},
         {"gateway", {"login_verify"}, true}});
    ASSERT_TRUE(mesh.start_all());
    const TickDriver driver(mesh);

    LoadgenConfig config;
    config.target = LoadgenLoginTarget::Gateway;
    config.robots = 250;
    config.concurrency = 64;
    config.duration_seconds = 25;
    config.poll_interval = std::chrono::milliseconds{50};
    config.endpoints =
        loadgen_endpoints(login_verify_port, queue_port, gateway_port);

    const auto report = run_loadgen(config);
    if (report.completed < 248) {
        std::cout << report.render();
    }

    EXPECT_GE(report.completed, 248);  // 完成率 ≥ 99%。
    EXPECT_EQ(report.attach.latency.samples(), report.attach.attempts);
    EXPECT_EQ(report.handoff.latency.samples(), report.handoff.attempts);

    // 拉取失败率 < 1%:retry/(retry + count),count 为拉取次数直方图
    // 计数(延迟桩恒成功,retry 应为 0)。
    const auto metrics =
        parse_metrics_text(mesh.service("gateway").prometheus_metrics());
    const auto retry_total = metrics.total("edge_fetch_retry_total");
    const auto fetch_count =
        metrics.total("edge_fetch_duration_seconds_count");
    const auto fetch_failure_rate = retry_total + fetch_count > 0
                                        ? retry_total /
                                              (retry_total + fetch_count)
                                        : 0.0;
    std::cout << "acceptance_m2 robots=" << report.robots
              << " completed=" << report.completed
              << " skipped=" << report.skipped
              << " attach_attempts=" << report.attach.attempts
              << " attach_failures=" << report.attach.failures
              << " handoff_attempts=" << report.handoff.attempts
              << " handoff_failures=" << report.handoff.failures
              << " fetch_count=" << fetch_count
              << " fetch_retries=" << retry_total
              << " fetch_failure_rate=" << fetch_failure_rate << '\n';
    EXPECT_GE(fetch_count, 248);
    if (retry_total + fetch_count > 0) {
        EXPECT_LT(fetch_failure_rate, 0.01);
    }
}

/// M3 冒烟规模按平台分档。Linux 是生产基线，也是完整规模的门槛:1 万
/// 取号 + 2000 轮询。macOS 缩到 1/2(#104):GitHub Actions 的 macOS
/// runner 承载不了瞬时上万 TLS 建连，用例曾间歇以 ssl_connect /
/// connect_poll_timeout 失败(完成 9719、9797),失败全在建连阶段，与
/// 登录链业务无关。缩档只缩规模，成功率仍是 ≥ 99.9%,不放宽比例;
/// 轮询并发与机器人同比减半，波数(≈ 16)与放行等待不变。取号并发
/// 32 已远低于建连瓶颈，两档相同。
struct M3SmokeScale final {
    std::uint64_t queue_number_robots;
    std::uint64_t poll_robots;
    std::uint64_t poll_concurrency;
};
constexpr M3SmokeScale kM3SmokeLinuxScale{10000, 2000, 125};
#if defined(__APPLE__)
constexpr std::uint64_t kM3SmokeDivisor = 2;
#else
constexpr std::uint64_t kM3SmokeDivisor = 1;
#endif
constexpr M3SmokeScale kM3SmokeScale{
    kM3SmokeLinuxScale.queue_number_robots / kM3SmokeDivisor,
    kM3SmokeLinuxScale.poll_robots / kM3SmokeDivisor,
    // 向上取整:125 → 63,保持波数不多于 Linux 档。
    (kM3SmokeLinuxScale.poll_concurrency + kM3SmokeDivisor - 1) /
        kM3SmokeDivisor};

/// ≥ 99.9% 成功率的最少完成数(向下取整丢弃的 0.1%)。
constexpr std::uint64_t at_least_99_9_percent(std::uint64_t robots) {
    return robots - robots / 1000;
}
static_assert(at_least_99_9_percent(10000) == 9990);
static_assert(at_least_99_9_percent(5000) == 4995);

/// M3 冒烟:1 万取号(Tickets 相位,32 并发)+ 并发 progress 轮询
/// (2000 机器人 Poll 相位),合并成功率 ≥ 99.9%;macOS 按 kM3SmokeScale
/// 缩档。账号表与取号机器人等量;号值跨两次跑连续，快释放下号牌即时
/// 可兑换。#89 后每次成功取号都要跨线性一致的耐久提交边界;这里仍保留
/// Linux 1 万规模与 99.9% 成功率，写入吞吐及 1700/s 差距由
/// QueueStoreEtcdIntegrationTest 单独测量报告。
TEST(LoadgenIntegrationTest, M3SmokeTenThousandTicketsAndConcurrentPolls) {
    const ScopedLoadgenEnvironment environment;
    const std::filesystem::path source = REALMMESH_SOURCE_DIR "/configs";
    const test_support::TemporaryDirectory scratch("loadgen-it-m3-");
    ASSERT_TRUE(copy_configs_with_discovery_disabled(source, scratch.path()));
    const auto ports = unused_tcp_ports(2);
    const auto login_verify_port = ports.at(0);
    const auto queue_port = ports.at(1);
    TestEtcd etcd;
    use_loadgen_free_ports(
        scratch.path(), login_verify_port, queue_port, 0, 0, etcd.endpoint(),
        false, true, false);
    write_robot_accounts(scratch.path(), kM3SmokeScale.queue_number_robots);

    service_host::MeshHost mesh(
        scratch.path(),
        {{"login_verify", {}, false}, {"queue", {}, false}});
    ASSERT_TRUE(mesh.start_all());
    const TickDriver driver(mesh);

    // 取号并发 32(线程数 = 并发槽,不再一机器人一线程):并发突发
    // 保持在 tick 驱动 accept 的 backlog(128)之下。窗口只约束机器人
    // 能否起跑，不再重复承担存储吞吐门槛；窗口关闭时没起跑的机器人
    // 记 skipped 单列(此前会被记成拨号超时,掩盖真实吞吐)。
    LoadgenConfig tickets_run;
    tickets_run.target = LoadgenLoginTarget::Tickets;
    tickets_run.robots = kM3SmokeScale.queue_number_robots;
    tickets_run.concurrency = 32;
    tickets_run.duration_seconds = 75;
    tickets_run.endpoints = loadgen_endpoints(login_verify_port, queue_port, 0);
    const auto tickets_report = run_loadgen(tickets_run);
    const auto tickets_min_completed =
        at_least_99_9_percent(kM3SmokeScale.queue_number_robots);
    if (tickets_report.completed < tickets_min_completed) {
        std::cout << tickets_report.render();
    }

    EXPECT_GE(tickets_report.completed, tickets_min_completed);
    const auto queue_metrics = parse_metrics_text(
        mesh.service("queue").prometheus_metrics());
    EXPECT_GE(queue_metrics.total("tickets_issued_total"),
              tickets_min_completed);

    // 并发 progress 轮询:放行走默认步长,帧间隔收紧到 1s(helper 的
    // fast_release_frames)。轮询机器人取号后要等下一个放行帧才
    // admitted,自带 ~1s 等待;默认 2s 帧下 2000/125 ≈ 16 波 × 2s 的
    // admission 等待远超 30s 截止,末波机器人会撞上过期截止,dial
    // 成片报 connection_error(实测恰好截断 500 个)。
    LoadgenConfig poll_run;
    poll_run.target = LoadgenLoginTarget::Poll;
    poll_run.robots = kM3SmokeScale.poll_robots;
    poll_run.concurrency = kM3SmokeScale.poll_concurrency;
    poll_run.duration_seconds = 30;
    poll_run.poll_interval = std::chrono::milliseconds{50};
    poll_run.endpoints = loadgen_endpoints(login_verify_port, queue_port, 0);
    const auto poll_report = run_loadgen(poll_run);
    std::cout << "acceptance_m3 tickets_robots=" << tickets_report.robots
              << " tickets_completed=" << tickets_report.completed
              << " tickets_skipped=" << tickets_report.skipped
              << " poll_robots=" << poll_report.robots
              << " poll_completed=" << poll_report.completed
              << " poll_skipped=" << poll_report.skipped
              << " poll_attempts=" << poll_report.poll.attempts << '\n';
    const auto poll_min_completed =
        at_least_99_9_percent(kM3SmokeScale.poll_robots);
    if (poll_report.completed < poll_min_completed) {
        std::cout << poll_report.render();
    }

    EXPECT_GE(poll_report.completed, poll_min_completed);
    EXPECT_GE(poll_report.poll.attempts, kM3SmokeScale.poll_robots);
    EXPECT_EQ(poll_report.attach.attempts, 0);
    EXPECT_EQ(poll_report.handoff.attempts, 0);
}

/// 成功拨号不属于 attach 时延:代理在 TLS 握手前延迟转发,总墙钟包含
/// 人为停顿,attach 样本不应包含它。
TEST(LoadgenIntegrationTest, SuccessfulGatewayDialTimeIsExcludedFromAttach) {
    const ScopedLoadgenEnvironment environment;
    const std::filesystem::path source = REALMMESH_SOURCE_DIR "/configs";
    const test_support::TemporaryDirectory scratch(
        "loadgen-it-dial-latency-");
    ASSERT_TRUE(copy_configs_with_discovery_disabled(source, scratch.path()));
    const auto ports = unused_tcp_ports(4);
    const auto login_verify_port = ports.at(0);
    const auto queue_port = ports.at(1);
    const auto gateway_port = ports.at(2);
    TestEtcd etcd;
    use_loadgen_free_ports(
        scratch.path(), login_verify_port, queue_port, gateway_port,
        ports.at(3), etcd.endpoint(), true, true, false);
    write_robot_accounts(scratch.path(), 1);

    service_host::MeshHost mesh(
        scratch.path(),
        {{"login_verify", {}, false},
         {"queue", {}, false},
         {"gateway", {"login_verify"}, true}});
    ASSERT_TRUE(mesh.start_all());
    const TickDriver driver(mesh);
    constexpr auto dial_delay = std::chrono::milliseconds{2000};
    const DelayedTcpProxy proxy(gateway_port, dial_delay);

    LoadgenConfig config;
    config.target = LoadgenLoginTarget::Gateway;
    config.robots = 1;
    config.concurrency = 1;
    config.duration_seconds = 8;
    config.poll_interval = std::chrono::milliseconds{20};
    config.endpoints =
        loadgen_endpoints(login_verify_port, queue_port, proxy.port());

    const auto started = std::chrono::steady_clock::now();
    const auto report = run_loadgen(config);
    const auto wall = std::chrono::steady_clock::now() - started;

    ASSERT_EQ(report.completed, 1);
    ASSERT_EQ(report.attach.attempts, 1);
    EXPECT_GE(wall, dial_delay);
    EXPECT_LT(report.attach.latency.max(), dial_delay.count() * 3 / 4);
}

/// 网关拨号发生在 attach 计时器之外,但失败仍归 attach 相位:
/// attach 有一次 connection_error 样本,并且 handoff 尚未开始。
TEST(LoadgenIntegrationTest, GatewayDialFailureIsChargedToAttach) {
    const ScopedLoadgenEnvironment environment;
    const std::filesystem::path source = REALMMESH_SOURCE_DIR "/configs";
    const test_support::TemporaryDirectory scratch("loadgen-it-dial-fail-");
    ASSERT_TRUE(copy_configs_with_discovery_disabled(source, scratch.path()));
    const auto ports = unused_tcp_ports(3);
    const auto login_verify_port = ports.at(0);
    const auto queue_port = ports.at(1);
    const auto closed_gateway_port = ports.at(2);
    TestEtcd etcd;
    use_loadgen_free_ports(
        scratch.path(), login_verify_port, queue_port, closed_gateway_port,
        0, etcd.endpoint(), true, true, false);
    write_robot_accounts(scratch.path(), 1);

    service_host::MeshHost mesh(
        scratch.path(),
        {{"login_verify", {}, false}, {"queue", {}, false}});
    ASSERT_TRUE(mesh.start_all());
    const TickDriver driver(mesh);

    LoadgenConfig config;
    config.target = LoadgenLoginTarget::Gateway;
    config.robots = 1;
    config.concurrency = 1;
    config.duration_seconds = 5;
    config.poll_interval = std::chrono::milliseconds{20};
    config.endpoints = loadgen_endpoints(
        login_verify_port, queue_port, closed_gateway_port);

    const auto report = run_loadgen(config);

    EXPECT_EQ(report.completed, 0);
    EXPECT_EQ(report.verify.failures, 0);
    EXPECT_EQ(report.tickets.failures, 0);
    EXPECT_EQ(report.poll.failures, 0);
    // 共享链在 admit grace / 总窗口内按 200ms 重试 Gateway；每次拨号
    // 失败仍精确落在 attach connection_error，且不会伪造 handoff。
    EXPECT_GT(report.attach.attempts, 1U);
    EXPECT_EQ(report.attach.failures, report.attach.attempts);
    ASSERT_TRUE(report.attach.by_kind.contains(FailureKind::ConnectionError));
    EXPECT_EQ(report.attach.by_kind.at(FailureKind::ConnectionError),
              report.attach.attempts);
    EXPECT_EQ(report.attach.latency.samples(), report.attach.attempts);
    EXPECT_EQ(report.attach.latency.max(), 0.0);
    EXPECT_EQ(report.handoff.attempts, 0);
    EXPECT_NE(report.render().find("dial_failures:"), std::string::npos);
}

}  // namespace
}  // namespace realm::loadgen
