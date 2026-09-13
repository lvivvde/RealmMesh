#include "realmmesh/loadgen/loadgen.hpp"
#include "realmmesh/loadgen/metrics_scrape.hpp"
#include "realmmesh/network/tcp/tcp_listener.hpp"
#include "realmmesh/service_host/mesh_host.hpp"
#include "realmmesh/game/common/compact_jws.hpp"
#include "realmmesh/game/common/queue_number.hpp"
#include "realmmesh/test_support/temporary_directory.hpp"

#include <gtest/gtest.h>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <sys/fcntl.h>
#include <sys/resource.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
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

// 固定测试种子(64 位十六进制 = 32 字节):identity 与 queue 两把键同源
// 注入服务进程,测试侧用同一份构造 codec 做号牌验签断言。
constexpr std::string_view kTestSeedHex =
    "0102030405060708090a0b0c0d0e0f10"
    "1112131415161718191a1b1c1d1e1f20";

/// TLS 证书/会话票据/两把签名种子的环境变量守护:指向 CMake 预生成的
/// 自签证书与固定测试密钥,析构还原。
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
            ::setenv("REALMMESH_QUEUE_KEY_SEED", kTestSeedHex.data(), 1),
            0);
    }
    ~ScopedLoadgenEnvironment() {
        static_cast<void>(::unsetenv("REALMMESH_TLS_CERTIFICATE_FILE"));
        static_cast<void>(::unsetenv("REALMMESH_TLS_PRIVATE_KEY_FILE"));
        static_cast<void>(::unsetenv("REALMMESH_SESSION_TICKET_KEY"));
        static_cast<void>(::unsetenv("REALMMESH_IDENTITY_KEY_SEED"));
        static_cast<void>(::unsetenv("REALMMESH_QUEUE_KEY_SEED"));
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
/// 默认一致;account_id 缺省按账号名派生)。
void write_robot_accounts(
    const std::filesystem::path& root, std::size_t count) {
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

[[nodiscard]] std::optional<std::string> base64_decode_text(
    std::string_view input) {
    const auto decode = [](char character) -> int {
        if (character >= 'A' && character <= 'Z') return character - 'A';
        if (character >= 'a' && character <= 'z') return character - 'a' + 26;
        if (character >= '0' && character <= '9') return character - '0' + 52;
        if (character == '+') return 62;
        if (character == '/') return 63;
        return -1;
    };
    std::string output;
    output.reserve((input.size() / 4U) * 3U);
    for (std::size_t offset = 0; offset + 3U < input.size() + 1U;
         offset += 4U) {
        if (offset + 4U > input.size()) {
            return std::nullopt;
        }
        const int first = decode(input[offset]);
        const int second = decode(input[offset + 1U]);
        const int third =
            offset + 2U < input.size() && input[offset + 2U] != '='
                ? decode(input[offset + 2U])
                : -1;
        const int fourth =
            offset + 3U < input.size() && input[offset + 3U] != '='
                ? decode(input[offset + 3U])
                : -1;
        if (first < 0 || second < 0 || third < -1 || fourth < -1) {
            return std::nullopt;
        }
        const std::uint32_t value = (static_cast<std::uint32_t>(first) << 18U) |
                                    (static_cast<std::uint32_t>(second) << 12U) |
                                    (static_cast<std::uint32_t>(std::max(third, 0)) << 6U) |
                                    static_cast<std::uint32_t>(std::max(fourth, 0));
        output.push_back(static_cast<char>((value >> 16U) & 0xFFU));
        if (third >= 0) {
            output.push_back(static_cast<char>((value >> 8U) & 0xFFU));
        }
        if (fourth >= 0) {
            output.push_back(static_cast<char>(value & 0xFFU));
        }
    }
    return output;
}

/// 最低限度的 etcd v3 KV 面(POST /v3/kv/range + /v3/kv/put,进程内
/// httplib 服务):放行阀门的生产读取路径原样生效(queue 按 budget_
/// interval 拉取 /realmmesh/budgets/service/{gateway,realm}/ 前缀),
/// 只是注册中心换为测试内静态种子——发现关闭的 CI 拓扑里没有预算
/// publisher,fail-closed 语义下阀门必须由夹具供血。租约/事务不在
/// 队列存取路径上,不实现。
class FakeEtcd final {
public:
    FakeEtcd() {
        kv_.emplace(
            "/realmmesh/budgets/service/gateway/gateway-dev-01",
            R"({"conn_free":100000,"fetch_free":100000})");
        kv_.emplace(
            "/realmmesh/budgets/service/realm/realm-dev-01",
            R"({"conn_free":100000})");
        server_.Post("/v3/kv/range", [this](const auto& request, auto& response) {
            handle_range(request, response);
        });
        server_.Post("/v3/kv/put", [this](const auto& request, auto& response) {
            handle_put(request, response);
        });
        port_ = static_cast<std::uint16_t>(server_.bind_to_any_port("127.0.0.1"));
        EXPECT_GT(port_, 0);
        thread_ = std::thread([this] { static_cast<void>(server_.listen_after_bind()); });
    }
    ~FakeEtcd() {
        server_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    FakeEtcd(const FakeEtcd&) = delete;
    FakeEtcd& operator=(const FakeEtcd&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    void handle_range(
        const httplib::Request& request, httplib::Response& response) {
        const auto body = nlohmann::json::parse(
            request.body, nullptr, false);
        if (body.is_discarded()) {
            response.status = 400;
            return;
        }
        const auto start = base64_decode_text(body.value("key", ""));
        if (!start.has_value()) {
            response.status = 400;
            return;
        }
        const bool prefix = body.contains("range_end");
        std::optional<std::string> end;
        if (prefix) {
            end = base64_decode_text(body.value("range_end", ""));
            if (!end.has_value()) {
                response.status = 400;
                return;
            }
        }
        nlohmann::json kvs = nlohmann::json::array();
        std::scoped_lock lock{mutex_};
        for (const auto& [key, value] : kv_) {
            const bool in_range =
                prefix ? (key >= *start && key < *end) : (key == *start);
            if (in_range) {
                kvs.push_back({
                    {"key", base64_encode_text(key)},
                    {"value", base64_encode_text(value)},
                });
            }
        }
        response.set_content(
            nlohmann::json{{"kvs", std::move(kvs)}}.dump(), "application/json");
    }

    void handle_put(
        const httplib::Request& request, httplib::Response& response) {
        const auto body = nlohmann::json::parse(request.body, nullptr, false);
        const auto key = body.is_discarded()
                             ? std::nullopt
                             : base64_decode_text(body.value("key", ""));
        const auto value = body.is_discarded()
                               ? std::nullopt
                               : base64_decode_text(body.value("value", ""));
        if (!key.has_value() || !value.has_value()) {
            response.status = 400;
            return;
        }
        std::scoped_lock lock{mutex_};
        kv_.insert_or_assign(*key, *value);
        response.set_content(nlohmann::json::object().dump(), "application/json");
    }

    httplib::Server server_;
    std::thread thread_;
    std::mutex mutex_;
    std::map<std::string, std::string> kv_;
    std::uint16_t port_{0};
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
void use_loadgen_free_ports(
    const std::filesystem::path& root,
    std::uint16_t login_verify_port,
    std::uint16_t queue_port,
    std::uint16_t gateway_port,
    std::uint16_t grant_endpoint_port,
    std::uint16_t etcd_port,
    bool fast_release,
    bool fast_release_frames,
    bool long_handoff_grace) {
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
    if (etcd_port != 0) {
        queue = replace_all(
            queue,
            "http://127.0.0.1:2379",
            "http://127.0.0.1:" + std::to_string(etcd_port));
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
    ASSERT_TRUE(write_file(gateway_path, gateway));
}

/// MeshHost 不自转线程(帧尾指标发布与 HTTPS poll 循环都靠外部 tick
/// 驱动);测试期间 2ms 节拍持续驱动。tick 线程里的异常不允许逃逸成
/// 裸 std::terminate(CI 上缓冲的日志会随 abort 丢光):捕获后落
/// unbuffered stderr 并停摆驱动,让后续断言失败时还能看到 what()。
class TickDriver final {
public:
    explicit TickDriver(service_host::MeshHost& mesh)
        : thread_([this, &mesh] {
              try {
                  while (running_.load(std::memory_order_relaxed)) {
                      mesh.tick();
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

[[nodiscard]] RobotEndpoints loadgen_endpoints(
    std::uint16_t login_verify_port,
    std::uint16_t queue_port,
    std::uint16_t gateway_port) {
    RobotEndpoints endpoints;
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

/// 进程当前打开的 fd 数(0..rlim_cur 逐个 F_GETFD):fd 不泄漏断言的
/// 探针。基线口径两次一致即可,不求绝对完备。
[[nodiscard]] std::uint64_t count_open_fds() {
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
        if (::fcntl(descriptor, F_GETFD) != -1) {
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
        scratch.path(), login_verify_port, 0, 0, 0, 0, false, false, false);
    write_robot_accounts(scratch.path(), 200);

    service_host::MeshHost mesh(
        scratch.path(),
        {{"login_verify", {}, false}});
    ASSERT_TRUE(mesh.start_all());
    const TickDriver driver(mesh);

    LoadgenConfig config;
    config.phase = RobotPhase::Verify;
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
    FakeEtcd etcd;
    use_loadgen_free_ports(
        scratch.path(), login_verify_port, queue_port, 0, 0, etcd.port(),
        true, false, false);
    write_robot_accounts(scratch.path(), 300);

    service_host::MeshHost mesh(
        scratch.path(),
        {{"login_verify", {}, false}, {"queue", {}, false}});
    ASSERT_TRUE(mesh.start_all());
    const TickDriver driver(mesh);

    LoadgenConfig config;
    config.phase = RobotPhase::Tickets;
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
    EXPECT_EQ(report.tickets.attempts, 300);
    EXPECT_EQ(report.tickets.failures, 0);
    // 吞吐下限(spec L1):300 号牌 30s 上限 ≈ 10/s 数量级守门;本机
    // 参考亚秒。压在计数断言之后,失败时输出顺序不误导。
    EXPECT_LT(wall_seconds, 30);

    const auto metrics =
        parse_metrics_text(mesh.service("queue").prometheus_metrics());
    EXPECT_EQ(metrics.total("tickets_issued_total"), 300);

    // 号码牌验签:测试用同源种子构造签发方同款 codec,kid 与 queue.lua
    // 一致(queue-v1);每个号牌 claims 可验且号值 ≥ 1。
    common::QueueNumberCodec codec(
        common::parse_identity_seed_hex(kTestSeedHex), "queue-v1");
    const auto now = std::chrono::system_clock::now();
    ASSERT_EQ(report.number_tokens.size(), 300);
    for (const auto& token : report.number_tokens) {
        const auto claims = codec.validate(token, now);
        ASSERT_TRUE(claims.has_value());
        EXPECT_GE(claims->number, 1);
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
    FakeEtcd etcd;
    use_loadgen_free_ports(
        scratch.path(), login_verify_port, queue_port, gateway_port,
        ports.at(3), etcd.port(), true, false, true);
    write_robot_accounts(scratch.path(), 150);

    service_host::MeshHost mesh(
        scratch.path(),
        {{"login_verify", {}, false},
         {"queue", {}, false},
         {"gateway", {"login_verify"}, true}});
    ASSERT_TRUE(mesh.start_all());
    const TickDriver driver(mesh);
    const auto endpoints =
        loadgen_endpoints(login_verify_port, queue_port, gateway_port);

    // 预热:吸收一次性开销(OpenSSL/Lua/日志句柄),fd 基线从这之后取。
    LoadgenConfig warmup;
    warmup.phase = RobotPhase::Gateway;
    warmup.robots = 2;
    warmup.concurrency = 2;
    warmup.duration_seconds = 3;
    warmup.poll_interval = std::chrono::milliseconds{50};
    warmup.endpoints = endpoints;
    EXPECT_EQ(run_loadgen(warmup).completed, 2);

    const auto fd_before = count_open_fds();

    // 基线跑:小规模取 attach p50 + 服务侧 fetch 均值,作无漂移对照。
    LoadgenConfig baseline;
    baseline.phase = RobotPhase::All;
    baseline.robots = 30;
    baseline.concurrency = 30;
    baseline.duration_seconds = 4;
    baseline.poll_interval = std::chrono::milliseconds{50};
    baseline.endpoints = endpoints;
    const auto base_report = run_loadgen(baseline);
    const auto base_p50 = base_report.attach.latency.summary().p50_ms;
    const auto base_metrics =
        parse_metrics_text(mesh.service("gateway").prometheus_metrics());
    const auto base_fetch_sum =
        base_metrics.total("edge_fetch_duration_seconds_sum");
    const auto base_fetch_count =
        base_metrics.total("edge_fetch_duration_seconds_count");

    // 采样线程:主跑期间 150ms 一次抓网关水位。
    std::vector<WaterSample> samples;
    std::mutex samples_mutex;
    std::atomic_bool sampling{true};
    std::thread sampler([&] {
        while (sampling.load(std::memory_order_relaxed)) {
            auto snapshot = parse_metrics_text(
                mesh.service("gateway").prometheus_metrics());
            std::scoped_lock lock{samples_mutex};
            samples.push_back(sample_water(snapshot));
            std::this_thread::sleep_for(std::chrono::milliseconds{150});
        }
    });

    LoadgenConfig main_run;
    main_run.phase = RobotPhase::All;
    main_run.robots = 100;
    main_run.concurrency = 100;
    main_run.duration_seconds = 5;
    main_run.poll_interval = std::chrono::milliseconds{50};
    main_run.endpoints = endpoints;
    const auto report = run_loadgen(main_run);
    sampling.store(false, std::memory_order_relaxed);
    sampler.join();

    EXPECT_EQ(report.completed, 100);
    EXPECT_EQ(report.attach.failures, 0);
    EXPECT_EQ(report.handoff.failures, 0);

    // 水位断言:三段计数与额度账自洽(段和 + conn_free 恒等于管线连接
    // 容量,任何时刻快照都成立);handed-off 水位真实存在(hold 语义下
    // 100 个机器人全持,取 ≥ 50 宽松)。
    ASSERT_FALSE(samples.empty());
    double capacity = 0;
    double max_handed_off = 0;
    for (const auto& sample : samples) {
        const auto total =
            sample.pending + sample.fetching + sample.handed_off +
            sample.conn_free;
        if (capacity == 0) {
            capacity = total;
        }
        EXPECT_DOUBLE_EQ(total, capacity);
        max_handed_off = std::max(max_handed_off, sample.handed_off);
    }
    EXPECT_GT(capacity, 0);
    EXPECT_GE(max_handed_off, 50);

    // 时延无漂移:主跑 attach p50 相对自身基线不劣化超过 5 倍(宽松守
    // 门;基线为 0 时跳过,毫秒精度下 0 表示无样本)。
    const auto main_p50 = report.attach.latency.summary().p50_ms;
    if (base_p50 > 0) {
        EXPECT_LT(main_p50, base_p50 * 5);
    }

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
        if (count_open_fds() <= fd_before) {
            fds_settled = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    EXPECT_TRUE(fds_settled);
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
    FakeEtcd etcd;
    use_loadgen_free_ports(
        scratch.path(), login_verify_port, queue_port, gateway_port,
        ports.at(3), etcd.port(), true, false, false);
    write_robot_accounts(scratch.path(), 250);

    service_host::MeshHost mesh(
        scratch.path(),
        {{"login_verify", {}, false},
         {"queue", {}, false},
         {"gateway", {"login_verify"}, true}});
    ASSERT_TRUE(mesh.start_all());
    const TickDriver driver(mesh);

    LoadgenConfig config;
    config.phase = RobotPhase::Gateway;
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

    // 拉取失败率 < 1%:retry/(retry + count),count 为拉取次数直方图
    // 计数(延迟桩恒成功,retry 应为 0)。
    const auto metrics =
        parse_metrics_text(mesh.service("gateway").prometheus_metrics());
    const auto retry_total = metrics.total("edge_fetch_retry_total");
    const auto fetch_count =
        metrics.total("edge_fetch_duration_seconds_count");
    EXPECT_GE(fetch_count, 248);
    if (retry_total + fetch_count > 0) {
        EXPECT_LT(retry_total / (retry_total + fetch_count), 0.01);
    }
}

/// M3 冒烟:1 万取号(Tickets 相位,500 并发)+ 并发 progress 轮询
/// (2000 机器人 Poll 相位),合并成功率 ≥ 99.9%。账号表 1 万条;号
/// 值跨两次跑连续,快释放下号牌即时可兑换。CI 缩减档 ≤ 30s。
TEST(LoadgenIntegrationTest, M3SmokeTenThousandTicketsAndConcurrentPolls) {
    const ScopedLoadgenEnvironment environment;
    const std::filesystem::path source = REALMMESH_SOURCE_DIR "/configs";
    const test_support::TemporaryDirectory scratch("loadgen-it-m3-");
    ASSERT_TRUE(copy_configs_with_discovery_disabled(source, scratch.path()));
    const auto ports = unused_tcp_ports(2);
    const auto login_verify_port = ports.at(0);
    const auto queue_port = ports.at(1);
    FakeEtcd etcd;
    use_loadgen_free_ports(
        scratch.path(), login_verify_port, queue_port, 0, 0, etcd.port(),
        false, true, false);
    write_robot_accounts(scratch.path(), 10000);

    service_host::MeshHost mesh(
        scratch.path(),
        {{"login_verify", {}, false}, {"queue", {}, false}});
    ASSERT_TRUE(mesh.start_all());
    const TickDriver driver(mesh);

    // 两轮并发 100:低于监听 backlog(128)且留 28 位余量(两服务共用
    // tick 线程,accept 不保证即时排空)。并发即吞吐,但更高并发在本机
    // 未验证稳定,M3 冒烟走中低并发、拉长时间预算。
    LoadgenConfig tickets_run;
    tickets_run.phase = RobotPhase::Tickets;
    tickets_run.robots = 10000;
    tickets_run.concurrency = 100;
    tickets_run.duration_seconds = 45;
    tickets_run.endpoints = loadgen_endpoints(login_verify_port, queue_port, 0);
    const auto tickets_report = run_loadgen(tickets_run);
    if (tickets_report.completed < 9990) {
        std::cout << tickets_report.render();
    }

    EXPECT_GE(tickets_report.completed, 9990);  // ≥ 99.9%。
    const auto queue_metrics = parse_metrics_text(
        mesh.service("queue").prometheus_metrics());
    EXPECT_GE(queue_metrics.total("tickets_issued_total"), 9990);

    // 并发 progress 轮询:放行走默认步长,帧间隔收紧到 1s(helper 的
    // fast_release_frames)。轮询机器人取号后要等下一个放行帧才
    // admitted,自带 ~1s 等待;默认 2s 帧下 2000/125 ≈ 16 波 × 2s 的
    // admission 等待远超 30s 截止,末波机器人会撞上过期截止,dial
    // 成片报 connection_error(实测恰好截断 500 个)。
    LoadgenConfig poll_run;
    poll_run.phase = RobotPhase::Poll;
    poll_run.robots = 2000;
    poll_run.concurrency = 125;
    poll_run.duration_seconds = 30;
    poll_run.poll_interval = std::chrono::milliseconds{50};
    poll_run.endpoints = loadgen_endpoints(login_verify_port, queue_port, 0);
    const auto poll_report = run_loadgen(poll_run);
    if (poll_report.completed < 1998) {
        std::cout << poll_report.render();
    }

    EXPECT_GE(poll_report.completed, 1998);  // ≥ 99.9%。
    EXPECT_GE(poll_report.poll.attempts, 2000);
}

}  // namespace
}  // namespace realm::loadgen
