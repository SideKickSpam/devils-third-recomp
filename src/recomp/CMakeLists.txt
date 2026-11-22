cmake_minimum_required(VERSION 3.20)
project(dt_recomp CXX)

# Windows: the C and C++ runtime linked in, so the release runs on PCs without the Visual C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(dt_recomp main.cpp rpx.cpp decoder.cpp recompiler.cpp)
# The hook list read by default (dt_recomp --hooks FILE uses another).
target_compile_definitions(dt_recomp PRIVATE DT_HOOKS_FILE="${CMAKE_CURRENT_SOURCE_DIR}/hooks.txt")
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(dt_recomp PRIVATE /W3)
    target_compile_definitions(dt_recomp PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(dt_recomp PRIVATE -Wall -Wextra)
endif()
