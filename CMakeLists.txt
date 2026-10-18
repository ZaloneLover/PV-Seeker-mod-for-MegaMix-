cmake_minimum_required(VERSION 3.20)
project(PvSeek CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Link the C++ runtime statically so the mod loads without any redistributable
# (this is what DivaModLoader recommends).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------------------
# Microsoft Detours
#
# No special cmake flags are needed. The build looks here, in this order:
#   1. -DDETOURS_DIR=<folder>         a folder with include/detours.h and lib/x64/detours.lib
#   2. <this folder>/third_party/Detours      (same layout as 1)
#   3. vcpkg, if you ran  vcpkg install detours:x64-windows-static
#        looked up in  -DVCPKG_DIR=<folder>, then the VCPKG_ROOT environment variable,
#        then C:/vcpkg   (it works WITHOUT the vcpkg toolchain file)
#   4. anything CMake finds by itself (for example when you do pass the vcpkg toolchain file)
# ---------------------------------------------------------------------------------------
set(DETOURS_DIR "" CACHE PATH "Folder with include/detours.h and lib/x64/detours.lib")
set(VCPKG_DIR   "" CACHE PATH "Your vcpkg folder (only if it is not C:/vcpkg and VCPKG_ROOT is not set)")

set(DETOURS_FOUND OFF)
set(_detours_tried "")

# 1. explicit folder - if given, it must be right (no silent fallback)
if(DETOURS_DIR)
    if(EXISTS "${DETOURS_DIR}/include/detours.h" AND EXISTS "${DETOURS_DIR}/lib/x64/detours.lib")
        set(DETOURS_INCLUDE_DIR "${DETOURS_DIR}/include")
        set(DETOURS_LIBRARY "${DETOURS_DIR}/lib/x64/detours.lib")
        set(DETOURS_FOUND ON)
    else()
        message(FATAL_ERROR
            "DETOURS_DIR is set to '${DETOURS_DIR}' but include/detours.h or lib/x64/detours.lib is missing there.")
    endif()
endif()

# 2. folder next to this file
if(NOT DETOURS_FOUND)
    set(_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Detours")
    string(APPEND _detours_tried "  ${_dir}   (needs include/detours.h and lib/x64/detours.lib)\n")
    if(EXISTS "${_dir}/include/detours.h" AND EXISTS "${_dir}/lib/x64/detours.lib")
        set(DETOURS_INCLUDE_DIR "${_dir}/include")
        set(DETOURS_LIBRARY "${_dir}/lib/x64/detours.lib")
        set(DETOURS_FOUND ON)
    endif()
endif()

# 3. vcpkg install folders, found without the toolchain file
if(NOT DETOURS_FOUND)
    set(_vcpkg_roots "")
    if(VCPKG_DIR)
        list(APPEND _vcpkg_roots "${VCPKG_DIR}")
    endif()
    if(DEFINED ENV{VCPKG_ROOT} AND NOT "$ENV{VCPKG_ROOT}" STREQUAL "")
        file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}" _env_root)
        list(APPEND _vcpkg_roots "${_env_root}")
    endif()
    list(APPEND _vcpkg_roots "C:/vcpkg")
    list(REMOVE_DUPLICATES _vcpkg_roots)

    foreach(_root IN LISTS _vcpkg_roots)
        foreach(_triplet x64-windows-static x64-windows)
            set(_inst "${_root}/installed/${_triplet}")
            string(APPEND _detours_tried "  ${_inst}   (needs include/detours/detours.h and lib/detours.lib)\n")
            if(NOT DETOURS_FOUND
               AND EXISTS "${_inst}/lib/detours.lib"
               AND (EXISTS "${_inst}/include/detours/detours.h" OR EXISTS "${_inst}/include/detours.h"))
                set(DETOURS_INCLUDE_DIR "${_inst}/include")
                set(DETOURS_LIBRARY "${_inst}/lib/detours.lib")
                set(DETOURS_FOUND ON)
            endif()
        endforeach()
    endforeach()
endif()

# 4. whatever CMake can find on its own (vcpkg toolchain file, CMAKE_PREFIX_PATH, ...)
if(NOT DETOURS_FOUND)
    find_path(DETOURS_FOUND_INCLUDE NAMES detours/detours.h detours.h)
    find_library(DETOURS_FOUND_LIBRARY NAMES detours)
    string(APPEND _detours_tried "  CMake's own search (CMAKE_PREFIX_PATH / toolchain file)\n")
    if(DETOURS_FOUND_INCLUDE AND DETOURS_FOUND_LIBRARY)
        set(DETOURS_INCLUDE_DIR "${DETOURS_FOUND_INCLUDE}")
        set(DETOURS_LIBRARY "${DETOURS_FOUND_LIBRARY}")
        set(DETOURS_FOUND ON)
    endif()
endif()

if(NOT DETOURS_FOUND)
    message(FATAL_ERROR
        "Detours was not found.\n"
        "I looked in:\n${_detours_tried}\n"
        "Fix - pick one:\n"
        "  1. vcpkg:   C:\\vcpkg\\vcpkg install detours:x64-windows-static\n"
        "     then DELETE the 'build' folder (CMake remembers old results) and run cmake again.\n"
        "     If vcpkg is not in C:/vcpkg, set the VCPKG_ROOT environment variable or add \"-DVCPKG_DIR=<your vcpkg folder>\".\n"
        "  2. Put a folder with include/detours.h and lib/x64/detours.lib at\n"
        "     ${CMAKE_CURRENT_SOURCE_DIR}/third_party/Detours\n"
        "  3. Or pass \"-DDETOURS_DIR=<that folder>\".\n"
        "In PowerShell, put the WHOLE -D option in quotes, as shown above.")
endif()

message(STATUS "Detours include folder: ${DETOURS_INCLUDE_DIR}")
message(STATUS "Detours library:        ${DETOURS_LIBRARY}")

add_library(PvSeek SHARED src/PvSeek.cpp)
target_include_directories(PvSeek PRIVATE ${DETOURS_INCLUDE_DIR})
target_link_libraries(PvSeek PRIVATE ${DETOURS_LIBRARY} comctl32 user32)
set_target_properties(PvSeek PROPERTIES PREFIX "")

# Put PvSeek.dll, config.toml and pvseek.ini together, ready to copy into  <game>/mods/PvSeek/
add_custom_command(TARGET PvSeek POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/config.toml $<TARGET_FILE_DIR:PvSeek>/config.toml
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/pvseek.ini  $<TARGET_FILE_DIR:PvSeek>/pvseek.ini)
