include(FetchContent)

FetchContent_Declare(
    sol2_source
    URL https://codeload.github.com/ThePhD/sol2/tar.gz/refs/tags/v3.3.1
    URL_HASH SHA256=ad121461047d52b449aa84234a6b578fa3ed95d67d1a0703902eba72417f61bb
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    # optional<T&>::emplace 调用了特化里不存在的 construct 成员,
    # clang 17+ 严格两阶段查找在解析期报错(GCC 不触发);对齐上游 develop 的修复。
    PATCH_COMMAND patch -p1 -i
        ${CMAKE_CURRENT_LIST_DIR}/patches/fix-optional-ref-emplace-for-new-clang.patch
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
