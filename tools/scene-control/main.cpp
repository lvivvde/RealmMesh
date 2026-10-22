#include <nlohmann/json.hpp>
#include "realmmesh/game/common/scene_internal_protocol.hpp"
#include "realmmesh/game/common/scene_protocol.hpp"
#include "realmmesh/game/common/session_ticket.hpp"
#include "realmmesh/network/client/tls_client_stream.hpp"
#include "realmmesh/network/codec/length_field_codec.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>

namespace common = realm::game::common;
namespace network = realm::network;
namespace internal = ::realmmesh::protocol::scene_internal::v1;
namespace {
using Clock = std::chrono::steady_clock;
std::string environment(const char* name) {
    const auto* value = std::getenv(name);
    return value ? value : "";
}
class Control {
public:
    Control(
        std::string host,
        std::uint16_t port,
        std::string instance,
        const network::client::TlsClientOptions& tls,
        Clock::time_point deadline)
        : instance_(std::move(instance)),
          caller_(
              "scene-control-" +
              common::correlation_id_hex(common::make_correlation_id())) {
        auto connected =
            network::client::TlsClientStream::dial(host, port, tls, deadline);
        if (!connected.ok() ||
            connected.stream->negotiated_alpn() != common::scene_internal_alpn)
            throw std::runtime_error("Scene mutual TLS authentication failed");
        stream_ = connected.stream;
        incarnation_ = "discover";
        const auto status = call(internal::OPERATION_STATUS, {}, 0, deadline);
        if (!status.dynamic())
            throw std::runtime_error("Scene dynamic mode is disabled");
        incarnation_ = status.scene_incarnation();
    }
    common::SceneReply call(
        internal::Operation operation,
        const std::string& id,
        std::uint32_t timeout,
        Clock::time_point deadline) {
        common::SceneRequest request;
        request.set_realm_incarnation(caller_);
        request.set_scene_instance_id(instance_);
        request.set_scene_incarnation(incarnation_);
        request.set_operation(operation);
        if (operation == internal::OPERATION_SET_DRAINING) {
            request.set_draining(true);
            request.set_drain_id(id);
            request.set_drain_timeout_ms(timeout);
        }
        const auto correlation = next_++;
        if (!stream_->write_all(
                codec_.encode(common::encode(request, correlation)), deadline))
            throw std::runtime_error("Scene control write failed");
        while (Clock::now() < deadline) {
            const auto decoded = codec_.try_decode(input_);
            if (decoded.status == network::DecodeStatus::FrameReady) {
                if (common::decode_scene_push(decoded.payload)) continue;
                const auto reply = common::decode_scene_reply(decoded.payload);
                if (!reply ||
                    common::scene_internal_request_id(decoded.payload) !=
                        correlation ||
                    reply->realm_incarnation() != caller_ ||
                    reply->scene_instance_id() != instance_ ||
                    (incarnation_ != "discover" &&
                     reply->scene_incarnation() != incarnation_) ||
                    reply->operation() != operation ||
                    reply->status() != internal::STATUS_OK)
                    throw std::runtime_error(
                        "Scene control rejected or invalid reply");
                return *reply;
            }
            if (decoded.status == network::DecodeStatus::FrameTooLarge)
                throw std::runtime_error("Scene control frame exceeds limit");
            std::array<std::byte, 8192> bytes{};
            const auto count = stream_->read_some(bytes, deadline);
            if (!count || *count == 0) break;
            input_.append(std::span<const std::byte>(bytes.data(), *count));
        }
        throw std::runtime_error("Scene control deadline exceeded");
    }

private:
    std::string instance_, caller_, incarnation_;
    std::shared_ptr<network::client::TlsClientStream> stream_;
    network::LengthFieldCodec codec_{common::scene_envelope_max};
    network::ByteBuffer input_;
    std::uint64_t next_{1};
};
void print(const common::SceneReply& reply) {
    const char* phase =
        reply.drain_phase() == internal::DRAIN_PHASE_COMPLETE  ? "complete"
        : reply.drain_phase() == internal::DRAIN_PHASE_FAILED  ? "failed"
        : reply.drain_phase() == internal::DRAIN_PHASE_RUNNING ? "running"
                                                               : "idle";
    std::cout << nlohmann::json{{"instance_id", reply.scene_instance_id()},
        {"incarnation", reply.scene_incarnation()}, {"phase", phase},
        {"operation_id", reply.drain_id()}, {"draining", reply.draining()},
        {"active_members", reply.active_members()}, {"prepared_members", reply.prepared_members()},
        {"owned_shards", reply.shards_size()}}.dump() << '\n' << std::flush;
}
}  // namespace
int main(int argc, char** argv) {
    try {
        std::map<std::string, std::string> options{
            {"--host", "127.0.0.1"},
            {"--port", "7200"},
            {"--instance", "scene-dev-01"},
            {"--timeout-ms", "20000"},
            {"--ca", environment("REALMMESH_SERVICE_CA_FILE")},
            {"--certificate", environment("REALMMESH_REALM_CERTIFICATE_FILE")},
            {"--key", environment("REALMMESH_REALM_PRIVATE_KEY_FILE")},
            {"--server-identity", ""},
            {"--operation-id", ""}};
        bool drain = false;
        for (int i = 1; i < argc; ++i) {
            const std::string key = argv[i];
            if (key == "--help") {
                std::cout
                    << "Usage: realm_mesh_scene_control --status|--drain [--host HOST] [--port PORT] "
                       "[--instance ID] --server-identity DNS [--ca FILE --certificate FILE --key FILE] "
                       "[--operation-id ID] [--timeout-ms 100..60000]\n";
                return 0;
            }
            if (key == "--drain") {
                drain = true;
                continue;
            }
            if (key == "--status") continue;
            if (!options.contains(key) || ++i >= argc)
                throw std::invalid_argument("invalid control option");
            options[key] = argv[i];
        }
        for (const auto* key :
             {"--host",
              "--instance",
              "--ca",
              "--certificate",
              "--key",
              "--server-identity"})
            if (options.at(key).empty())
                throw std::invalid_argument(
                    "missing control identity or endpoint option");
        const auto port = std::stoul(options.at("--port"));
        const auto timeout = std::stoul(options.at("--timeout-ms"));
        if (port == 0 || port > 65535 || timeout < 100 || timeout > 60000)
            throw std::invalid_argument("invalid control port or timeout");
        network::client::TlsClientOptions tls;
        tls.alpn = common::scene_internal_alpn;
        tls.ca_file = options.at("--ca");
        tls.certificate_chain_file = options.at("--certificate");
        tls.private_key_file = options.at("--key");
        tls.server_identity = options.at("--server-identity");
        const auto deadline =
            Clock::now() + std::chrono::milliseconds(timeout + 3000);
        const auto call_deadline = [&] {
            return std::min(deadline, Clock::now() + std::chrono::seconds(3));
        };
        Control control(
            options.at("--host"),
            static_cast<std::uint16_t>(port),
            options.at("--instance"),
            tls,
            call_deadline());
        const auto operation_id = options.at("--operation-id").empty()
                                      ? options.at("--instance")
                                      : options.at("--operation-id");
        auto reply = control.call(
            drain ? internal::OPERATION_SET_DRAINING
                  : internal::OPERATION_STATUS,
            operation_id,
            static_cast<std::uint32_t>(timeout),
            call_deadline());
        print(reply);
        while (drain && reply.drain_phase() == internal::DRAIN_PHASE_RUNNING &&
               Clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            reply = control.call(
                internal::OPERATION_STATUS, {}, 0, call_deadline());
            print(reply);
        }
        if (reply.drain_phase() == internal::DRAIN_PHASE_FAILED ||
            (drain && reply.drain_phase() != internal::DRAIN_PHASE_COMPLETE))
            return 2;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
