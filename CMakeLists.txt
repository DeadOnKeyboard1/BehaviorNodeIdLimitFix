cmake_minimum_required(VERSION 3.21)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
endif()

set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0141 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" CACHE STRING "" FORCE)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "Embedded" CACHE STRING "" FORCE)

project(
    BehaviorNodeIdLimitFix
    VERSION 2.5.3
    LANGUAGES CXX
)

if(DEFINED ENV{SKSE_TOOLCHAIN_ROOT} AND NOT "$ENV{SKSE_TOOLCHAIN_ROOT}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{SKSE_TOOLCHAIN_ROOT}" TOOLCHAIN_ROOT)
else()
    get_filename_component(TOOLCHAIN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../_toolchain" ABSOLUTE)
endif()

set(COMMONLIB_ROOT "${TOOLCHAIN_ROOT}/CommonLibSSE-NG-v16.7.0" CACHE PATH "Path to CommonLibSSE-NG")
if(NOT EXISTS "${COMMONLIB_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "CommonLibSSE-NG not found at: ${COMMONLIB_ROOT}")
endif()

set(DEPS_ROOT "${TOOLCHAIN_ROOT}/skyrim_1_7_104_compat/deps-msvc1951" CACHE PATH "Path to prebuilt dependencies")
set(VCPKG_PREFIX "${TOOLCHAIN_ROOT}/vcpkg/installed/x64-windows-static-md")
if(EXISTS "${VCPKG_PREFIX}")
    list(PREPEND CMAKE_PREFIX_PATH "${VCPKG_PREFIX}")
endif()
if(EXISTS "${DEPS_ROOT}")
    list(PREPEND CMAKE_PREFIX_PATH "${DEPS_ROOT}")
endif()
if(EXISTS "${DEPS_ROOT}/include/rapidcsv.h")
    set(RAPIDCSV_INCLUDE_DIRS "${DEPS_ROOT}/include" CACHE PATH "Path to rapidcsv headers" FORCE)
endif()

find_package(directxmath CONFIG QUIET)

set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FETCHCONTENT_SOURCE_DIR_HDE64 "${TOOLCHAIN_ROOT}/minhook-v1.3.4" CACHE PATH "Local instruction decoder source")
set(SKSE_SUPPORT_PATCH_SAFETY ON CACHE BOOL "" FORCE)
set(COMMONLIB_PREBUILT OFF CACHE BOOL "" FORCE)
set(COMMONLIB_ENABLE_IPO OFF CACHE BOOL "" FORCE)
set(REX_OPTION_INI OFF CACHE BOOL "" FORCE)
set(REX_OPTION_JSON OFF CACHE BOOL "" FORCE)
set(REX_OPTION_TOML OFF CACHE BOOL "" FORCE)
set(SKSE_SUPPORT_XBYAK OFF CACHE BOOL "" FORCE)

add_subdirectory("${COMMONLIB_ROOT}" "${CMAKE_BINARY_DIR}/commonlib" EXCLUDE_FROM_ALL)
add_library(
    ${PROJECT_NAME}
    SHARED
    src/Main.cpp
    src/Hooks.cpp
    src/GraphCache.cpp
    src/GraphCacheStore.cpp
    src/GraphSlimming.cpp
    src/GraphPerformance.cpp
    src/StartupPreload.cpp
    src/AnimationPreload.cpp
    src/HkxDirectoryIndex.cpp
    src/OfflineGraph.cpp
    src/OfflinePreload.cpp
    include/Hooks.h
    include/PCH.h
)

target_link_libraries(${PROJECT_NAME} PRIVATE CommonLibSSE::CommonLibSSE user32 gdi32)
target_compile_definitions(${PROJECT_NAME} PRIVATE BEHAVIOR_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    BEHAVIOR_VERSION_MINOR=${PROJECT_VERSION_MINOR} BEHAVIOR_VERSION_PATCH=${PROJECT_VERSION_PATCH})
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include" "${FETCHCONTENT_SOURCE_DIR_HDE64}/src/hde")
target_precompile_headers(${PROJECT_NAME} PRIVATE include/PCH.h)

include(CTest)
if(BUILD_TESTING)
    add_executable(BehaviorGraphPlanTests tests/GraphPlanTests.cpp src/GraphCacheStore.cpp src/AnimationPreload.cpp src/HkxDirectoryIndex.cpp)
    target_compile_features(BehaviorGraphPlanTests PRIVATE cxx_std_23)
    target_include_directories(BehaviorGraphPlanTests PRIVATE include)
    add_test(NAME BehaviorGraphPlan COMMAND BehaviorGraphPlanTests)
    add_executable(BehaviorOfflineGraphTests tests/OfflineGraphTests.cpp src/OfflineGraph.cpp src/OfflinePreload.cpp)
    target_compile_features(BehaviorOfflineGraphTests PRIVATE cxx_std_23)
    target_include_directories(BehaviorOfflineGraphTests PRIVATE include)
    add_test(NAME BehaviorOfflineGraph COMMAND BehaviorOfflineGraphTests)
    add_executable(BehaviorSlimmingTests tests/SlimmingTests.cpp src/GraphSlimming.cpp)
    target_compile_features(BehaviorSlimmingTests PRIVATE cxx_std_23)
    target_include_directories(BehaviorSlimmingTests PRIVATE include)
    add_test(NAME BehaviorSlimming COMMAND BehaviorSlimmingTests)
    add_executable(BehaviorSlimmingBenchmark tests/SlimmingBenchmark.cpp src/GraphSlimming.cpp)
    target_compile_features(BehaviorSlimmingBenchmark PRIVATE cxx_std_23)
    target_include_directories(BehaviorSlimmingBenchmark PRIVATE include)
    add_executable(BehaviorSlimmingBenchmark107 tests/SlimmingBenchmark.cpp)
    target_compile_definitions(BehaviorSlimmingBenchmark107 PRIVATE BEHAVIOR_REFERENCE_107)
    target_compile_features(BehaviorSlimmingBenchmark107 PRIVATE cxx_std_23)
    target_include_directories(BehaviorSlimmingBenchmark107 PRIVATE tests/reference-1.0.7)
endif()

find_package(spdlog CONFIG REQUIRED)
find_package(directxtk CONFIG REQUIRED)
add_library(BehaviorPrivacyProbe SHARED EXCLUDE_FROM_ALL tests/PrivacyProbe.cpp)
target_compile_features(BehaviorPrivacyProbe PRIVATE cxx_std_23)
target_link_libraries(BehaviorPrivacyProbe PRIVATE spdlog::spdlog Microsoft::DirectXTK)
if(MSVC)
    target_compile_options(BehaviorPrivacyProbe PRIVATE /Z7)
    target_link_options(BehaviorPrivacyProbe PRIVATE /DEBUG:FULL /INCREMENTAL:NO /PDBALTPATH:%_PDB% /Brepro)
endif()

if(MSVC)
    target_compile_options(
        ${PROJECT_NAME}
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            /EHsc
            /Z7
    )
    target_link_options(
        ${PROJECT_NAME}
        PRIVATE
            /DEBUG:FULL
            /OPT:REF
            /OPT:ICF
            /INCREMENTAL:NO
            /PDBALTPATH:%_PDB%
            /Brepro
    )
endif()
