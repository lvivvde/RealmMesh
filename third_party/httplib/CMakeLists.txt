include(FetchContent)

FetchContent_Declare(
    cpp_httplib_source
    URL https://codeload.github.com/yhirose/cpp-httplib/tar.gz/refs/tags/v0.51.0
    URL_HASH SHA256=d740ced75352f44e9d66d08806dc231b5621bb3592b5a5d5b2bd890a9a9d86cc
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_GetProperties(cpp_httplib_source)
if(NOT cpp_httplib_source_POPULATED)
    if(POLICY CMP0169)
        cmake_policy(PUSH)
        cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Populate(cpp_httplib_source)
    if(POLICY CMP0169)
        cmake_policy(POP)
    endif()
endif()

add_library(realm_httplib INTERFACE)
add_library(httplib::httplib ALIAS realm_httplib)
target_include_directories(realm_httplib SYSTEM INTERFACE
    ${cpp_httplib_source_SOURCE_DIR}
)
target_compile_features(realm_httplib INTERFACE cxx_std_17)
