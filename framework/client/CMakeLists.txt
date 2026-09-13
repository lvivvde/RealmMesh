add_library(realm_client
    src/adaptive_poller.cpp
    src/login_chain.cpp
    src/login_stage.cpp
    src/wire_login_transport.cpp
)
add_library(RealmMesh::Client ALIAS realm_client)

target_include_directories(realm_client
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(realm_client PUBLIC cxx_std_20)
set_target_properties(realm_client PROPERTIES CXX_EXTENSIONS OFF)

# 自研 TLS/TCP 传输 + HTTP/1.1 与 edge 帧客户端(ADR-0007):链路不引入
# 第三方 HTTP 栈。Realm 段与网关段共用竞速与拨号器。
target_link_libraries(realm_client
    PUBLIC RealmMesh::Network
    PRIVATE RealmMesh::GameCommon
)

if(MSVC)
    target_compile_options(realm_client PRIVATE /W4 /permissive-)
else()
    target_compile_options(realm_client PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
    )
endif()
