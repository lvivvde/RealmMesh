add_library(realm_scripting
    src/lua_runtime.cpp
)

add_library(RealmMesh::Scripting ALIAS realm_scripting)

target_include_directories(realm_scripting
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(realm_scripting PUBLIC Sol2::Sol2)
target_compile_features(realm_scripting PUBLIC cxx_std_20)
set_target_properties(realm_scripting PROPERTIES CXX_EXTENSIONS OFF)

if(MSVC)
    target_compile_options(realm_scripting PRIVATE /W4 /permissive-)
else()
    target_compile_options(realm_scripting PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
    )
endif()
