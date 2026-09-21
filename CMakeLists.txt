cmake_minimum_required(VERSION 3.21)

project(FabricPartitionManager
  VERSION 1.0.0
  DESCRIPTION "Authoritative representation and governance of partially disconnected fabric components"
  HOMEPAGE_URL "https://github.com/summonlabs/Fabric-Partition-Manager"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Version consistency. The C++ header is the authoritative source; the CMake
# project version must agree with it or configuration fails fast.
# ---------------------------------------------------------------------------
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/include/fabric_partition_manager/version.hpp"
     FABRIC_PARTITION_MANAGER_VERSION_HEADER)

function(fpm_header_constant NAME OUT_VAR)
  string(REGEX MATCH "inline constexpr int ${NAME} = ([0-9]+);" MATCH
         "${FABRIC_PARTITION_MANAGER_VERSION_HEADER}")
  if(NOT MATCH)
    message(FATAL_ERROR "include/fabric_partition_manager/version.hpp does not define ${NAME}")
  endif()
  set(${OUT_VAR} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

fpm_header_constant(version_major FPM_VERSION_MAJOR)
fpm_header_constant(version_minor FPM_VERSION_MINOR)
fpm_header_constant(version_patch FPM_VERSION_PATCH)

set(FPM_HEADER_VERSION "${FPM_VERSION_MAJOR}.${FPM_VERSION_MINOR}.${FPM_VERSION_PATCH}")
if(NOT FPM_HEADER_VERSION STREQUAL PROJECT_VERSION)
  message(FATAL_ERROR
    "version mismatch: project() declares ${PROJECT_VERSION} but version.hpp declares "
    "${FPM_HEADER_VERSION}")
endif()

message(STATUS "Fabric Partition Manager ${PROJECT_VERSION}")

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(FPM_BUILD_TESTS "Build the Fabric Partition Manager test suites" ON)
option(FPM_BUILD_EXAMPLES "Build the Fabric Partition Manager examples" ON)
option(FPM_BUILD_BENCHMARKS "Build the Fabric Partition Manager benchmarks" ON)
option(FPM_BUILD_APPS "Build the coordinator, publisher and CLI" ON)
option(FPM_ENABLE_ANALYZE "Run the MSVC static analyzer on first-party code" OFF)
option(FPM_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" ON)
option(FPM_BUILD_DISTRIBUTED_TESTS
  "Build the suites that start real coordinator and publisher processes" ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# Warning configuration for first-party targets only.
# ---------------------------------------------------------------------------
function(fpm_apply_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc)
    if(FPM_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    if(FPM_ENABLE_ANALYZE)
      # System and platform headers are third-party code. They are declared
      # external so that analysis reports first-party findings only; no
      # first-party warning is suppressed by this.
      target_compile_options(${target} PRIVATE
        /analyze
        /external:anglebrackets
        /external:env:INCLUDE
        /external:W0)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    if(FPM_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
add_library(FabricPartitionManager
  src/authority.cpp
  src/decision.cpp
  src/digest.cpp
  src/encoding.cpp
  src/error.cpp
  src/limits.cpp
  src/lineage.cpp
  src/partition.cpp
  src/persistence.cpp
  src/policy.cpp
  src/protocol.cpp
  src/reachability.cpp
  src/reason.cpp
  src/runtime.cpp
  src/service.cpp
  src/socket.cpp
  src/strong_id.cpp
  src/synthetic.cpp
  src/topology.cpp
  src/version.cpp)

add_library(SummonSoftwareLabs::FabricPartitionManager ALIAS FabricPartitionManager)

set_target_properties(FabricPartitionManager PROPERTIES
  EXPORT_NAME FabricPartitionManager
  POSITION_INDEPENDENT_CODE ON
  WINDOWS_EXPORT_ALL_SYMBOLS ON)

target_include_directories(FabricPartitionManager
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

fpm_apply_warnings(FabricPartitionManager)

if(WIN32)
  target_link_libraries(FabricPartitionManager PRIVATE ws2_32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(FabricPartitionManager PUBLIC Threads::Threads)
endif()

# ---------------------------------------------------------------------------
# Applications
# ---------------------------------------------------------------------------
if(FPM_BUILD_APPS)
  add_executable(fabric-partition-coordinator apps/coordinator_main.cpp apps/arguments.cpp)
  add_executable(fabric-partition-publisher apps/publisher_main.cpp apps/arguments.cpp)
  add_executable(fabric-partition-cli apps/cli_main.cpp apps/arguments.cpp)
  foreach(app fabric-partition-coordinator fabric-partition-publisher fabric-partition-cli)
    target_link_libraries(${app} PRIVATE SummonSoftwareLabs::FabricPartitionManager)
    target_include_directories(${app} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/apps")
    fpm_apply_warnings(${app})
  endforeach()
  install(TARGETS fabric-partition-coordinator fabric-partition-publisher fabric-partition-cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(FPM_BUILD_EXAMPLES)
  set(FPM_EXAMPLES
    ex_detect_partition
    ex_unknown_is_not_disconnected
    ex_authority_is_not_reachability
    ex_split_brain_conflict
    ex_governed_merge
    ex_restart_revalidation
    ex_isolation_and_degradation
    ex_persistence_recovery
    ex_distributed_publishers)
  foreach(example ${FPM_EXAMPLES})
    add_executable(fpm_example_${example} examples/${example}.cpp)
    target_link_libraries(fpm_example_${example}
      PRIVATE SummonSoftwareLabs::FabricPartitionManager)
    target_include_directories(fpm_example_${example} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/examples")
    fpm_apply_warnings(fpm_example_${example})
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------
if(FPM_BUILD_BENCHMARKS)
  add_executable(fpm_benchmarks benchmarks/benchmarks.cpp)
  target_link_libraries(fpm_benchmarks PRIVATE SummonSoftwareLabs::FabricPartitionManager)
  fpm_apply_warnings(fpm_benchmarks)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(FPM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS FabricPartitionManager
  EXPORT FabricPartitionManagerTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

set(FPM_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/FabricPartitionManager")

install(EXPORT FabricPartitionManagerTargets
  NAMESPACE SummonSoftwareLabs::
  FILE FabricPartitionManagerTargets.cmake
  DESTINATION ${FPM_CMAKE_INSTALL_DIR})

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FabricPartitionManagerConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/FabricPartitionManagerConfig.cmake"
  INSTALL_DESTINATION ${FPM_CMAKE_INSTALL_DIR})

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/FabricPartitionManagerConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/FabricPartitionManagerConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/FabricPartitionManagerConfigVersion.cmake"
  DESTINATION ${FPM_CMAKE_INSTALL_DIR})
