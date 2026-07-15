cmake_minimum_required(VERSION 3.21)

project(
    SkyrimClickCasting
    VERSION 0.0.1
    LANGUAGES CXX
)

find_package(CommonLibSSE CONFIG REQUIRED)

add_commonlibsse_plugin(
    ${PROJECT_NAME}
    SOURCES
        src/main.cpp
        src/ClickCast.cpp
    NAME "Skyrim Click-Casting"
    AUTHOR "AkaiRin"
    USE_ADDRESS_LIBRARY
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)

# CommonLibSSE-NG's headers (e.g. REL/Relocation.h) assume the library prelude in
# SKSE/Impl/PCH.h has been force-included first, and the CMake-generated plugin
# declaration file needs std::literals at global scope. The vcpkg-exported target does
# not propagate the precompiled header, so force-include our PCH.h (which wraps both)
# into every translation unit, including the generated __<target>Plugin.cpp.
target_precompile_headers(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/PCH.h")
