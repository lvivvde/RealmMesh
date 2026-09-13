# TLS 测试身份:多个测试目标与 dev-services 脚本测试共用同一份自签证书。
# 只生成在测试构建目录内,绝不写入源码树。
# 用 ECDSA P-256 而非 RSA:集成压测(#48)单事件循环串行做服务端握手
# 签名,RSA-2048 每次签名毫秒级,两万次握手就是数十秒纯 CPU;ECDSA
# 签名微秒级,测试拓扑的吞吐由链路而非证书算力决定。

find_program(REALMMESH_OPENSSL_EXECUTABLE openssl REQUIRED)
set(REALMMESH_TEST_TLS_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-tls")
set(REALMMESH_TEST_TLS_CERTIFICATE "${REALMMESH_TEST_TLS_DIRECTORY}/certificate.pem")
set(REALMMESH_TEST_TLS_PRIVATE_KEY "${REALMMESH_TEST_TLS_DIRECTORY}/private-key.pem")
add_custom_command(
    OUTPUT
        "${REALMMESH_TEST_TLS_CERTIFICATE}"
        "${REALMMESH_TEST_TLS_PRIVATE_KEY}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${REALMMESH_TEST_TLS_DIRECTORY}"
    COMMAND "${REALMMESH_OPENSSL_EXECUTABLE}" req -x509 -newkey ec
        -pkeyopt ec_paramgen_curve:P-256 -nodes
        -keyout "${REALMMESH_TEST_TLS_PRIVATE_KEY}"
        -out "${REALMMESH_TEST_TLS_CERTIFICATE}"
        -days 3650 -subj /CN=localhost
        -addext subjectAltName=DNS:localhost,IP:127.0.0.1
    VERBATIM
)
add_custom_target(realmmesh_test_tls_identity DEPENDS
    "${REALMMESH_TEST_TLS_CERTIFICATE}"
    "${REALMMESH_TEST_TLS_PRIVATE_KEY}"
)
