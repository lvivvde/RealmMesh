# Exercises the Ninja build graph contract (#123). Run through ctest.
#
# - realmmesh_check_ninja is a pure function over a generator name and an
#   executable, checked here against stand-in `ninja` scripts.
# - realmmesh_require_ninja must stop a Ninja configure before project() with a
#   hint, so a throwaway project is configured with a stand-in that is too old.
# - realmmesh_use_ninja_link_pool must put every link edge in a depth-1 pool,
#   except targets released with realmmesh_link_outside_pool, so a throwaway C
#   project is generated with the real Ninja and its build.ninja is inspected.
# - The libsodium install manifest check runs in script mode against fixture
#   install trees.
cmake_minimum_required(VERSION 3.20)

include("${REALM_MESH_TEST_SOURCE_DIR}/cmake/RealmMeshNinja.cmake")
set(sodium_manifest "${REALM_MESH_TEST_SOURCE_DIR}/third_party/sodium/sodium-install-manifest.cmake")
include("${sodium_manifest}")

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

# Writes an executable stand-in for ninja that prints <output> and exits with <status>.
function(fake_ninja name output status)
    set(path "${work}/bin/${name}")
    file(WRITE "${path}" "#!/bin/sh\necho '${output}'\nexit ${status}\n")
    file(CHMOD "${path}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
endfunction()

fake_ninja(ninja-1.10 "1.10.2" 0)
fake_ninja(ninja-1.11 "1.11.1" 0)
fake_ninja(ninja-1.13 "1.13.2" 0)
fake_ninja(ninja-2.0-git "2.0.0.git" 0)
fake_ninja(ninja-garbage "not a version" 0)
fake_ninja(ninja-broken "1.13.2" 3)
set(bin "${work}/bin")

# --- realmmesh_check_ninja ---------------------------------------------------
realmmesh_check_ninja("Unix Makefiles" "" error)
expect_ok("Make generator needs no ninja" "${error}")

realmmesh_check_ninja("Ninja" "${bin}/ninja-1.11" error)
expect_ok("ninja 1.11.1 accepted" "${error}")
realmmesh_check_ninja("Ninja" "${bin}/ninja-1.13" error)
expect_ok("ninja 1.13.2 accepted" "${error}")
realmmesh_check_ninja("Ninja Multi-Config" "${bin}/ninja-2.0-git" error)
expect_ok("ninja 2.0.0.git accepted" "${error}")

realmmesh_check_ninja("Ninja" "${bin}/ninja-1.10" error)
expect_error("ninja 1.10.2" "${error}" "1.11")
expect_error("ninja 1.10.2 hint" "${error}" "--preset dev-make")
realmmesh_check_ninja("Ninja" "REALMMESH_NINJA-NOTFOUND" error)
expect_error("ninja missing" "${error}" "--preset dev-make")
realmmesh_check_ninja("Ninja" "" error)
expect_error("ninja unset" "${error}" "`brew install ninja`; Ubuntu")
realmmesh_check_ninja("Ninja" "${bin}/ninja-garbage" error)
expect_error("ninja unparseable version" "${error}" "not a version")
realmmesh_check_ninja("Ninja" "${bin}/ninja-broken" error)
expect_error("ninja that fails to run" "${error}" "--version")

# --- realmmesh_require_ninja stops before project() ---------------------------
set(hint_source "${work}/hint-src")
file(MAKE_DIRECTORY "${hint_source}")
file(WRITE "${hint_source}/CMakeLists.txt" "
cmake_minimum_required(VERSION 3.20)
include(\"${REALM_MESH_TEST_SOURCE_DIR}/cmake/RealmMeshNinja.cmake\")
realmmesh_require_ninja()
message(FATAL_ERROR \"reached past realmmesh_require_ninja\")
project(Hint LANGUAGES NONE)
")
execute_process(
    COMMAND "${REALM_MESH_TEST_CMAKE_COMMAND}" -S "${hint_source}" -B "${work}/hint-build"
        -G Ninja "-DCMAKE_MAKE_PROGRAM=${bin}/ninja-1.10"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output
)
if(status EQUAL 0)
    message(FATAL_ERROR "configure with ninja 1.10.2 succeeded:\n${output}")
endif()
expect_error("configure with ninja 1.10.2" "${output}" "--preset dev-make")
string(FIND "${output}" "reached past" position)
if(NOT position EQUAL -1)
    message(FATAL_ERROR "realmmesh_require_ninja did not stop the configure:\n${output}")
endif()

# --- realmmesh_use_ninja_link_pool ---------------------------------------------
find_program(real_ninja NAMES ninja-build ninja)
if(NOT real_ninja)
    message(FATAL_ERROR "the link pool case needs a real ninja on PATH")
endif()
set(pool_source "${work}/pool-src")
file(MAKE_DIRECTORY "${pool_source}")
file(WRITE "${pool_source}/lib.c" "int lib(void) { return 0; }\n")
file(WRITE "${pool_source}/main.c" "int lib(void);\nint main(void) { return lib(); }\n")
file(WRITE "${pool_source}/CMakeLists.txt" "
cmake_minimum_required(VERSION 3.20)
include(\"${REALM_MESH_TEST_SOURCE_DIR}/cmake/RealmMeshNinja.cmake\")
realmmesh_require_ninja()
project(Pool LANGUAGES C)
realmmesh_use_ninja_link_pool()
add_subdirectory(sub)
add_executable(pool_main main.c)
target_link_libraries(pool_main PRIVATE pool_static pool_shared)
add_executable(pool_free main.c)
target_link_libraries(pool_free PRIVATE pool_static)
realmmesh_link_outside_pool(pool_free)
")
file(MAKE_DIRECTORY "${pool_source}/sub")
file(WRITE "${pool_source}/sub/CMakeLists.txt"
    "add_library(pool_static STATIC ../lib.c)\nadd_library(pool_shared SHARED ../lib.c)\n")
execute_process(
    COMMAND "${REALM_MESH_TEST_CMAKE_COMMAND}" -S "${pool_source}" -B "${work}/pool-build"
        -G Ninja "-DCMAKE_MAKE_PROGRAM=${real_ninja}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output
)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "configuring the link pool project failed:\n${output}")
endif()
file(READ "${work}/pool-build/build.ninja" build_ninja)
file(READ "${work}/pool-build/CMakeFiles/rules.ninja" rules_ninja)
string(REGEX MATCH "pool ${REALMMESH_NINJA_LINK_POOL}\n  depth = 1\n" pool_declaration
    "${rules_ninja}${build_ninja}")
if(pool_declaration STREQUAL "")
    message(FATAL_ERROR "the generated Ninja files do not declare pool ${REALMMESH_NINJA_LINK_POOL} with depth 1")
endif()
# Link comments name the output file (sub/libpool_static.a, pool_main, ...).
foreach(target pool_main pool_static pool_shared)
    string(REGEX MATCH "# Link the [^\n]*${target}[^\n]*\n[^#]*" link_edge "${build_ninja}")
    string(FIND "${link_edge}" "pool = ${REALMMESH_NINJA_LINK_POOL}" position)
    if(link_edge STREQUAL "" OR position EQUAL -1)
        message(FATAL_ERROR "the link edge of ${target} is not in pool ${REALMMESH_NINJA_LINK_POOL}:\n${link_edge}")
    endif()
    message(STATUS "ok: ${target} links in pool ${REALMMESH_NINJA_LINK_POOL}")
endforeach()
string(REGEX MATCH "# Link the [^\n]*pool_free[^\n]*\n[^#]*" link_edge "${build_ninja}")
string(FIND "${link_edge}" "pool = " position)
if(link_edge STREQUAL "" OR NOT position EQUAL -1)
    message(FATAL_ERROR "the link edge of pool_free must not be in a pool:\n${link_edge}")
endif()
message(STATUS "ok: pool_free links outside the pool")

# --- libsodium install manifest ------------------------------------------------
function(check_sodium_tree label tree out_error)
    execute_process(
        COMMAND "${REALM_MESH_TEST_CMAKE_COMMAND}" "-DREALMMESH_SODIUM_INSTALL_DIR=${tree}"
            -P "${sodium_manifest}"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output
    )
    if(status EQUAL 0)
        set(${out_error} "" PARENT_SCOPE)
    else()
        set(${out_error} "${output}" PARENT_SCOPE)
    endif()
endfunction()

set(tree "${work}/sodium-install")
foreach(file IN LISTS REALMMESH_SODIUM_INSTALLED_FILES)
    file(WRITE "${tree}/${file}" "")
endforeach()
# Installed but not consumed: not part of the manifest, never flagged.
file(WRITE "${tree}/lib/libsodium.la" "")
file(WRITE "${tree}/lib/pkgconfig/libsodium.pc" "")
check_sodium_tree("complete tree" "${tree}" error)
expect_ok("libsodium tree matching the manifest" "${error}")

file(WRITE "${tree}/include/sodium/crypto_new.h" "")
check_sodium_tree("extra header" "${tree}" error)
expect_error("undeclared libsodium header" "${error}" "include/sodium/crypto_new.h")
file(REMOVE "${tree}/include/sodium/crypto_new.h")

file(REMOVE "${tree}/include/sodium/crypto_box.h")
check_sodium_tree("missing header" "${tree}" error)
expect_error("missing libsodium header" "${error}" "include/sodium/crypto_box.h")
