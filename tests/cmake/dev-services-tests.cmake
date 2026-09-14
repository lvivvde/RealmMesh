# dev-services.sh 的行为测试:通过 bash 驱动真实脚本与真实 realm_mesh 二进制。
# 这些用例占用真实端口并操作进程组,全部 RUN_SERIAL。
function(add_dev_services_test name test_case timeout)
    add_test(
        NAME "DevServicesScriptTest.${name}"
        COMMAND bash
            "${PROJECT_SOURCE_DIR}/tests/scripts/dev_services_test.sh"
            "${test_case}"
            "${PROJECT_SOURCE_DIR}"
            "$<TARGET_FILE:realm_mesh>"
            "${REALMMESH_TEST_TLS_CERTIFICATE}"
            "${REALMMESH_TEST_TLS_PRIVATE_KEY}"
            ${ARGN}
    )
    set_tests_properties("DevServicesScriptTest.${name}" PROPERTIES
        LABELS integration
        RUN_SERIAL TRUE
        TIMEOUT "${timeout}"
    )
endfunction()

add_dev_services_test(StartUsesSupervisor start_uses_supervisor 20)
add_dev_services_test(
    UnreadyRealmBlocksDependents unready_realm_blocks_dependents 20)
add_dev_services_test(ChildFailureStopsGroup child_failure_stops_group 20)
add_dev_services_test(StopIsReverseOrdered stop_is_reverse_ordered 20)
add_dev_services_test(CommandsManageServiceGroup commands_manage_service_group 30)
add_dev_services_test(
    NewChainFlowUsesServiceGroup
    new_chain_flow_uses_service_group
    30
    "$<TARGET_FILE:new_chain_flow_test>"
)
