cmake_minimum_required(VERSION 3.21)

project(FabricPartitionManagerConsumer
  VERSION 1.0.0
  DESCRIPTION "Independent consumer of an installed Fabric Partition Manager"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# The only dependency is the installed package. Nothing here reaches into the
# library's source tree, its private headers or its build directory.
find_package(FabricPartitionManager CONFIG REQUIRED)

add_executable(fabric_partition_manager_consumer main.cpp)
target_link_libraries(fabric_partition_manager_consumer
  PRIVATE SummonSoftwareLabs::FabricPartitionManager)

if(MSVC)
  target_compile_options(fabric_partition_manager_consumer
    PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
else()
  target_compile_options(fabric_partition_manager_consumer
    PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
endif()
