#include "realmmesh/network/transport/transport_capabilities.hpp"

namespace realm::network {

// REALM_NETWORK_QUIC_AVAILABLE 由 CMake 在配置期按 MsQuic 是否存在给出 1/0。
// 这里只读取它的值,不做预处理分支——QUIC 的构造实现由 CMake 选定的
// 实现文件承担(ADR-0001:平台/能力分支不进入传输层)。
TransportCapabilities platform_transport_capabilities() noexcept {
    return TransportCapabilities{
        .quic = REALM_NETWORK_QUIC_AVAILABLE != 0,
        .tls_tcp = true,
    };
}

}  // namespace realm::network
