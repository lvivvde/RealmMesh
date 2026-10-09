#include "realmmesh/test_support/etcd_process.hpp"

#include <mongoc/mongoc.h>

#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;

// This executable owns the driver lifetime; the tested process keeps its own
// unique mongocxx::instance. The deadline also covers blocked driver operations.
class Deadline final {
public:
    explicit Deadline(std::chrono::milliseconds timeout)
        : worker_([this, end = std::chrono::steady_clock::now() + timeout] {
              std::unique_lock lock(mutex_);
              if (!condition_.wait_until(lock, end, [this] { return finished_; })) {
                  constexpr char message[] =
                      "MongoDB fixture initialization deadline exceeded\n";
                  static_cast<void>(::write(STDERR_FILENO, message, sizeof(message) - 1));
                  std::_Exit(124);
              }
          }) {}

    ~Deadline() {
        {
            std::lock_guard lock(mutex_);
            finished_ = true;
        }
        condition_.notify_one();
        worker_.join();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool finished_{false};
    std::thread worker_;
};

struct Driver final {
    Driver() { mongoc_init(); }
    ~Driver() { mongoc_cleanup(); }
};

struct Document final {
    bson_t value = BSON_INITIALIZER;
    ~Document() { bson_destroy(&value); }
    Document() = default;
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
};

std::string_view string_field(const bson_t& document, const char* key) {
    bson_iter_t field;
    if (!bson_iter_init_find(&field, &document, key) || !BSON_ITER_HOLDS_UTF8(&field)) {
        return {};
    }
    return bson_iter_utf8(&field, nullptr);
}

int positive_integer(std::string_view text, int maximum) {
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value <= 0 || value > maximum) {
        throw std::invalid_argument("invalid fixture port or deadline");
    }
    return value;
}

void initialize(const std::string& member, int port, int timeout_ms) {
    Deadline deadline{std::chrono::milliseconds{timeout_ms}};
    while (!realm::test_support::loopback_port_open(static_cast<std::uint16_t>(port))) {
        std::this_thread::sleep_for(20ms);
    }

    Driver driver;
    const std::string connection = "mongodb://" + member + "/?directConnection=true";
    std::unique_ptr<mongoc_uri_t, decltype(&mongoc_uri_destroy)> uri{
        mongoc_uri_new(connection.c_str()), mongoc_uri_destroy};
    if (!uri) throw std::runtime_error("invalid local MongoDB URI");
    for (const char* option : {MONGOC_URI_SERVERSELECTIONTIMEOUTMS,
                               MONGOC_URI_CONNECTTIMEOUTMS, MONGOC_URI_SOCKETTIMEOUTMS}) {
        if (!mongoc_uri_set_option_as_int32(uri.get(), option, 1000)) {
            throw std::runtime_error("cannot set MongoDB fixture timeout");
        }
    }
    std::unique_ptr<mongoc_client_t, decltype(&mongoc_client_destroy)> client{
        mongoc_client_new_from_uri(uri.get()), mongoc_client_destroy};
    if (!client) throw std::runtime_error("cannot create MongoDB fixture client");

    Document status;
    BSON_APPEND_INT32(&status.value, "replSetGetStatus", 1);
    Document reply;
    bson_error_t error{};
    if (mongoc_client_command_simple(client.get(), "admin", &status.value, nullptr, &reply.value, &error)) {
        if (string_field(reply.value, "set") != "rs0") {
            throw std::runtime_error("MongoDB fixture is not the rs0 replica set");
        }
    } else {
        if (string_field(reply.value, "codeName") != "NotYetInitialized") {
            throw std::runtime_error(std::string{"replSetGetStatus failed: "} + error.message);
        }
        const auto config = "{\"replSetInitiate\":{\"_id\":\"rs0\",\"members\":[{\"_id\":0,\"host\":\"" + member + "\"}]}}";
        std::unique_ptr<bson_t, decltype(&bson_destroy)> command{
            bson_new_from_json(reinterpret_cast<const std::uint8_t*>(config.c_str()), -1, &error), bson_destroy};
        if (!command) throw std::runtime_error("cannot encode replica set initialization");
        Document initiated;
        if (!mongoc_client_command_simple(client.get(), "admin", command.get(), nullptr, &initiated.value, &error)) {
            throw std::runtime_error(std::string{"replSetInitiate failed: "} + error.message);
        }
    }

    Document hello;
    BSON_APPEND_INT32(&hello.value, "hello", 1);
    while (true) {
        Document observed;
        if (mongoc_client_command_simple(client.get(), "admin", &hello.value, nullptr, &observed.value, &error)) {
            bson_iter_t primary;
            if (string_field(observed.value, "setName") == "rs0" &&
                bson_iter_init_find(&primary, &observed.value, "isWritablePrimary") &&
                BSON_ITER_HOLDS_BOOL(&primary) && bson_iter_bool(&primary)) {
                return;
            }
        }
        std::this_thread::sleep_for(20ms);
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::invalid_argument("usage: realmmesh_mongodb_fixture 127.0.0.1:PORT TIMEOUT_MS");
        const std::string member = argv[1];
        constexpr std::string_view prefix = "127.0.0.1:";
        if (!member.starts_with(prefix)) throw std::invalid_argument("fixture must use IPv4 loopback");
        const int port = positive_integer(std::string_view{member}.substr(prefix.size()), 65535);
        const int timeout_ms = positive_integer(argv[2], 30000);
        initialize(member, port, timeout_ms);
        std::cout << "rs0 primary ready\n";
        return 0;
    } catch (const std::invalid_argument& error) {
        std::cerr << error.what() << '\n';
        return 64;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
