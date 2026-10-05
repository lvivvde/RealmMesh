include(ExternalProject)

# libsodium 走自己的 Autotools/Make,build/install 明确串行(#125):不用 $(MAKE)
# 加入顶层 make 的 jobserver,也清掉继承的 MAKEFLAGS,顶层 --jobs 不乘进外部构建。
find_program(REALMMESH_SODIUM_MAKE NAMES make gmake REQUIRED)
mark_as_advanced(REALMMESH_SODIUM_MAKE)
set(sodium_make
    "${CMAKE_COMMAND}" -E env --unset=MAKEFLAGS --unset=MFLAGS --unset=MAKELEVEL
    "${REALMMESH_SODIUM_MAKE}" -j1)

set(sodium_install_dir "${CMAKE_CURRENT_BINARY_DIR}/install")
file(MAKE_DIRECTORY "${sodium_install_dir}/include")

# make install 并进构建步，装出的头与静态库作为构建步的 BUILD_BYPRODUCTS(#123):Ninja 由此
# 知道它们的产出者，删掉任一文件都会重跑这一步补回，下游在同一轮里重编/重链。单独的安装步
# 声明产物要 INSTALL_BYPRODUCTS(CMake 3.26),最低版本 3.20 用不了。装完按清单核对，
# 升级 libsodium 时清单漏改即构建失败。
include("${CMAKE_CURRENT_SOURCE_DIR}/sodium-install-manifest.cmake")
list(TRANSFORM REALMMESH_SODIUM_INSTALLED_FILES PREPEND "${sodium_install_dir}/"
    OUTPUT_VARIABLE sodium_byproducts)

ExternalProject_Add(
    sodium_external
    URL https://github.com/jedisct1/libsodium/releases/download/1.0.22-RELEASE/libsodium-1.0.22.tar.gz
    URL_HASH SHA256=adbdd8f16149e81ac6078a03aca6fc03b592b89ef7b5ed83841c086191be3349
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    UPDATE_DISCONNECTED TRUE
    CONFIGURE_COMMAND
        <SOURCE_DIR>/configure
        --prefix=<INSTALL_DIR>
        --disable-shared
        --enable-static
        --with-pic
    BUILD_COMMAND ${sodium_make}
        COMMAND ${sodium_make} install
        COMMAND "${CMAKE_COMMAND}" "-DREALMMESH_SODIUM_INSTALL_DIR=${sodium_install_dir}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/sodium-install-manifest.cmake"
    INSTALL_COMMAND ""
    INSTALL_DIR "${sodium_install_dir}"
    BUILD_BYPRODUCTS ${sodium_byproducts}
)

add_library(realm_sodium STATIC IMPORTED GLOBAL)
add_library(Sodium::Sodium ALIAS realm_sodium)
set_target_properties(realm_sodium PROPERTIES
    IMPORTED_LOCATION "${sodium_install_dir}/lib/libsodium.a"
    INTERFACE_INCLUDE_DIRECTORIES "${sodium_install_dir}/include"
)
add_dependencies(realm_sodium sodium_external)
