# 开发脚本在缺少 setsid(1) 的平台(macOS)上用它把服务放进新的会话。
add_executable(realm_detach main.cpp)

target_compile_features(realm_detach PRIVATE cxx_std_20)
set_target_properties(realm_detach PROPERTIES
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(MSVC)
    target_compile_options(realm_detach PRIVATE /W4 /permissive-)
else()
    target_compile_options(realm_detach PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
endif()
