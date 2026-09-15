cmake_minimum_required(VERSION 3.21)

# iOS must be configured before project() so CMake selects the iPhone SDK and
# only emits the arm64 slice Geometry Dash can load.
if (CMAKE_SYSTEM_NAME STREQUAL "iOS" OR GEODE_TARGET_PLATFORM STREQUAL "iOS")
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "iOS architecture" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum supported iOS" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

project(ClickIndicators VERSION 7.5.0 LANGUAGES C CXX)

add_library(${PROJECT_NAME} SHARED
    src/main.cpp
    src/feature_pack.cpp
)

# Gate A only: keep the protected-overlay prototype isolated to native Windows.
# Production presentation integration remains intentionally out of scope here.
if (WIN32)
    target_sources(${PROJECT_NAME} PRIVATE
        src/windows/glfw_native_bridge.cpp
        src/windows/streamproof_overlay.cpp
        src/windows/streamproof_overlay.hpp
    )
    target_include_directories(${PROJECT_NAME} PRIVATE
        "$ENV{GEODE_SDK}/loader/include/Geode/cocos/robtop"
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE GLFW_EXPOSE_NATIVE_WGL)
    target_link_libraries(${PROJECT_NAME}
        d2d1
        d3d11
        dcomp
        dwmapi
        dxgi
    )
endif()

# Keep iOS explicitly arm64 even if this project is built outside the official
# Geode GitHub Action (for example from Xcode/CMake on a Mac).
if (CMAKE_SYSTEM_NAME STREQUAL "iOS" OR GEODE_TARGET_PLATFORM STREQUAL "iOS")
    set_target_properties(${PROJECT_NAME} PROPERTIES
        OSX_ARCHITECTURES "arm64"
        XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH "NO"
        XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "14.0"
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE CLICK_INDICATORS_IOS=1)

    # Geode 5 removed CCARRAY_FOREACH. feature_pack.cpp has two iOS-only
    # safe-area loops that still use the old spelling, so force-include a tiny
    # compatibility shim after Geode's definition. This is intentionally iOS
    # only and does not change Android or Windows compilation.
    target_compile_options(${PROJECT_NAME} PRIVATE
        -include "${CMAKE_CURRENT_SOURCE_DIR}/src/geode5_ios_compat.hpp"
    )
endif()

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Set the GEODE_SDK environment variable.")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

setup_geode_mod(${PROJECT_NAME})
