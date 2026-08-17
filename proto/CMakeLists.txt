set(realm_protocol_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(realm_protocol_sources
    realmmesh/common/v1/envelope.proto
    realmmesh/edge/v1/edge.proto
)
set(realm_protocol_generated_sources
    "${realm_protocol_generated_dir}/realmmesh/common/v1/envelope.pb.cc"
    "${realm_protocol_generated_dir}/realmmesh/edge/v1/edge.pb.cc"
)
set(realm_protocol_generated_headers
    "${realm_protocol_generated_dir}/realmmesh/common/v1/envelope.pb.h"
    "${realm_protocol_generated_dir}/realmmesh/edge/v1/edge.pb.h"
)

add_custom_command(
    OUTPUT
        ${realm_protocol_generated_sources}
        ${realm_protocol_generated_headers}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${realm_protocol_generated_dir}"
    COMMAND $<TARGET_FILE:protobuf::protoc>
        --proto_path=${CMAKE_CURRENT_SOURCE_DIR}
        --cpp_out=${realm_protocol_generated_dir}
        ${realm_protocol_sources}
    DEPENDS
        protobuf::protoc
        ${realm_protocol_sources}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating RealmMesh Protocol Buffer sources"
    VERBATIM
)

add_library(realm_protocol STATIC
    ${realm_protocol_generated_sources}
    ${realm_protocol_generated_headers}
)
add_library(RealmMesh::Protocol ALIAS realm_protocol)

target_include_directories(realm_protocol SYSTEM PUBLIC
    $<BUILD_INTERFACE:${realm_protocol_generated_dir}>
    $<BUILD_INTERFACE:${protobuf_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${protobuf_SOURCE_DIR}/third_party/utf8_range>
    $<BUILD_INTERFACE:${absl_SOURCE_DIR}>
)
target_link_libraries(realm_protocol PUBLIC
    protobuf::libprotobuf-lite
    utf8_validity
)
target_compile_features(realm_protocol PUBLIC cxx_std_20)
set_target_properties(realm_protocol PROPERTIES CXX_EXTENSIONS OFF)
