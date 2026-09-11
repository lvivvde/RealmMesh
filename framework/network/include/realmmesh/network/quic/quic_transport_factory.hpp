#pragma once

#include "realmmesh/network/transport/transport_config.hpp"

#include <memory>

namespace realm::observability {
class Logger;
}

namespace realm::network {

/// QUIC Transport 的构造入口。实现文件由 CMake 按 MsQuic 是否可用选定:
/// 可用时返回真实传输,不可用时抛错(能力位与构建期不一致,属内部矛盾)。
/// 调用方因此不需要任何平台预处理分支(ADR-0001)。
[[nodiscard]] std::unique_ptr<IMessageTransport> make_quic_transport(
    TransportConfig config, observability::Logger* logger);

}  // namespace realm::network
