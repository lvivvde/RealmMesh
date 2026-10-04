# Isolation of generic cache keys shared by vendored dependencies (#121).
#
# realmmesh_cache_isolation_begin(<context> <key>...)
# realmmesh_cache_isolation_end(<key>...)
#
# Some upstream projects read the same generic name with different meanings.
# mongo-c-driver keeps BUILD_VERSION if it is defined, while mongo-cxx-driver
# declares BUILD_VERSION=0.0.0 in the cache; once that entry exists the next
# configure feeds 0.0.0 to mongo-c-driver, rewrites its version headers and
# rebuilds every file that includes them.
#
# The caller brackets the dependencies with begin/end and hands each one its
# value as a normal variable. A normal variable only shadows a cache entry
# under CMP0126 NEW (CMake 3.21+); the 3.20 minimum still runs the OLD
# behaviour, where `set(... CACHE ...)` removes the normal variable. So the
# keys must never persist in the cache:
#   * begin fails when a key is already cached: it is a leftover from an older
#     RealmMesh configure or an interrupted one, or a generic `-D<key>`
#     override that would leak into every dependency, and the message names
#     the `-U<key>` migration;
#   * end removes whatever entry the dependencies declared, leaving the cache
#     as it was before begin.
#
# realmmesh_cache_migration_hint(<out_variable> <flags>)
#
# Formats the reconfigure commands that apply <flags> (`-U<key>` and the like)
# to the current build directory, directly and through a preset. Every stale
# cache diagnostic in the build ends with it.
#
# This file has no project dependencies on purpose: the probe under
# tests/cmake replays the upstream statements against a real CMakeCache.
function(realmmesh_cache_migration_hint out_variable flags)
    set(${out_variable}
        "    cmake -S \"${CMAKE_SOURCE_DIR}\" -B \"${CMAKE_BINARY_DIR}\"${flags}\nor, through a preset:\n    cmake --preset <name>${flags}"
        PARENT_SCOPE)
endfunction()

function(realmmesh_cache_isolation_begin context)
    set(_entries "")
    set(_unset_flags "")
    foreach(_key IN LISTS ARGN)
        if(DEFINED CACHE{${_key}})
            get_property(_type CACHE "${_key}" PROPERTY TYPE)
            string(APPEND _entries "\n    ${_key}:${_type}=$CACHE{${_key}}")
            string(APPEND _unset_flags " -U${_key}")
        endif()
    endforeach()
    if(_entries STREQUAL "")
        return()
    endif()
    realmmesh_cache_migration_hint(_hint "${_unset_flags}")
    message(FATAL_ERROR
        "${context} owns these cache keys and sets them per dependency, but the "
        "cache already holds:${_entries}\n"
        "They are left over from an older or interrupted RealmMesh configure, or "
        "come from a generic -D override that would leak into every dependency; "
        "per-dependency values are not configurable. Remove them and "
        "configure again:\n${_hint}")
endfunction()

function(realmmesh_cache_isolation_end)
    foreach(_key IN LISTS ARGN)
        unset(${_key} CACHE)
    endforeach()
endfunction()
