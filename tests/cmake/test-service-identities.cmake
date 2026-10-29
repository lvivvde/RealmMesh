# 独立于客户端接入证书的服务 CA;生成规则登记全部产物与脚本输入。
set(REALMMESH_TEST_SERVICE_TLS_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-service-tls")
set(service_identity_outputs "${REALMMESH_TEST_SERVICE_TLS_DIRECTORY}/ca.pem")
foreach(role IN ITEMS realm scene scene2 chat chat2 other)
    list(APPEND service_identity_outputs
        "${REALMMESH_TEST_SERVICE_TLS_DIRECTORY}/${role}.pem"
        "${REALMMESH_TEST_SERVICE_TLS_DIRECTORY}/${role}.key")
endforeach()
add_custom_command(OUTPUT ${service_identity_outputs}
    COMMAND bash "${PROJECT_SOURCE_DIR}/tests/scripts/generate_service_test_certs.sh"
        "${REALMMESH_TEST_SERVICE_TLS_DIRECTORY}" "${REALMMESH_OPENSSL_EXECUTABLE}"
    DEPENDS "${PROJECT_SOURCE_DIR}/tests/scripts/generate_service_test_certs.sh"
    VERBATIM)
add_custom_target(realmmesh_test_service_identities DEPENDS ${service_identity_outputs})
