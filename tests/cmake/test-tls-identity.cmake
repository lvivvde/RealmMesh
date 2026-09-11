# TLS 测试身份:多个测试目标与 dev-services 脚本测试共用同一份自签证书。
# 只生成在测试构建目录内,绝不写入源码树。

find_program(REALMMESH_OPENSSL_EXECUTABLE openssl REQUIRED)
set(REALMMESH_TEST_TLS_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-tls")
set(REALMMESH_TEST_TLS_CERTIFICATE "${REALMMESH_TEST_TLS_DIRECTORY}/certificate.pem")
set(REALMMESH_TEST_TLS_PRIVATE_KEY "${REALMMESH_TEST_TLS_DIRECTORY}/private-key.pem")
add_custom_command(
    OUTPUT
        "${REALMMESH_TEST_TLS_CERTIFICATE}"
        "${REALMMESH_TEST_TLS_PRIVATE_KEY}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${REALMMESH_TEST_TLS_DIRECTORY}"
    COMMAND "${REALMMESH_OPENSSL_EXECUTABLE}" req -x509 -newkey rsa:2048 -nodes
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
