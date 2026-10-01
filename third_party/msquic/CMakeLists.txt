# MsQuic discovery is Linux-only: the package layout, runtime dependencies
# (libnuma) and linker flags are all Linux-specific, and the
# macOS/Windows port deliberately serves TLS/TCP only (ADR-0002).
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(REALM_MESH_MSQUIC_FOUND FALSE PARENT_SCOPE)
    message(STATUS "MsQuic: skipped on ${CMAKE_SYSTEM_NAME} (Linux-only, see ADR-0002)")
    return()
endif()

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

add_library(MsQuic::MsQuic SHARED IMPORTED GLOBAL)
# libmsquic 2.6 链接 libcrypto 与 libnuma;libcrypto 经 OpenSSL 已在链接行上。
find_library(MSQUIC_NUMA_LIBRARY NAMES numa libnuma.so.1)
if(NOT MSQUIC_NUMA_LIBRARY)
    message(FATAL_ERROR
        "MsQuic runtime dependency libnuma was not found. "
        "Install the dependencies of libmsquic."
    )
endif()
# 运行期定位:MsQuic 常装在非系统前缀(.tools/msquic 或自定义 MSQUIC_ROOT),
# 把实际找到的库目录放进 rpath 最可靠;CMAKE_LIBRARY_PATH 未设置时跳过。
# 本文件 Linux-only,$<SHELL_PATH> 是恒等变换;且空变量直接进生成器表达式
# 会因「不是绝对路径」让 configure 失败(CI 裸 preset 时即如此),故先判空。
get_filename_component(MSQUIC_LIBRARY_DIR "${MSQUIC_LIBRARY}" DIRECTORY)
set(MSQUIC_INTERFACE_LINK_OPTIONS "-Wl,--no-as-needed")
foreach(MSQUIC_RPATH_DIR "${MSQUIC_LIBRARY_DIR}" "${CMAKE_LIBRARY_PATH}")
    if(MSQUIC_RPATH_DIR)
        list(APPEND MSQUIC_INTERFACE_LINK_OPTIONS
            "-Wl,-rpath,${MSQUIC_RPATH_DIR}")
    endif()
endforeach()

set_target_properties(MsQuic::MsQuic PROPERTIES
    IMPORTED_LOCATION "${MSQUIC_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${MSQUIC_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES
        "${MSQUIC_NUMA_LIBRARY}"
    INTERFACE_LINK_OPTIONS "${MSQUIC_INTERFACE_LINK_OPTIONS}"
)

set(REALM_MESH_MSQUIC_FOUND TRUE PARENT_SCOPE)
