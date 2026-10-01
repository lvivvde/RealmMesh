# MsQuic 发现按平台分支(ADR-0012 修订 ADR-0002):
# - Linux 是生产基准,缺失 MsQuic 即配置失败,保证 linux CI 一定编入 QUIC;
# - macOS 自动检测 Homebrew libmsquic,找不到就只走 TLS/TCP,不让配置失败;
# - 其他平台只走 TLS/TCP。
# Linux 安装脚本固定 2.6.1,Homebrew 只提供当前 2.6.x,故只对齐到次版本。
set(MSQUIC_EXPECTED_SERIES "2.6")

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(MSQUIC_HOMEBREW_PREFIXES
        "/opt/homebrew/opt/libmsquic"
        "/usr/local/opt/libmsquic"
    )
    if(DEFINED ENV{HOMEBREW_PREFIX})
        list(PREPEND MSQUIC_HOMEBREW_PREFIXES "$ENV{HOMEBREW_PREFIX}/opt/libmsquic")
    endif()
    list(TRANSFORM MSQUIC_HOMEBREW_PREFIXES APPEND "/include"
        OUTPUT_VARIABLE MSQUIC_HOMEBREW_INCLUDE_HINTS)
    list(TRANSFORM MSQUIC_HOMEBREW_PREFIXES APPEND "/lib"
        OUTPUT_VARIABLE MSQUIC_HOMEBREW_LIBRARY_HINTS)

    find_path(MSQUIC_INCLUDE_DIR
        NAMES msquic.h
        HINTS "${MSQUIC_ROOT}/include" ${MSQUIC_HOMEBREW_INCLUDE_HINTS}
    )
    find_library(MSQUIC_LIBRARY
        NAMES msquic
        HINTS "${MSQUIC_ROOT}/lib" ${MSQUIC_HOMEBREW_LIBRARY_HINTS}
    )
    if(NOT MSQUIC_INCLUDE_DIR OR NOT MSQUIC_LIBRARY)
        set(REALM_MESH_MSQUIC_FOUND FALSE PARENT_SCOPE)
        message(STATUS
            "MsQuic: not found, QUIC disabled (TLS/TCP only). "
            "Enable it with `brew install libmsquic` and reconfigure, "
            "or set MSQUIC_ROOT (ADR-0012)."
        )
        return()
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_path(MSQUIC_INCLUDE_DIR
        NAMES msquic.h
        HINTS
            "${MSQUIC_ROOT}/include"
            "${MSQUIC_ROOT}/usr/include"
            "${PROJECT_SOURCE_DIR}/.tools/msquic/usr/include"
    )
    find_library(MSQUIC_LIBRARY
        NAMES msquic libmsquic.so.2
        HINTS
            "${MSQUIC_ROOT}/lib"
            "${MSQUIC_ROOT}/lib64"
            "${MSQUIC_ROOT}/usr/lib"
            "${MSQUIC_ROOT}/usr/lib/x86_64-linux-gnu"
            "${PROJECT_SOURCE_DIR}/.tools/msquic/usr/lib/x86_64-linux-gnu"
    )
    if(NOT MSQUIC_LIBRARY)
        foreach(MSQUIC_LIBRARY_CANDIDATE
            "${MSQUIC_ROOT}/lib/libmsquic.so.2"
            "${MSQUIC_ROOT}/lib64/libmsquic.so.2"
            "${MSQUIC_ROOT}/lib/x86_64-linux-gnu/libmsquic.so.2"
            "${PROJECT_SOURCE_DIR}/.tools/msquic/usr/lib/x86_64-linux-gnu/libmsquic.so.2"
            "${MSQUIC_ROOT}/usr/lib/libmsquic.so.2"
            "${MSQUIC_ROOT}/usr/lib/x86_64-linux-gnu/libmsquic.so.2"
        )
            if(EXISTS "${MSQUIC_LIBRARY_CANDIDATE}")
                set(MSQUIC_LIBRARY "${MSQUIC_LIBRARY_CANDIDATE}")
                break()
            endif()
        endforeach()
    endif()

    if(NOT MSQUIC_INCLUDE_DIR OR NOT MSQUIC_LIBRARY)
        message(FATAL_ERROR
            "MsQuic 2.x was not found. Install the official libmsquic package, "
            "provide msquic.h, and set MSQUIC_ROOT to their common prefix."
        )
    endif()

    # libmsquic 2.6 链接 libcrypto 与 libnuma;libcrypto 经 OpenSSL 已在链接行上。
    find_library(MSQUIC_NUMA_LIBRARY NAMES numa libnuma.so.1)
    if(NOT MSQUIC_NUMA_LIBRARY)
        message(FATAL_ERROR
            "MsQuic runtime dependency libnuma was not found. "
            "Install the dependencies of libmsquic."
        )
    endif()
else()
    set(REALM_MESH_MSQUIC_FOUND FALSE PARENT_SCOPE)
    message(STATUS "MsQuic: skipped on ${CMAKE_SYSTEM_NAME} (TLS/TCP only, see ADR-0002)")
    return()
endif()

add_library(MsQuic::MsQuic SHARED IMPORTED GLOBAL)
set_target_properties(MsQuic::MsQuic PROPERTIES
    IMPORTED_LOCATION "${MSQUIC_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${MSQUIC_INCLUDE_DIR}"
)
# macOS 的 dylib 自带绝对 install name,运行期无需 rpath;Linux 需要下面的接线。
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # 运行期定位:MsQuic 常装在非系统前缀(.tools/msquic 或自定义 MSQUIC_ROOT),
    # 把实际找到的库目录放进 rpath 最可靠;CMAKE_LIBRARY_PATH 未设置时跳过。
    # 空变量直接进生成器表达式会因「不是绝对路径」让 configure 失败(CI 裸
    # preset 时即如此),故先判空。
    get_filename_component(MSQUIC_LIBRARY_DIR "${MSQUIC_LIBRARY}" DIRECTORY)
    set(MSQUIC_INTERFACE_LINK_OPTIONS "-Wl,--no-as-needed")
    foreach(MSQUIC_RPATH_DIR "${MSQUIC_LIBRARY_DIR}" "${CMAKE_LIBRARY_PATH}")
        if(MSQUIC_RPATH_DIR)
            list(APPEND MSQUIC_INTERFACE_LINK_OPTIONS
                "-Wl,-rpath,${MSQUIC_RPATH_DIR}")
        endif()
    endforeach()
    set_target_properties(MsQuic::MsQuic PROPERTIES
        INTERFACE_LINK_LIBRARIES "${MSQUIC_NUMA_LIBRARY}"
        INTERFACE_LINK_OPTIONS "${MSQUIC_INTERFACE_LINK_OPTIONS}"
    )
endif()

# 版本取自库文件真实路径(libmsquic.so.2.6.1 / libmsquic.2.6.2.dylib);
# 漂移只告警,不让配置失败。
file(REAL_PATH "${MSQUIC_LIBRARY}" MSQUIC_LIBRARY_REAL_PATH)
get_filename_component(MSQUIC_LIBRARY_FILE_NAME "${MSQUIC_LIBRARY_REAL_PATH}" NAME)
if(MSQUIC_LIBRARY_FILE_NAME MATCHES "([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(MSQUIC_VERSION "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
    if(NOT "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}" VERSION_EQUAL MSQUIC_EXPECTED_SERIES)
        message(WARNING
            "MsQuic ${MSQUIC_VERSION} found at ${MSQUIC_LIBRARY_REAL_PATH}; "
            "the project is aligned on ${MSQUIC_EXPECTED_SERIES}.x. "
            "QUIC behaviour may differ from CI."
        )
    endif()
else()
    set(MSQUIC_VERSION "unknown version")
    message(WARNING
        "Cannot read the MsQuic version from ${MSQUIC_LIBRARY_REAL_PATH}; "
        "the project is aligned on ${MSQUIC_EXPECTED_SERIES}.x."
    )
endif()
message(STATUS "MsQuic: ${MSQUIC_VERSION} (${MSQUIC_LIBRARY_REAL_PATH})")

set(REALM_MESH_MSQUIC_FOUND TRUE PARENT_SCOPE)
