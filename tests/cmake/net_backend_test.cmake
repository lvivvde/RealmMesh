# Exercises the REALMMESH_NET_BACKEND contract (ADR-0002). Run through ctest;
# each case shells out to `cmake -P` so configure failures are observed as
# subprocess exits instead of aborting this script.
cmake_minimum_required(VERSION 3.20)

set(probe "${REALM_MESH_TEST_SOURCE_DIR}/tests/cmake/net_backend_probe.cmake")
set(result_file "${REALM_MESH_TEST_WORK_DIR}/resolved-net-backend.txt")

function(run_probe system_name requested)
    file(REMOVE "${result_file}")
    execute_process(
        COMMAND "${REALM_MESH_TEST_CMAKE_COMMAND}"
            "-DREALM_MESH_PROBE_MODULE=${REALM_MESH_TEST_SOURCE_DIR}/cmake/RealmMeshNetBackend.cmake"
            "-DREALM_MESH_PROBE_SYSTEM=${system_name}"
            "-DREALM_MESH_PROBE_REQUESTED=${requested}"
            "-DREALM_MESH_PROBE_RESULT=${result_file}"
            -P "${probe}"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
    )
    set(PROBE_STATUS "${status}" PARENT_SCOPE)
    set(PROBE_STDERR "${stderr}" PARENT_SCOPE)
    if(EXISTS "${result_file}")
        file(READ "${result_file}" resolved)
        string(STRIP "${resolved}" resolved)
    else()
        set(resolved "")
    endif()
    set(PROBE_RESOLVED "${resolved}" PARENT_SCOPE)
endfunction()

function(expect_resolution system_name requested expected_backend)
    run_probe("${system_name}" "${requested}")
    if(NOT PROBE_STATUS EQUAL 0)
        message(FATAL_ERROR
            "${system_name} + ${requested}: expected success, configure failed: "
            "${PROBE_STDERR}")
    endif()
    if(NOT PROBE_RESOLVED STREQUAL "${expected_backend}")
        message(FATAL_ERROR
            "${system_name} + ${requested}: expected backend "
            "'${expected_backend}', got '${PROBE_RESOLVED}'")
    endif()
    message(STATUS "ok: ${system_name} + ${requested} -> ${PROBE_RESOLVED}")
endfunction()

function(expect_rejection system_name requested expected_message)
    run_probe("${system_name}" "${requested}")
    if(PROBE_STATUS EQUAL 0)
        message(FATAL_ERROR
            "${system_name} + ${requested}: expected a configure error, but it "
            "succeeded with backend '${PROBE_RESOLVED}'")
    endif()
    string(FIND "${PROBE_STDERR}" "${expected_message}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "${system_name} + ${requested}: error did not mention "
            "'${expected_message}': ${PROBE_STDERR}")
    endif()
    message(STATUS "ok: ${system_name} + ${requested} rejected (${expected_message})")
endfunction()

# auto maps to the platform's own backend.
expect_resolution(Linux auto epoll)
expect_resolution(Darwin auto kqueue)

# An explicit backend is legal on its own platform.
expect_resolution(Linux epoll epoll)
expect_resolution(Darwin kqueue kqueue)

# A mismatch is rejected rather than silently resolved.
expect_rejection(Linux kqueue "does not match platform")
expect_rejection(Darwin epoll "does not match platform")

# iocp is recognised everywhere, implemented nowhere, and points at ADR-0002.
expect_rejection(Linux iocp "not implemented")
expect_rejection(Darwin iocp "not implemented")
expect_rejection(Windows auto "not implemented")

# Unknown values are distinguished from recognised-but-unimplemented ones.
expect_rejection(Linux bogus "not a known backend")

message(STATUS "REALMMESH_NET_BACKEND contract holds")
