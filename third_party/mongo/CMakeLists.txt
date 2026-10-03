include(FetchContent)

# MongoDB C++ 驱动(mongocxx/bsoncxx)及其依赖的 C 驱动(libmongoc/libbson),
# 均以固定 release 源码包 + SHA256 构建为静态库(ADR-0011)。C 驱动在这里先行
# 声明：C++ 驱动发现 bson_static/mongoc_static 目标已存在时，就不会再按
# 未固定的 Git tag 自行下载。
#
# 以下变量都是本目录作用域的普通变量，只影响两个驱动子项目，不外溢到
# RealmMesh 其他目标(尤其 BUILD_SHARED_LIBS)。
set(fetch_args "")
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.25.0")
    # 驱动头文件按系统头处理，避免 RealmMesh 的 -Wconversion 等告警波及。
    list(APPEND fetch_args SYSTEM)
endif()

set(BUILD_TESTING OFF)
set(ENABLE_TESTS OFF)
set(ENABLE_EXAMPLES OFF)
set(ENABLE_UNINSTALL OFF)
set(ENABLE_STATIC ON)
set(ENABLE_SHARED OFF)
set(ENABLE_PIC ON)
# TLS 与 SCRAM 认证走项目已强制依赖的 OpenSSL 3;开发拓扑暂不开认证，
# 但跨机器暴露前须交付 TLS/认证，届时无需改动构建。
set(ENABLE_SSL OPENSSL)
set(ENABLE_SASL OFF)
set(ENABLE_SNAPPY OFF)
set(ENABLE_ZSTD OFF)
set(ENABLE_ZLIB OFF)
set(ENABLE_SRV OFF)
set(ENABLE_CLIENT_SIDE_ENCRYPTION OFF)
set(ENABLE_MONGODB_AWS_AUTH OFF)
set(ENABLE_SHM_COUNTERS OFF)
set(BUILD_SHARED_LIBS OFF)

# 两个驱动各有版本，却读同一个通用键 BUILD_VERSION:C 驱动沿用已定义的值，
# C++ 驱动把它声明为缓存项 0.0.0。缓存里一旦留下这一项，下次配置 C 驱动就
# 拿到 0.0.0,改写版本头并重编全部依赖它的源码(#121)。因此逐个驱动设为
# 本目录普通变量，并由 RealmMeshCacheIsolation 保证该键不进缓存。
set(realmmesh_mongo_c_version 2.5.5)
set(realmmesh_mongo_cxx_version 4.6.0)
include("${PROJECT_SOURCE_DIR}/cmake/RealmMeshCacheIsolation.cmake")
realmmesh_cache_isolation_begin("third_party/mongo" BUILD_VERSION)

FetchContent_Declare(
    mongo-c-driver
    URL https://github.com/mongodb/mongo-c-driver/releases/download/${realmmesh_mongo_c_version}/mongo-c-driver-${realmmesh_mongo_c_version}.tar.gz
    URL_HASH SHA256=fa255802fe748b98d4464e07223e87a44c88c02c35398d7607b2f02e967395b1
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    ${fetch_args}
)
set(BUILD_VERSION ${realmmesh_mongo_c_version})
FetchContent_MakeAvailable(mongo-c-driver)

FetchContent_Declare(
    mongo-cxx-driver
    URL https://github.com/mongodb/mongo-cxx-driver/releases/download/r${realmmesh_mongo_cxx_version}/mongo-cxx-driver-r${realmmesh_mongo_cxx_version}.tar.gz
    URL_HASH SHA256=eac122db0789fc82b0ba93f92a1503d74c502bfe4728345eaa8650e50a79da11
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    ${fetch_args}
)
set(BUILD_VERSION ${realmmesh_mongo_cxx_version})
FetchContent_MakeAvailable(mongo-cxx-driver)
unset(BUILD_VERSION)
realmmesh_cache_isolation_end(BUILD_VERSION)

add_library(realm_mongocxx INTERFACE)
add_library(Mongo::cxx ALIAS realm_mongocxx)
target_link_libraries(realm_mongocxx INTERFACE mongocxx_static bsoncxx_static)
