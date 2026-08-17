include(FetchContent)

FetchContent_Declare(
    kcp_source
    URL https://codeload.github.com/skywind3000/kcp/tar.gz/9aed788c9dcef45ded526e3606103af3e58cd68c
    URL_HASH SHA256=35e8bc1feefef47bec01db3f9b9aa91214141623bcd42a84de39c6db22307deb
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

if(POLICY CMP0169)
    cmake_policy(PUSH)
    cmake_policy(SET CMP0169 OLD)
endif()
FetchContent_GetProperties(kcp_source)
if(NOT kcp_source_POPULATED)
    FetchContent_Populate(kcp_source)
endif()
if(POLICY CMP0169)
    cmake_policy(POP)
endif()

add_library(realm_kcp STATIC ${kcp_source_SOURCE_DIR}/ikcp.c)
add_library(Kcp::Kcp ALIAS realm_kcp)

target_include_directories(realm_kcp SYSTEM PUBLIC
    ${kcp_source_SOURCE_DIR}
)

set_target_properties(realm_kcp PROPERTIES POSITION_INDEPENDENT_CODE ON)
