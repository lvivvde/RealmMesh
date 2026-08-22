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
find_library(MSQUIC_XDP_LIBRARY NAMES xdp libxdp.so.1)
find_library(MSQUIC_NL3_LIBRARY NAMES nl-3 libnl-3.so.200)
find_library(MSQUIC_NL_ROUTE_LIBRARY NAMES nl-route-3 libnl-route-3.so.200)
find_library(MSQUIC_NUMA_LIBRARY NAMES numa libnuma.so.1)
if(NOT MSQUIC_XDP_LIBRARY OR NOT MSQUIC_NL3_LIBRARY OR
   NOT MSQUIC_NL_ROUTE_LIBRARY OR NOT MSQUIC_NUMA_LIBRARY)
    message(FATAL_ERROR
        "MsQuic runtime dependencies were not found (libxdp, libnl-3, "
        "libnl-route-3, libnuma). Install the dependencies of libmsquic."
    )
endif()
set_target_properties(MsQuic::MsQuic PROPERTIES
    IMPORTED_LOCATION "${MSQUIC_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${MSQUIC_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES
        "${MSQUIC_XDP_LIBRARY};${MSQUIC_NL_ROUTE_LIBRARY};${MSQUIC_NL3_LIBRARY};${MSQUIC_NUMA_LIBRARY}"
    INTERFACE_LINK_OPTIONS
        "-Wl,--no-as-needed;-Wl,-rpath,$<SHELL_PATH:${MSQUIC_ROOT}/lib/x86_64-linux-gnu>;-Wl,-rpath,$<SHELL_PATH:${CMAKE_LIBRARY_PATH}>"
)
