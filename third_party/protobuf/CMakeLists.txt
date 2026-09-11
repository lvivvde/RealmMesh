include(FetchContent)

# Protobuf otherwise fetches Abseil through an unpinned Git checkout during
# configuration. Declare the exact version required by Protobuf v35 here so
# RealmMesh builds remain reproducible and do not depend on global Git mirrors.
set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    absl
    URL https://github.com/abseil/abseil-cpp/archive/refs/tags/20250512.1.tar.gz
    URL_HASH SHA256=9b7a064305e9fd94d124ffa6cc358592eb42b5da588fb4e07d09254aa40086db
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(absl)

# RealmMesh only needs protoc for code generation, never libprotoc. Pointing
# WITH_PROTOC at a release binary makes protobuf create an imported
# protobuf::protoc target and skip building libprotoc -- which on macOS fails to
# compile upb's bootstrap reflection headers (google/protobuf/descriptor.upb.h
# is only generated for the normal, non-bootstrap build). Hosts without a
# prebuilt binary still build protoc from source.
set(realmmesh_protoc_url "")
set(realmmesh_protoc_sha256 "")
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND
   CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    set(realmmesh_protoc_url
        "https://github.com/protocolbuffers/protobuf/releases/download/v35.0/protoc-35.0-linux-x86_64.zip")
    set(realmmesh_protoc_sha256
        a45cda0989c17dd950db55f6fbe1e5814c50fda08e87aa422980ac1f89dddbbc)
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND
       CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    set(realmmesh_protoc_url
        "https://github.com/protocolbuffers/protobuf/releases/download/v35.0/protoc-35.0-osx-aarch_64.zip")
    set(realmmesh_protoc_sha256
        45444963204757fd3e2fbe304bc1fdadfb488d8556ff099c4cc06575eab88976)
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND
       CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    set(realmmesh_protoc_url
        "https://github.com/protocolbuffers/protobuf/releases/download/v35.0/protoc-35.0-osx-x86_64.zip")
    set(realmmesh_protoc_sha256
        3580c2d115fccb5b0239960c8f70f8da14787b1973a46b2f39c315ad71c11e01)
endif()

if(realmmesh_protoc_url)
    FetchContent_Declare(
        realmmesh_protoc
        URL "${realmmesh_protoc_url}"
        URL_HASH SHA256=${realmmesh_protoc_sha256}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(realmmesh_protoc)
    set(WITH_PROTOC "${realmmesh_protoc_SOURCE_DIR}/bin/protoc")
    file(CHMOD "${WITH_PROTOC}"
        PERMISSIONS
            OWNER_READ OWNER_WRITE OWNER_EXECUTE
            GROUP_READ GROUP_EXECUTE
            WORLD_READ WORLD_EXECUTE
    )
endif()

set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_CONFORMANCE OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_PROTOBUF_BINARIES ON CACHE BOOL "" FORCE)
set(protobuf_BUILD_PROTOC_BINARIES ON CACHE BOOL "" FORCE)
set(protobuf_BUILD_LIBUPB OFF CACHE BOOL "" FORCE)
set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
set(protobuf_WITH_ZLIB OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    protobuf
    URL https://github.com/protocolbuffers/protobuf/releases/download/v35.0/protobuf-35.0.tar.gz
    URL_HASH SHA256=8f907baca4b34a3b4854103ba5811e418fb6e2ff11fe0d8df9e8280b11d79926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# Protobuf v35 explicitly selects CMP0141 OLD for MSVC compatibility, which
# CMake 4.x reports as deprecated even on non-MSVC builds. Suppress only the
# dependency's deprecation diagnostics and restore the project setting below.
set(realmmesh_warn_deprecated_was_defined FALSE)
if(DEFINED CMAKE_WARN_DEPRECATED)
    set(realmmesh_warn_deprecated_was_defined TRUE)
    set(realmmesh_saved_warn_deprecated "${CMAKE_WARN_DEPRECATED}")
endif()
set(CMAKE_WARN_DEPRECATED OFF)
FetchContent_MakeAvailable(protobuf)
if(realmmesh_warn_deprecated_was_defined)
    set(CMAKE_WARN_DEPRECATED "${realmmesh_saved_warn_deprecated}")
else()
    unset(CMAKE_WARN_DEPRECATED)
endif()

# RealmMesh links only the lite runtime. Keep the reflection-heavy full runtime
# out of the default build while leaving its target available to future tools.
if(TARGET libprotobuf)
    set_target_properties(libprotobuf PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()
