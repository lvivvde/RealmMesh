include(FetchContent)

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND
   CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    FetchContent_Declare(
        realmmesh_protoc
        URL https://github.com/protocolbuffers/protobuf/releases/download/v35.0/protoc-35.0-linux-x86_64.zip
        URL_HASH SHA256=a45cda0989c17dd950db55f6fbe1e5814c50fda08e87aa422980ac1f89dddbbc
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
FetchContent_MakeAvailable(protobuf)

# RealmMesh links only the lite runtime. Keep the reflection-heavy full runtime
# out of the default build while leaving its target available to future tools.
if(TARGET libprotobuf)
    set_target_properties(libprotobuf PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()
