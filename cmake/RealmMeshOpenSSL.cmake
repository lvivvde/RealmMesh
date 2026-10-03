# Single OpenSSL source for the whole build (#121).
#
# realmmesh_find_openssl()
#
# Called from the top-level CMakeLists.txt before any vendored dependency
# that looks for OpenSSL (mongo-c-driver does), so every consumer reuses the
# same FindOpenSSL cache entries and imported targets:
#   * an explicit OPENSSL_ROOT_DIR (-D, cache or environment) wins and must
#     hold OpenSSL headers; an invalid one fails instead of falling back;
#   * otherwise macOS defaults to Homebrew's dedicated openssl@3 prefix. The
#     shared /opt/homebrew/include that FindOpenSSL would pick on its own also
#     exposes Homebrew's Abseil, which then shadows the pinned one;
#   * discovery results already in the cache (OPENSSL_INCLUDE_DIR and the two
#     libraries) must lie inside the selected root. find_path/find_library
#     never revisit a cached result, so a stale entry is reported with its
#     `-U<key>` migration rather than silently kept. Fresh results are held to
#     the same root, since search paths such as OpenSSL_ROOT or
#     CMAKE_PREFIX_PATH come before the root hints;
#   * the found headers must come from a dedicated include directory, and
#     headers and libraries from one installation;
#   * a probe program links OpenSSL::SSL and OpenSSL::Crypto (catching an
#     architecture mismatch) and, when not cross-compiling, checks that the
#     header and runtime major.minor versions agree. Its verdict is cached
#     until the resolved files change.
#
# The check functions below have no project dependencies on purpose:
# tests/cmake exercises every branch against fixture trees.
include("${CMAKE_CURRENT_LIST_DIR}/RealmMeshCacheIsolation.cmake")

# Sets <out_error> to an empty string when <root> holds OpenSSL headers.
function(realmmesh_openssl_check_root root out_error)
    if(EXISTS "${root}/include/openssl/opensslv.h")
        set(${out_error} "" PARENT_SCOPE)
    else()
        set(${out_error}
            "OPENSSL_ROOT_DIR='${root}' has no include/openssl/opensslv.h; point it at an OpenSSL 3 installation prefix."
            PARENT_SCOPE)
    endif()
endfunction()

# Sets <out_error> to an empty string when each cached discovery result is
# unset or resolves inside <root>, and to a message naming the stale keys and
# their `-U` migration otherwise.
function(realmmesh_openssl_check_selection root include_dir ssl_library crypto_library out_error)
    file(REAL_PATH "${root}" _root)
    set(_stale "")
    set(_unset_flags "")
    set(_keys OPENSSL_INCLUDE_DIR OPENSSL_SSL_LIBRARY OPENSSL_CRYPTO_LIBRARY)
    set(_values "${include_dir}" "${ssl_library}" "${crypto_library}")
    foreach(_key _value IN ZIP_LISTS _keys _values)
        if(_value STREQUAL "" OR _value MATCHES "-NOTFOUND$")
            continue()
        endif()
        if(EXISTS "${_value}")
            file(REAL_PATH "${_value}" _real)
            string(FIND "${_real}/" "${_root}/" _position)
            if(_position EQUAL 0)
                continue()
            endif()
            string(APPEND _stale "\n    ${_key}=${_value} (resolves to ${_real})")
        else()
            string(APPEND _stale "\n    ${_key}=${_value} (does not exist)")
        endif()
        string(APPEND _unset_flags " -U${_key}")
    endforeach()
    if(_stale STREQUAL "")
        set(${out_error} "" PARENT_SCOPE)
        return()
    endif()
    realmmesh_cache_migration_hint(_hint "${_unset_flags}")
    set(${out_error}
        "The OpenSSL root is '${root}' (${_root}), but cached discovery results lie outside it:${_stale}\nFindOpenSSL keeps cached results even when the root changes. Remove them and configure again:\n${_hint}"
        PARENT_SCOPE)
endfunction()

# Sets <out_error> to an empty string and <out_prefix> to the installation
# prefix when <include_dir> is a dedicated OpenSSL include directory and both
# libraries belong to the same installation. <library_architecture> is the
# Debian multiarch subdirectory under lib/, or empty.
function(realmmesh_openssl_check_layout include_dir ssl_library crypto_library library_architecture out_error out_prefix)
    set(${out_prefix} "" PARENT_SCOPE)
    set(_header "${include_dir}/openssl/opensslv.h")
    if(NOT EXISTS "${_header}")
        set(${out_error} "OpenSSL include directory '${include_dir}' has no openssl/opensslv.h." PARENT_SCOPE)
        return()
    endif()
    file(REAL_PATH "${include_dir}" _include)
    file(REAL_PATH "${_header}" _real_header)
    get_filename_component(_header_include "${_real_header}" DIRECTORY)
    get_filename_component(_header_include "${_header_include}" DIRECTORY)
    if(NOT _include STREQUAL _header_include)
        set(${out_error}
            "OpenSSL include directory '${include_dir}' is not a dedicated OpenSSL include directory: its openssl/ resolves into '${_header_include}'. A shared include root (such as Homebrew's /opt/homebrew/include) also exposes other packages' headers, e.g. Abseil, ahead of the pinned ones. Set OPENSSL_ROOT_DIR to the OpenSSL installation prefix instead."
            PARENT_SCOPE)
        return()
    endif()
    get_filename_component(_prefix "${_include}" DIRECTORY)

    foreach(_library IN ITEMS "${ssl_library}" "${crypto_library}")
        file(REAL_PATH "${_library}" _real_library)
        get_filename_component(_library_dir "${_real_library}" DIRECTORY)
        if(library_architecture AND _library_dir MATCHES "/${library_architecture}$")
            get_filename_component(_library_dir "${_library_dir}" DIRECTORY)
        endif()
        get_filename_component(_library_prefix "${_library_dir}" DIRECTORY)
        if(NOT _library_prefix STREQUAL _prefix)
            set(${out_error}
                "OpenSSL headers and libraries come from different installations: headers from '${_prefix}', '${_library}' from '${_library_prefix}'. Point OPENSSL_ROOT_DIR at one installation and remove the stale cache entries with -U."
                PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_error} "" PARENT_SCOPE)
    set(${out_prefix} "${_prefix}" PARENT_SCOPE)
endfunction()

function(_realmmesh_openssl_default_root out_root)
    set(${out_root} "" PARENT_SCOPE)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        return()
    endif()
    set(_candidates /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3)
    if(NOT "$ENV{HOMEBREW_PREFIX}" STREQUAL "")
        list(PREPEND _candidates "$ENV{HOMEBREW_PREFIX}/opt/openssl@3")
    endif()
    foreach(_candidate IN LISTS _candidates)
        realmmesh_openssl_check_root("${_candidate}" _error)
        if(_error STREQUAL "")
            set(${out_root} "${_candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

function(realmmesh_find_openssl)
    if(DEFINED OPENSSL_ROOT_DIR AND NOT OPENSSL_ROOT_DIR STREQUAL "")
        set(_root "${OPENSSL_ROOT_DIR}")
        set(_origin "OPENSSL_ROOT_DIR")
    elseif(DEFINED ENV{OPENSSL_ROOT_DIR} AND NOT "$ENV{OPENSSL_ROOT_DIR}" STREQUAL "")
        set(_root "$ENV{OPENSSL_ROOT_DIR}")
        set(_origin "environment OPENSSL_ROOT_DIR")
    else()
        _realmmesh_openssl_default_root(_root)
        set(_origin "macOS default (Homebrew openssl@3)")
        if(_root STREQUAL "")
            set(_origin "FindOpenSSL search")
        else()
            # A normal variable: the default stays a default and is chosen
            # afresh by every configure.
            set(OPENSSL_ROOT_DIR "${_root}" PARENT_SCOPE)
            set(OPENSSL_ROOT_DIR "${_root}")
        endif()
    endif()

    if(NOT _root STREQUAL "")
        realmmesh_openssl_check_root("${_root}" _error)
        if(NOT _error STREQUAL "")
            message(FATAL_ERROR "${_error}")
        endif()
        realmmesh_openssl_check_selection("${_root}"
            "$CACHE{OPENSSL_INCLUDE_DIR}" "$CACHE{OPENSSL_SSL_LIBRARY}" "$CACHE{OPENSSL_CRYPTO_LIBRARY}" _error)
        if(NOT _error STREQUAL "")
            message(FATAL_ERROR "${_error}")
        endif()
    endif()

    find_package(OpenSSL 3.0 REQUIRED COMPONENTS SSL Crypto)
    # Imported targets and cache entries are directory-wide already; hand the
    # result variables to the caller too.
    set(OPENSSL_FOUND "${OPENSSL_FOUND}" PARENT_SCOPE)
    set(OPENSSL_VERSION "${OPENSSL_VERSION}" PARENT_SCOPE)

    if(NOT _root STREQUAL "")
        realmmesh_openssl_check_selection("${_root}"
            "${OPENSSL_INCLUDE_DIR}" "${OPENSSL_SSL_LIBRARY}" "${OPENSSL_CRYPTO_LIBRARY}" _error)
        if(NOT _error STREQUAL "")
            message(FATAL_ERROR
                "FindOpenSSL resolved OpenSSL outside the selected root: another search path such as "
                "OpenSSL_ROOT or CMAKE_PREFIX_PATH took precedence. Drop that path or point "
                "OPENSSL_ROOT_DIR at that installation.\n${_error}")
        endif()
    endif()

    realmmesh_openssl_check_layout("${OPENSSL_INCLUDE_DIR}" "${OPENSSL_SSL_LIBRARY}" "${OPENSSL_CRYPTO_LIBRARY}"
        "${CMAKE_LIBRARY_ARCHITECTURE}" _error _prefix)
    if(NOT _error STREQUAL "")
        message(FATAL_ERROR "${_error}")
    endif()

    _realmmesh_openssl_probe()

    if(_root STREQUAL "")
        set(_root_text "none")
    else()
        set(_root_text "${_root}")
    endif()
    message(STATUS "RealmMesh: OpenSSL ${OPENSSL_VERSION} from ${_prefix} (root ${_root_text}, ${_origin})")
endfunction()

# Links and runs cmake/RealmMeshOpenSSLCheck.c against the imported targets.
function(_realmmesh_openssl_probe)
    set(_signature "${CMAKE_C_COMPILER};${CMAKE_OSX_ARCHITECTURES}")
    foreach(_file IN ITEMS "${OPENSSL_INCLUDE_DIR}/openssl/opensslv.h" "${OPENSSL_SSL_LIBRARY}" "${OPENSSL_CRYPTO_LIBRARY}")
        file(REAL_PATH "${_file}" _real)
        file(TIMESTAMP "${_real}" _stamp "%s" UTC)
        list(APPEND _signature "${_real}@${_stamp}")
    endforeach()
    if(DEFINED CACHE{REALMMESH_OPENSSL_VERIFIED} AND "$CACHE{REALMMESH_OPENSSL_VERIFIED}" STREQUAL "${_signature}")
        return()
    endif()

    set(_source "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/RealmMeshOpenSSLCheck.c")
    set(_bindir "${CMAKE_BINARY_DIR}/CMakeFiles/realm_openssl_check")
    if(CMAKE_CROSSCOMPILING)
        try_compile(_compiled "${_bindir}" "${_source}"
            LINK_LIBRARIES OpenSSL::SSL OpenSSL::Crypto
            OUTPUT_VARIABLE _output)
        set(_ran 0)
    else()
        try_run(_ran _compiled "${_bindir}" "${_source}"
            LINK_LIBRARIES OpenSSL::SSL OpenSSL::Crypto
            COMPILE_OUTPUT_VARIABLE _output
            RUN_OUTPUT_VARIABLE _run_output)
    endif()
    if(NOT _compiled)
        message(FATAL_ERROR
            "OpenSSL from '${OPENSSL_INCLUDE_DIR}' and '${OPENSSL_SSL_LIBRARY}' cannot build a program for "
            "this target (wrong architecture or mismatched installation):\n${_output}")
    endif()
    if(NOT _ran STREQUAL "0")
        message(FATAL_ERROR
            "OpenSSL headers in '${OPENSSL_INCLUDE_DIR}' do not match the runtime library "
            "'${OPENSSL_SSL_LIBRARY}':\n${_run_output}")
    endif()
    set(REALMMESH_OPENSSL_VERIFIED "${_signature}" CACHE INTERNAL "OpenSSL files last verified by the probe")
endfunction()
