# Event Loop backend selection (ADR-0001 compile-time selection, ADR-0002
# support levels).
#
# realm_mesh_select_net_backend(<system_name> <requested> <out_variable>)
#
# Resolves REALMMESH_NET_BACKEND against the target platform:
#   * `auto` maps to the platform's backend (Linux -> epoll, Darwin -> kqueue);
#   * an explicit backend is legal only on the platform it belongs to, so a
#     mismatch fails with a message naming both sides;
#   * `iocp` is a recognised value that is not implemented anywhere yet, and
#     says so while pointing at ADR-0002.
#
# This file has no project dependencies on purpose: the probe under
# tests/cmake exercises every branch in isolation with `cmake -P`.
function(realm_mesh_select_net_backend system_name requested out_variable)
    set(_valid_backends auto epoll kqueue iocp)
    list(FIND _valid_backends "${requested}" _requested_index)
    if(_requested_index EQUAL -1)
        message(FATAL_ERROR
            "REALMMESH_NET_BACKEND='${requested}' is not a known backend. "
            "Valid values: ${_valid_backends} (see ADR-0002).")
    endif()

    if(requested STREQUAL "iocp")
        message(FATAL_ERROR
            "REALMMESH_NET_BACKEND=iocp is recognised but not implemented: "
            "RealmMesh has no Windows Event Loop backend yet (see ADR-0002).")
    endif()

    if(system_name STREQUAL "Linux")
        set(_platform_backend epoll)
    elseif(system_name STREQUAL "Darwin")
        set(_platform_backend kqueue)
    elseif(system_name STREQUAL "Windows")
        set(_platform_backend iocp)
    else()
        message(FATAL_ERROR
            "realm_network has no Event Loop backend for '${system_name}'. "
            "Known platforms: Linux, Darwin, Windows (see ADR-0001).")
    endif()

    if(requested STREQUAL "auto")
        set(_resolved "${_platform_backend}")
    elseif(requested STREQUAL "${_platform_backend}")
        set(_resolved "${requested}")
    else()
        message(FATAL_ERROR
            "REALMMESH_NET_BACKEND=${requested} does not match platform "
            "'${system_name}', whose backend is ${_platform_backend} "
            "(see ADR-0002).")
    endif()

    # Reached when `auto` lands on a platform whose backend is not implemented.
    if(_resolved STREQUAL "iocp")
        message(FATAL_ERROR
            "realm_network has no Event Loop backend for '${system_name}': iocp "
            "is recognised but not implemented (see ADR-0002).")
    endif()

    set(${out_variable} "${_resolved}" PARENT_SCOPE)
endfunction()
