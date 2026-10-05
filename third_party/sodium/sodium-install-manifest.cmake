# libsodium 1.0.22 安装产物清单(#123):相对 INSTALL_DIR、本仓消费的那部分——全部公共头
# (取自 src/libsodium/include/Makefile.am 的 nobase_include_HEADERS 与
# nobase_nodist_include_HEADERS)和静态库。libsodium.la、pkgconfig 不被消费，不列。
#
# third_party/sodium/CMakeLists.txt 把它们声明为外部构建的 BUILD_BYPRODUCTS,Ninja 才知道
# 谁产出这些文件：删掉任意一个都会重跑 make install 补回，并在同一轮里重编/重链下游。
#
# 以 `cmake -DREALMMESH_SODIUM_INSTALL_DIR=<dir> -P` 运行时，核对 <dir> 下实际装出的头文件
# 与静态库和清单一致：外部构建在 make install 之后跑这一步，升级 libsodium 时清单漏改即
# 构建失败，而不是留下一个删了不能恢复的头。
set(REALMMESH_SODIUM_INSTALLED_FILES
    include/sodium.h
    include/sodium/core.h
    include/sodium/crypto_aead_aes256gcm.h
    include/sodium/crypto_aead_aegis128l.h
    include/sodium/crypto_aead_aegis256.h
    include/sodium/crypto_aead_chacha20poly1305.h
    include/sodium/crypto_aead_xchacha20poly1305.h
    include/sodium/crypto_auth.h
    include/sodium/crypto_auth_hmacsha256.h
    include/sodium/crypto_auth_hmacsha512.h
    include/sodium/crypto_auth_hmacsha512256.h
    include/sodium/crypto_box.h
    include/sodium/crypto_box_curve25519xchacha20poly1305.h
    include/sodium/crypto_box_curve25519xsalsa20poly1305.h
    include/sodium/crypto_core_ed25519.h
    include/sodium/crypto_core_ristretto255.h
    include/sodium/crypto_core_hchacha20.h
    include/sodium/crypto_core_hsalsa20.h
    include/sodium/crypto_core_keccak1600.h
    include/sodium/crypto_core_salsa20.h
    include/sodium/crypto_core_salsa2012.h
    include/sodium/crypto_core_salsa208.h
    include/sodium/crypto_generichash.h
    include/sodium/crypto_generichash_blake2b.h
    include/sodium/crypto_hash.h
    include/sodium/crypto_hash_sha256.h
    include/sodium/crypto_hash_sha3.h
    include/sodium/crypto_hash_sha512.h
    include/sodium/crypto_ipcrypt.h
    include/sodium/crypto_kdf.h
    include/sodium/crypto_kdf_hkdf_sha256.h
    include/sodium/crypto_kdf_hkdf_sha512.h
    include/sodium/crypto_kdf_blake2b.h
    include/sodium/crypto_kem.h
    include/sodium/crypto_kem_mlkem768.h
    include/sodium/crypto_kem_xwing.h
    include/sodium/crypto_kx.h
    include/sodium/crypto_onetimeauth.h
    include/sodium/crypto_onetimeauth_poly1305.h
    include/sodium/crypto_pwhash.h
    include/sodium/crypto_pwhash_argon2i.h
    include/sodium/crypto_pwhash_argon2id.h
    include/sodium/crypto_pwhash_scryptsalsa208sha256.h
    include/sodium/crypto_scalarmult.h
    include/sodium/crypto_scalarmult_curve25519.h
    include/sodium/crypto_scalarmult_ed25519.h
    include/sodium/crypto_scalarmult_ristretto255.h
    include/sodium/crypto_secretbox.h
    include/sodium/crypto_secretbox_xchacha20poly1305.h
    include/sodium/crypto_secretbox_xsalsa20poly1305.h
    include/sodium/crypto_secretstream_xchacha20poly1305.h
    include/sodium/crypto_shorthash.h
    include/sodium/crypto_shorthash_siphash24.h
    include/sodium/crypto_sign.h
    include/sodium/crypto_sign_ed25519.h
    include/sodium/crypto_sign_edwards25519sha512batch.h
    include/sodium/crypto_stream.h
    include/sodium/crypto_stream_chacha20.h
    include/sodium/crypto_stream_salsa20.h
    include/sodium/crypto_stream_salsa2012.h
    include/sodium/crypto_stream_salsa208.h
    include/sodium/crypto_stream_xchacha20.h
    include/sodium/crypto_stream_xsalsa20.h
    include/sodium/crypto_verify_16.h
    include/sodium/crypto_verify_32.h
    include/sodium/crypto_verify_64.h
    include/sodium/crypto_xof_shake128.h
    include/sodium/crypto_xof_shake256.h
    include/sodium/crypto_xof_turboshake128.h
    include/sodium/crypto_xof_turboshake256.h
    include/sodium/export.h
    include/sodium/randombytes.h
    include/sodium/randombytes_internal_random.h
    include/sodium/randombytes_sysrandom.h
    include/sodium/runtime.h
    include/sodium/utils.h
    include/sodium/version.h
    lib/libsodium.a
)

if(DEFINED CMAKE_SCRIPT_MODE_FILE AND CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    if(NOT IS_DIRECTORY "${REALMMESH_SODIUM_INSTALL_DIR}")
        message(FATAL_ERROR "REALMMESH_SODIUM_INSTALL_DIR='${REALMMESH_SODIUM_INSTALL_DIR}' is not a directory")
    endif()
    file(GLOB_RECURSE _installed LIST_DIRECTORIES false RELATIVE "${REALMMESH_SODIUM_INSTALL_DIR}"
        "${REALMMESH_SODIUM_INSTALL_DIR}/include/*")
    if(EXISTS "${REALMMESH_SODIUM_INSTALL_DIR}/lib/libsodium.a")
        list(APPEND _installed lib/libsodium.a)
    endif()
    set(_undeclared ${_installed})
    list(REMOVE_ITEM _undeclared ${REALMMESH_SODIUM_INSTALLED_FILES})
    set(_missing ${REALMMESH_SODIUM_INSTALLED_FILES})
    if(_installed)
        list(REMOVE_ITEM _missing ${_installed})
    endif()
    if(_undeclared OR _missing)
        list(JOIN _undeclared " " _undeclared)
        list(JOIN _missing " " _missing)
        message(FATAL_ERROR
            "libsodium install tree ${REALMMESH_SODIUM_INSTALL_DIR} does not match "
            "${CMAKE_CURRENT_LIST_FILE}; update REALMMESH_SODIUM_INSTALLED_FILES.\n"
            "  installed but not declared: ${_undeclared}\n"
            "  declared but not installed: ${_missing}")
    endif()
endif()
