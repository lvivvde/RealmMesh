include(ExternalProject)

set(sodium_install_dir "${CMAKE_CURRENT_BINARY_DIR}/install")
file(MAKE_DIRECTORY "${sodium_install_dir}/include")

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
    BUILD_COMMAND $(MAKE)
    INSTALL_COMMAND $(MAKE) install
    INSTALL_DIR "${sodium_install_dir}"
    BUILD_BYPRODUCTS "${sodium_install_dir}/lib/libsodium.a"
)

add_library(realm_sodium STATIC IMPORTED GLOBAL)
add_library(Sodium::Sodium ALIAS realm_sodium)
set_target_properties(realm_sodium PROPERTIES
    IMPORTED_LOCATION "${sodium_install_dir}/lib/libsodium.a"
    INTERFACE_INCLUDE_DIRECTORIES "${sodium_install_dir}/include"
)
add_dependencies(realm_sodium sodium_external)
