include(FetchContent)

FetchContent_Declare(
    sol2_source
    URL https://codeload.github.com/ThePhD/sol2/tar.gz/refs/tags/v3.3.1
    URL_HASH SHA256=ad121461047d52b449aa84234a6b578fa3ed95d67d1a0703902eba72417f61bb
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_GetProperties(sol2_source)
if(NOT sol2_source_POPULATED)
    if(POLICY CMP0169)
        cmake_policy(PUSH)
        cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Populate(sol2_source)
    if(POLICY CMP0169)
        cmake_policy(POP)
    endif()
endif()

add_library(realm_sol2 INTERFACE)
add_library(Sol2::Sol2 ALIAS realm_sol2)

target_include_directories(realm_sol2 SYSTEM INTERFACE
    ${sol2_source_SOURCE_DIR}/include
)
target_link_libraries(realm_sol2 INTERFACE Lua::Lua)
target_compile_features(realm_sol2 INTERFACE cxx_std_17)
