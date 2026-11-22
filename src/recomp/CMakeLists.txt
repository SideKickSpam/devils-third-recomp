cmake_minimum_required(VERSION 3.20)
project(dt_recomp C CXX)

# Windows: the C and C++ runtime linked in, so the release runs on PCs without the Visual C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# zlib, for compressed RPX sections: the system copy if present, otherwise the pinned upstream release (Windows).
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
    set(DT_ZLIB ZLIB::ZLIB)
else()
    include(FetchContent)
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(zlib
        URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
        URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23)
    FetchContent_MakeAvailable(zlib)
    target_include_directories(zlibstatic INTERFACE "${zlib_SOURCE_DIR}" "${zlib_BINARY_DIR}")
    set(DT_ZLIB zlibstatic)
endif()

add_executable(dt_recomp main.cpp rpx.cpp decoder.cpp recompiler.cpp "${CMAKE_CURRENT_SOURCE_DIR}/../common/rpx_decompress.cpp")
target_include_directories(dt_recomp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../common")
target_link_libraries(dt_recomp PRIVATE ${DT_ZLIB})
# The hook list read by default (dt_recomp --hooks FILE uses another).
target_compile_definitions(dt_recomp PRIVATE DT_HOOKS_FILE="${CMAKE_CURRENT_SOURCE_DIR}/hooks.txt")
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(dt_recomp PRIVATE /W3)
    target_compile_definitions(dt_recomp PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(dt_recomp PRIVATE -Wall -Wextra)
endif()
