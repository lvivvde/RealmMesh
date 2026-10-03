# External protoc validation for REALMMESH_PROTOC_EXECUTABLE (#121).
#
# realmmesh_check_protoc(<executable> <expected_version> <out_error>)
#
# Sets <out_error> to an empty string when <executable> is an absolute path to
# a runnable protoc that reports exactly `libprotoc <expected_version>`, and
# to a message naming the problem otherwise. The generated code must match
# the vendored libprotobuf runtime, so any other version is rejected.
#
# This file has no project dependencies on purpose: tests/cmake exercises
# every branch with stand-in executables.
function(realmmesh_check_protoc executable expected_version out_error)
    set(_hint "REALMMESH_PROTOC_EXECUTABLE='${executable}'")
    if(NOT IS_ABSOLUTE "${executable}")
        set(${out_error} "${_hint} must be an absolute path to protoc ${expected_version}." PARENT_SCOPE)
        return()
    endif()
    if(NOT EXISTS "${executable}" OR IS_DIRECTORY "${executable}")
        set(${out_error} "${_hint} does not exist or is not a file." PARENT_SCOPE)
        return()
    endif()
    execute_process(
        COMMAND "${executable}" --version
        RESULT_VARIABLE _status
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _stderr
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
    )
    if(NOT _status STREQUAL "0")
        set(${out_error}
            "${_hint} cannot run (`--version` returned '${_status}': ${_stderr})." PARENT_SCOPE)
        return()
    endif()
    if(NOT _output STREQUAL "libprotoc ${expected_version}")
        set(${out_error}
            "${_hint} reports '${_output}', but the vendored protobuf runtime needs 'libprotoc ${expected_version}'."
            PARENT_SCOPE)
        return()
    endif()
    set(${out_error} "" PARENT_SCOPE)
endfunction()
