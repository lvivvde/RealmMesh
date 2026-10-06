# Configuration branches work without ccache; real native behavior is gated by
# the same tool availability as AUTO, and always present in both CI jobs.
find_program(REALMMESH_TEST_PYTHON NAMES python3 REQUIRED)
if(REALMMESH_CCACHE_EXECUTABLE AND EXISTS "${REALMMESH_CCACHE_EXECUTABLE}"
    AND NOT IS_DIRECTORY "${REALMMESH_CCACHE_EXECUTABLE}")
    set(REALMMESH_TEST_CCACHE "${REALMMESH_CCACHE_EXECUTABLE}")
else()
    unset(REALMMESH_TEST_CCACHE)
    unset(REALMMESH_TEST_CCACHE CACHE)
    find_program(REALMMESH_TEST_CCACHE NAMES ccache)
endif()
set(native_cache_contract FALSE)
if(REALMMESH_TEST_CCACHE)
    execute_process(COMMAND "${REALMMESH_TEST_CCACHE}" --version
        RESULT_VARIABLE status OUTPUT_VARIABLE version)
    if(status EQUAL 0 AND version MATCHES "^ccache version (4\\.[0-9]+(\\.[0-9]+)?)([\r\n]|$)")
        if(NOT CMAKE_MATCH_1 VERSION_LESS 4.8)
            set(native_cache_contract TRUE)
        endif()
    endif()
endif()
foreach(kind Config Native)
    if(kind STREQUAL "Native")
        if(NOT native_cache_contract)
            message(STATUS "CompilerCacheNativeTest not registered: ccache >= 4.8 absent")
            continue()
        endif()
        set(extra --native --ccache "${REALMMESH_TEST_CCACHE}")
    else()
        set(extra "")
    endif()
    add_test(NAME CompilerCache${kind}Test
        COMMAND "${REALMMESH_TEST_PYTHON}" "${PROJECT_SOURCE_DIR}/tests/scripts/ccache_test.py"
            --source "${PROJECT_SOURCE_DIR}"
            --work "${CMAKE_CURRENT_BINARY_DIR}/compiler-cache-${kind}"
            --cmake "${CMAKE_COMMAND}" ${extra})
    set_tests_properties(CompilerCache${kind}Test PROPERTIES LABELS integration)
endforeach()
