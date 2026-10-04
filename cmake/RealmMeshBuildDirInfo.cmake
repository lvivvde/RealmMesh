# 构建目录信息(#122):配置期把「预设名 → 真实 binaryDir」记下来,供脚本入口
# 按 --preset 找构建目录。CMake 预设的继承、宏展开与 CMakeUserPresets.json 只有
# CMake 自己解析得对，所以由配置过程写出结果，脚本只读结果、不解析预设 JSON。
#
# 预设通过缓存变量 REALMMESH_PRESET 传入名字(CMakePresets.json 的隐藏基础预设
# 设为 ${presetName},派生预设自动得到自己的名字)。不经预设的配置不定义它，此时
# 不写任何记录。
#
# 写出两份相同内容的键值文本:
#   build/.build-dirs/<preset>.txt          源码树下的登记，脚本按预设名查找
#   <binaryDir>/realmmesh-build-dir.txt     构建目录自带的身份，用来核对登记
# 内容不变时不重写，免得每次配置都刷新时间戳。

set(REALMMESH_BUILD_DIR_INFO_FILE_NAME "realmmesh-build-dir.txt")
# 与 scripts/lib/build-dir.sh 的校验保持一致：名字会成为文件名。
set(REALMMESH_PRESET_NAME_REGEX "^[A-Za-z0-9][A-Za-z0-9_.+-]*$")

function(_realmmesh_write_if_changed path content)
    if(EXISTS "${path}")
        file(READ "${path}" existing)
        if(existing STREQUAL content)
            return()
        endif()
    endif()
    file(WRITE "${path}" "${content}")
endfunction()

function(realmmesh_write_build_dir_info)
    if(NOT DEFINED REALMMESH_PRESET OR REALMMESH_PRESET STREQUAL "")
        return()
    endif()
    if(NOT REALMMESH_PRESET MATCHES "${REALMMESH_PRESET_NAME_REGEX}")
        message(WARNING
            "REALMMESH_PRESET \"${REALMMESH_PRESET}\" is not a usable preset "
            "name (letters, digits and _ . + -, starting with a letter or "
            "digit); build directory info was not recorded.")
        return()
    endif()

    set(identity_file
        "${CMAKE_BINARY_DIR}/${REALMMESH_BUILD_DIR_INFO_FILE_NAME}")
    if(EXISTS "${identity_file}")
        file(STRINGS "${identity_file}" previous_preset REGEX "^preset=")
        string(REGEX REPLACE "^preset=" "" previous_preset "${previous_preset}")
        if(NOT previous_preset STREQUAL "" AND
           NOT previous_preset STREQUAL REALMMESH_PRESET)
            message(WARNING
                "${CMAKE_BINARY_DIR} was previously configured by preset "
                "\"${previous_preset}\" and is now configured by preset "
                "\"${REALMMESH_PRESET}\"; the two presets share one cache. "
                "Give each preset its own binaryDir.")
        endif()
    endif()

    string(CONCAT content
        "preset=${REALMMESH_PRESET}\n"
        "source_dir=${PROJECT_SOURCE_DIR}\n"
        "binary_dir=${CMAKE_BINARY_DIR}\n"
        "generator=${CMAKE_GENERATOR}\n")
    _realmmesh_write_if_changed("${identity_file}" "${content}")
    _realmmesh_write_if_changed(
        "${PROJECT_SOURCE_DIR}/build/.build-dirs/${REALMMESH_PRESET}.txt"
        "${content}")
    message(STATUS
        "realm_build_dir: preset ${REALMMESH_PRESET} -> ${CMAKE_BINARY_DIR}")
endfunction()
