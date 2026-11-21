cmake_minimum_required(VERSION 3.24)
project(dt_port C CXX)

# Links the runtime with the recompiler's output (generated/, never committed).
#
# Linux:
#   build/recomp/dt_recomp game/dt.elf generated
#   cmake -S src/runtime -B build/game -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang
#   cmake --build build/game -j 12
# Windows (x64 Native Tools Command Prompt for VS 2022, with the "C++ Clang tools" component):
#   cmake -S src\runtime -B build\game -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
#   cmake --build build\game -j 12

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# CPUs from 2013 on (Intel Haswell, AMD Excavator/Zen): MOVBE loads and stores guest memory with its byte swap in one
# instruction, LZCNT/POPCNT/BMI serve cntlzw and the bit helpers. Integer instructions only (no FMA), so floating-point
# results are unchanged. Turn off for older CPUs.
option(DT_MODERN_CPU "Use MOVBE, LZCNT, POPCNT and BMI (CPUs from 2013 on)" ON)
if(DT_MODERN_CPU AND CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|amd64")
    if(MSVC)
        add_compile_options(/clang:-mmovbe /clang:-mlzcnt /clang:-mpopcnt /clang:-mbmi /clang:-mbmi2)
    else()
        add_compile_options(-mmovbe -mlzcnt -mpopcnt -mbmi -mbmi2)
    endif()
endif()

set(DT_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../generated" CACHE PATH "dt_recomp output directory")
# Each guest function entry is recorded in a per-thread ring, which crash and hang reports print. OFF saves that
# work on every call (an A/B switch for measuring the recompiled code's overhead).
option(DT_TRACE_RING "Record guest function entries for crash and hang reports" ON)
if(NOT DT_TRACE_RING)
    add_compile_definitions(PPC_NO_TRACE_RING)
endif()
get_filename_component(DT_GENERATED_DIR "${DT_GENERATED_DIR}" ABSOLUTE)
if(NOT EXISTS "${DT_GENERATED_DIR}/ppc_config.h")
    message(FATAL_ERROR "No recompiler output in ${DT_GENERATED_DIR}. Run dt_recomp first.")
endif()

# MSVC-style driver (clang-cl) versus GNU-style (clang++/g++).
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set(DT_MSVC_DRIVER ON)
endif()

# ---- SDL3: an installed copy if CMake can find one, otherwise the pinned release built from source ----
find_package(SDL3 CONFIG QUIET)
if(NOT SDL3_FOUND)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    if(UNIX AND NOT APPLE)
        # SDL refuses to configure when an enabled optional X11 extension or Wayland is missing its headers.
        # Enable each one only if its development headers are installed.
        include(CheckIncludeFile)
        foreach(dep IN ITEMS
                "X11/Xcursor/Xcursor.h|SDL_X11_XCURSOR" "X11/extensions/XInput2.h|SDL_X11_XINPUT"
                "X11/extensions/Xrandr.h|SDL_X11_XRANDR" "X11/extensions/Xfixes.h|SDL_X11_XFIXES"
                "X11/extensions/scrnsaver.h|SDL_X11_XSCRNSAVER" "X11/extensions/shape.h|SDL_X11_XSHAPE"
                "X11/extensions/sync.h|SDL_X11_XSYNC" "X11/extensions/XTest.h|SDL_X11_XTEST"
                "X11/extensions/Xdbe.h|SDL_X11_XDBE" "wayland-client.h|SDL_WAYLAND")
            string(REPLACE "|" ";" pair "${dep}")
            list(GET pair 0 header)
            list(GET pair 1 option)
            string(MAKE_C_IDENTIFIER "DT_HAVE_${header}" var)
            check_include_file("${header}" ${var})
            if(${var})
                set(${option} ON CACHE BOOL "" FORCE)
            else()
                set(${option} OFF CACHE BOOL "" FORCE)
            endif()
        endforeach()
    endif()
    FetchContent_Declare(SDL3
        URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz
        URL_HASH SHA256=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68)
    FetchContent_MakeAvailable(SDL3)
endif()

# ---- zlib: the system copy if present, otherwise the pinned upstream release (e.g. on Windows) ----
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
    set(DT_ZLIB ZLIB::ZLIB)
else()
    include(FetchContent)
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(zlib
        URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
        URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(zlib)
    target_include_directories(zlibstatic INTERFACE "${zlib_SOURCE_DIR}" "${zlib_BINARY_DIR}")
    set(DT_ZLIB zlibstatic)
endif()

# ---- Vulkan (headers + volk loader, no SDK needed) and AMD addrlib with R600/R700 support ----
include(FetchContent)
FetchContent_Declare(VulkanHeaders
    URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz
    URL_HASH SHA256=4a078be12bef21cfebc09d878b77a63cff9d68f899254a0b00d0e37ef73e7f7e)
FetchContent_Declare(volk
    URL https://github.com/zeux/volk/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz
    URL_HASH SHA256=1547d8d74395d4048fb3f4a6313da56db626e89b55db238d0d7e8944c8a645f3)
# decaf-emu/addrlib: AMD's MIT-licensed addrlib from Mesa, with the R600/R700 hardware layer that Mesa's copy
# no longer has (Latte is R700-family).
FetchContent_Declare(addrlib
    URL https://github.com/decaf-emu/addrlib/archive/194162c47469ce620dd2470eb767ff5e42f5954a.tar.gz
    URL_HASH SHA256=a0a3a2df008b5c6db483a07cd838cf8e8804dd00429be8d332fa1804542e602f)
FetchContent_GetProperties(VulkanHeaders)
if(NOT vulkanheaders_POPULATED)
    FetchContent_Populate(VulkanHeaders)
endif()
FetchContent_GetProperties(volk)
if(NOT volk_POPULATED)
    FetchContent_Populate(volk)
endif()
FetchContent_GetProperties(addrlib)
if(NOT addrlib_POPULATED)
    FetchContent_Populate(addrlib)
endif()

add_library(dt_volk STATIC "${volk_SOURCE_DIR}/volk.c")
target_include_directories(dt_volk PUBLIC "${volk_SOURCE_DIR}" "${vulkanheaders_SOURCE_DIR}/include")
target_compile_definitions(dt_volk PUBLIC VK_NO_PROTOTYPES)
if(WIN32)
    target_compile_definitions(dt_volk PRIVATE VK_USE_PLATFORM_WIN32_KHR)
endif()

# ---- Dear ImGui (MIT): the in-game settings overlay, with its SDL3 and Vulkan (through volk) backends ----
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz
    URL_HASH SHA256=21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()
add_library(dt_imgui STATIC "${imgui_SOURCE_DIR}/imgui.cpp" "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp" "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp" "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")
target_include_directories(dt_imgui PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
target_compile_definitions(dt_imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(dt_imgui PUBLIC dt_volk SDL3::SDL3)
# The overlay's font, embedded: Roboto Medium from ImGui's misc/fonts (Apache License 2.0).
set(DT_FONT_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated_include/overlay_font.h")
if(NOT EXISTS "${DT_FONT_HEADER}")
    file(READ "${imgui_SOURCE_DIR}/misc/fonts/Roboto-Medium.ttf" DT_FONT_HEX HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," DT_FONT_BYTES "${DT_FONT_HEX}")
    file(WRITE "${DT_FONT_HEADER}" "// Generated from Dear ImGui's misc/fonts/Roboto-Medium.ttf (Apache License 2.0).\n#pragma once\nstatic const unsigned char kOverlayFont[] = { ${DT_FONT_BYTES} };\n")
endif()

file(GLOB DT_ADDRLIB_SOURCES "${addrlib_SOURCE_DIR}/src/*.cpp" "${addrlib_SOURCE_DIR}/src/core/*.cpp" "${addrlib_SOURCE_DIR}/src/r600/*.cpp")
add_library(dt_addrlib STATIC ${DT_ADDRLIB_SOURCES})
target_include_directories(dt_addrlib PUBLIC "${addrlib_SOURCE_DIR}/include")
target_include_directories(dt_addrlib PRIVATE "${addrlib_SOURCE_DIR}/include/addrlib" "${addrlib_SOURCE_DIR}/src"
    "${addrlib_SOURCE_DIR}/src/core" "${addrlib_SOURCE_DIR}/src/r600")
if(DT_MSVC_DRIVER)
    target_compile_options(dt_addrlib PRIVATE /w)
else()
    target_compile_options(dt_addrlib PRIVATE -w)
endif()

# ---- recompiled game code ----
file(GLOB DT_RECOMP_SOURCES CONFIGURE_DEPENDS "${DT_GENERATED_DIR}/ppc_recomp.*.cpp")
set(DT_GENERATED_SOURCES ${DT_RECOMP_SOURCES} "${DT_GENERATED_DIR}/ppc_func_mapping.cpp" "${DT_GENERATED_DIR}/ppc_imports.cpp")

add_library(dt_guest OBJECT ${DT_GENERATED_SOURCES})
target_include_directories(dt_guest PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${DT_GENERATED_DIR}")
if(DT_MSVC_DRIVER)
    target_compile_options(dt_guest PRIVATE /w)
else()
    target_compile_options(dt_guest PRIVATE -w)
endif()

# ---- host runtime ----
# ---- shader translator (also built standalone as the dt_shader development tool) ----
file(GLOB DT_SHADER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/gpu/shader/*.cpp")
list(FILTER DT_SHADER_SOURCES EXCLUDE REGEX "dt_shader\\.cpp$")
add_library(dt_shaderlib STATIC ${DT_SHADER_SOURCES})
target_include_directories(dt_shaderlib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/gpu/shader")
add_executable(dt_shader "${CMAKE_CURRENT_SOURCE_DIR}/gpu/shader/dt_shader.cpp")
target_link_libraries(dt_shader PRIVATE dt_shaderlib)

file(GLOB DT_RUNTIME_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/cafe/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/host/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/gpu/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/assets/*.cpp")
# The shader and pipeline caches' version: a hash of the translator and renderer sources (cmake/cache_version.cmake).
file(GLOB DT_CACHE_VERSION_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/gpu/shader/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/gpu/shader/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/gpu/shader/*.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/gpu/renderer.*"
    "${CMAKE_CURRENT_SOURCE_DIR}/gpu/surface.*" "${CMAKE_CURRENT_SOURCE_DIR}/gpu/gx2_types.h")
list(FILTER DT_CACHE_VERSION_SOURCES EXCLUDE REGEX "dt_shader\\.cpp$")
set(DT_CACHE_VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated_include/cache_version.h")
add_custom_command(OUTPUT "${DT_CACHE_VERSION_HEADER}.stamp" BYPRODUCTS "${DT_CACHE_VERSION_HEADER}"
    COMMAND "${CMAKE_COMMAND}" "-DFILES=${DT_CACHE_VERSION_SOURCES}" "-DOUT=${DT_CACHE_VERSION_HEADER}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cache_version.cmake"
    COMMAND "${CMAKE_COMMAND}" -E touch "${DT_CACHE_VERSION_HEADER}.stamp"
    DEPENDS ${DT_CACHE_VERSION_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cache_version.cmake"
    COMMENT "Hashing the shader translator and renderer sources" VERBATIM)
list(APPEND DT_RUNTIME_SOURCES "${DT_CACHE_VERSION_HEADER}.stamp")
add_executable(dt_port ${DT_RUNTIME_SOURCES})
target_link_libraries(dt_port PRIVATE dt_guest dt_shaderlib dt_volk dt_addrlib dt_imgui SDL3::SDL3 ${DT_ZLIB})
target_include_directories(dt_port PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated_include")
if(DT_MSVC_DRIVER)
    target_compile_options(dt_port PRIVATE /W3)
    target_compile_definitions(dt_port PRIVATE _CRT_SECURE_NO_WARNINGS)
    # The main thread runs guest code too: give it the same 16 MB stack as guest threads.
    target_link_options(dt_port PRIVATE /STACK:16777216)
    # A map file, so crash reports (which give dt_port.exe+offset) can be turned into function names.
    target_link_options(dt_port PRIVATE /MAP:dt_port.map)
else()
    # Vulkan create-info structs are initialised as `Info i{ VK_STRUCTURE_TYPE_... }`.
    target_compile_options(dt_port PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
endif()
