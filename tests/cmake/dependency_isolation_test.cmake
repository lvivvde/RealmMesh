# Exercises the dependency isolation contract (#121). Run through ctest. The
# OpenSSL and protoc checks are pure functions over paths and executables and
# run in this script against fixture trees; cache isolation needs a real
# CMakeCache, so those cases configure throwaway projects with `cmake -S/-B`.
cmake_minimum_required(VERSION 3.20)

include("${REALM_MESH_TEST_SOURCE_DIR}/cmake/RealmMeshOpenSSL.cmake")
include("${REALM_MESH_TEST_SOURCE_DIR}/cmake/RealmMeshProtoc.cmake")

set(work "${REALM_MESH_TEST_WORK_DIR}")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")

function(expect_ok label error)
    if(NOT error STREQUAL "")
        message(FATAL_ERROR "${label}: expected success, got: ${error}")
    endif()
    message(STATUS "ok: ${label}")
endfunction()

function(expect_error label error expected)
    if(error STREQUAL "")
        message(FATAL_ERROR "${label}: expected an error mentioning '${expected}', got success")
    endif()
    string(FIND "${error}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "${label}: error did not mention '${expected}': ${error}")
    endif()
    message(STATUS "ok: ${label} rejected (${expected})")
endfunction()

# --- OpenSSL fixture trees -------------------------------------------------
# pkg/3.6.5            a real OpenSSL installation (Homebrew Cellar style)
# opt/openssl          dedicated prefix symlink to it (Homebrew opt/openssl@3)
# shared/include       shared include root: symlinks to OpenSSL beside Abseil
# other/3.0.0          a second, unrelated OpenSSL installation
# usr                  Debian multiarch layout (/usr/include + /usr/lib/<arch>)
set(ossl "${work}/openssl")
foreach(dir
        pkg/3.6.5/include/openssl pkg/3.6.5/lib
        shared/include/absl shared/lib opt
        other/3.0.0/include/openssl other/3.0.0/lib
        usr/include/openssl usr/lib/aarch64-linux-gnu)
    file(MAKE_DIRECTORY "${ossl}/${dir}")
endforeach()
foreach(file
        pkg/3.6.5/include/openssl/opensslv.h
        pkg/3.6.5/lib/libssl.3.dylib pkg/3.6.5/lib/libcrypto.3.dylib
        other/3.0.0/include/openssl/opensslv.h
        other/3.0.0/lib/libssl.3.dylib other/3.0.0/lib/libcrypto.3.dylib
        usr/include/openssl/opensslv.h
        usr/lib/aarch64-linux-gnu/libssl.so.3 usr/lib/aarch64-linux-gnu/libcrypto.so.3)
    file(WRITE "${ossl}/${file}" "")
endforeach()
file(CREATE_LINK "${ossl}/pkg/3.6.5" "${ossl}/opt/openssl" SYMBOLIC)
file(CREATE_LINK "${ossl}/pkg/3.6.5/lib/libssl.3.dylib" "${ossl}/pkg/3.6.5/lib/libssl.dylib" SYMBOLIC)
file(CREATE_LINK "${ossl}/pkg/3.6.5/lib/libcrypto.3.dylib" "${ossl}/pkg/3.6.5/lib/libcrypto.dylib" SYMBOLIC)
file(CREATE_LINK "${ossl}/pkg/3.6.5/include/openssl" "${ossl}/shared/include/openssl" SYMBOLIC)
file(CREATE_LINK "${ossl}/pkg/3.6.5/lib/libssl.3.dylib" "${ossl}/shared/lib/libssl.dylib" SYMBOLIC)
file(CREATE_LINK "${ossl}/usr/lib/aarch64-linux-gnu/libssl.so.3" "${ossl}/usr/lib/aarch64-linux-gnu/libssl.so" SYMBOLIC)
file(CREATE_LINK "${ossl}/usr/lib/aarch64-linux-gnu/libcrypto.so.3" "${ossl}/usr/lib/aarch64-linux-gnu/libcrypto.so" SYMBOLIC)

set(opt "${ossl}/opt/openssl")
set(opt_ssl "${opt}/lib/libssl.dylib")
set(opt_crypto "${opt}/lib/libcrypto.dylib")
file(REAL_PATH "${ossl}/pkg/3.6.5" pkg_real)

# Layout: headers must come from the package's own include directory, and
# headers and libraries from one installation.
realmmesh_openssl_check_layout("${opt}/include" "${opt_ssl}" "${opt_crypto}" "" error prefix)
expect_ok("dedicated prefix through a symlink" "${error}")
if(NOT prefix STREQUAL pkg_real)
    message(FATAL_ERROR "dedicated prefix resolved to '${prefix}', expected '${pkg_real}'")
endif()

realmmesh_openssl_check_layout("${ossl}/shared/include" "${opt_ssl}" "${opt_crypto}" "" error prefix)
expect_error("shared include root" "${error}" "not a dedicated OpenSSL include directory")

realmmesh_openssl_check_layout("${ossl}/other/3.0.0/include" "${opt_ssl}" "${opt_crypto}" "" error prefix)
expect_error("headers and libraries from different installations" "${error}" "different installations")

realmmesh_openssl_check_layout("${opt}/include" "${opt_ssl}" "${ossl}/other/3.0.0/lib/libcrypto.3.dylib" "" error prefix)
expect_error("libssl and libcrypto from different installations" "${error}" "different installations")

realmmesh_openssl_check_layout("${ossl}/usr/include"
    "${ossl}/usr/lib/aarch64-linux-gnu/libssl.so"
    "${ossl}/usr/lib/aarch64-linux-gnu/libcrypto.so"
    aarch64-linux-gnu error prefix)
expect_ok("Debian multiarch layout" "${error}")

realmmesh_openssl_check_layout("${ossl}/shared/lib" "${opt_ssl}" "${opt_crypto}" "" error prefix)
expect_error("include directory without OpenSSL headers" "${error}" "openssl/opensslv.h")

# Selection: cached discovery results must lie inside the selected root.
realmmesh_openssl_check_selection("${opt}" "${opt}/include" "${opt_ssl}" "${opt_crypto}" error)
expect_ok("cache inside the selected root" "${error}")

realmmesh_openssl_check_selection("${opt}" "${ossl}/shared/include" "${ossl}/shared/lib/libssl.dylib" "" error)
expect_error("stale shared include root" "${error}" "-UOPENSSL_INCLUDE_DIR")
string(FIND "${error}" "-UOPENSSL_SSL_LIBRARY" position)
if(NOT position EQUAL -1)
    message(FATAL_ERROR "a library symlinked into the selected root was reported as a conflict: ${error}")
endif()

realmmesh_openssl_check_selection("${opt}" "" "OPENSSL_SSL_LIBRARY-NOTFOUND" "" error)
expect_ok("nothing cached yet" "${error}")

realmmesh_openssl_check_selection("${opt}" "${opt}/include" "${ossl}/other/3.0.0/lib/libssl.3.dylib" "" error)
expect_error("library from another installation" "${error}" "-UOPENSSL_SSL_LIBRARY")

realmmesh_openssl_check_selection("${opt}" "${ossl}/gone/include" "" "" error)
expect_error("cached path that no longer exists" "${error}" "-UOPENSSL_INCLUDE_DIR")

# Root: an explicit root must hold OpenSSL headers; it never falls back.
realmmesh_openssl_check_root("${opt}" error)
expect_ok("explicit root with headers" "${error}")
realmmesh_openssl_check_root("${ossl}/shared/lib" error)
expect_error("explicit root without headers" "${error}" "OPENSSL_ROOT_DIR")

# --- protoc external entry -------------------------------------------------
set(bin "${work}/protoc")
file(MAKE_DIRECTORY "${bin}")
function(write_tool name body)
    file(WRITE "${bin}/${name}" "#!/bin/sh\n${body}\n")
    file(CHMOD "${bin}/${name}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
endfunction()
write_tool(protoc-35.0 "echo 'libprotoc 35.0'")
write_tool(protoc-34.1 "echo 'libprotoc 34.1'")
write_tool(protoc-broken "echo 'cannot execute' >&2\nexit 126")
file(WRITE "${bin}/protoc-not-executable" "")

realmmesh_check_protoc("${bin}/protoc-35.0" 35.0 error)
expect_ok("external protoc 35.0" "${error}")
realmmesh_check_protoc("${bin}/protoc-34.1" 35.0 error)
expect_error("external protoc with another version" "${error}" "libprotoc 34.1")
realmmesh_check_protoc("${bin}/protoc-broken" 35.0 error)
expect_error("external protoc that fails to run" "${error}" "cannot run")
realmmesh_check_protoc("${bin}/protoc-not-executable" 35.0 error)
expect_error("external protoc without execute permission" "${error}" "cannot run")
realmmesh_check_protoc("${bin}/missing" 35.0 error)
expect_error("external protoc that does not exist" "${error}" "does not exist")
realmmesh_check_protoc("protoc" 35.0 error)
expect_error("external protoc given by name" "${error}" "absolute path")

# --- shared cache key isolation --------------------------------------------
# The stand-in children replay the audited upstream statements: mongo-c-driver
# keeps a defined BUILD_VERSION (build/cmake/BuildVersion.cmake), and
# mongo-cxx-driver declares BUILD_VERSION 0.0.0 in the cache, then reads its
# own version file when it sees 0.0.0 (CMakeLists.txt:99, :177). Both CMP0126
# behaviours are replayed: the minimum CMake 3.20 predates the policy (OLD).
set(project "${work}/cache-isolation")
file(MAKE_DIRECTORY "${project}/c" "${project}/cxx")
file(WRITE "${project}/CMakeLists.txt" "
cmake_minimum_required(VERSION 3.20)
project(cache_isolation_probe LANGUAGES NONE)
include(\"${REALM_MESH_TEST_SOURCE_DIR}/cmake/RealmMeshCacheIsolation.cmake\")
file(WRITE \"\${CMAKE_BINARY_DIR}/versions.txt\" \"\")
realmmesh_cache_isolation_begin(probe BUILD_VERSION)
set(BUILD_VERSION 2.5.5)
add_subdirectory(c)
set(BUILD_VERSION 4.6.0)
add_subdirectory(cxx)
unset(BUILD_VERSION)
realmmesh_cache_isolation_end(BUILD_VERSION)
")
file(WRITE "${project}/c/CMakeLists.txt" "
if(NOT DEFINED BUILD_VERSION)
    set(BUILD_VERSION 9.9.9-from-c-version-file)
endif()
file(APPEND \"\${CMAKE_BINARY_DIR}/versions.txt\" \"c=\${BUILD_VERSION}\\n\")
")
file(WRITE "${project}/cxx/CMakeLists.txt" "
if(PROBE_CMP0126)
    cmake_policy(SET CMP0126 \${PROBE_CMP0126})
endif()
set(BUILD_VERSION \"0.0.0\" CACHE STRING \"Library version\")
if(BUILD_VERSION STREQUAL \"0.0.0\")
    set(BUILD_VERSION 4.6.0)
endif()
file(APPEND \"\${CMAKE_BINARY_DIR}/versions.txt\" \"cxx=\${BUILD_VERSION}\\n\")
")

function(configure_probe build)
    execute_process(
        COMMAND "${REALM_MESH_TEST_CMAKE_COMMAND}" -S "${project}" -B "${build}" -Wno-dev ${ARGN}
        RESULT_VARIABLE status
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
    )
    set(PROBE_STATUS "${status}" PARENT_SCOPE)
    set(PROBE_STDERR "${stderr}" PARENT_SCOPE)
endfunction()

foreach(policy OLD NEW)
    set(build "${work}/cache-isolation-${policy}")
    foreach(round 1 2 3)
        if(round EQUAL 1)
            configure_probe("${build}" "-DPROBE_CMP0126=${policy}" "-DUSER_CHOICE:STRING=kept")
        else()
            configure_probe("${build}")
        endif()
        if(NOT PROBE_STATUS EQUAL 0)
            message(FATAL_ERROR "CMP0126 ${policy} round ${round}: configure failed: ${PROBE_STDERR}")
        endif()
        file(READ "${build}/versions.txt" versions)
        if(NOT versions STREQUAL "c=2.5.5\ncxx=4.6.0\n")
            message(FATAL_ERROR "CMP0126 ${policy} round ${round}: drivers saw ${versions}")
        endif()
        file(STRINGS "${build}/CMakeCache.txt" leaked REGEX "^BUILD_VERSION:")
        if(leaked)
            message(FATAL_ERROR "CMP0126 ${policy} round ${round}: BUILD_VERSION left in the cache: ${leaked}")
        endif()
        file(STRINGS "${build}/CMakeCache.txt" kept REGEX "^USER_CHOICE:STRING=kept$")
        if(NOT kept)
            message(FATAL_ERROR "CMP0126 ${policy} round ${round}: an unrelated cache entry was disturbed")
        endif()
    endforeach()
    message(STATUS "ok: CMP0126 ${policy}: per-driver versions stable over 3 configures, cache restored")
endforeach()

foreach(entry "BUILD_VERSION=9.9.9" "BUILD_VERSION:STRING=0.0.0")
    string(MAKE_C_IDENTIFIER "${entry}" name)
    configure_probe("${work}/cache-isolation-${name}" "-D${entry}")
    if(PROBE_STATUS EQUAL 0)
        message(FATAL_ERROR "-D${entry}: expected a configure error, but it succeeded")
    endif()
    foreach(expected "BUILD_VERSION" "-UBUILD_VERSION")
        string(FIND "${PROBE_STDERR}" "${expected}" position)
        if(position EQUAL -1)
            message(FATAL_ERROR "-D${entry}: error did not mention '${expected}': ${PROBE_STDERR}")
        endif()
    endforeach()
    message(STATUS "ok: pre-existing -D${entry} rejected with a migration hint")
endforeach()

message(STATUS "dependency isolation contract holds")
