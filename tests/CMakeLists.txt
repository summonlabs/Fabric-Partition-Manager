# Fabric Partition Manager 1.0.0 - Summon Software Labs
# Test suites. No CTest TIMEOUT property is set anywhere: a hanging test is a
# defect and must surface as a hang rather than be masked by a timer.

set(FPM_TEST_SUITES
  identity
  reachability
  partition
  property
  authority
  merge
  persistence
  corruption
  protocol
  limits
  lifecycle
  concurrency
  scale)

add_library(fpm_test_support STATIC test_harness.cpp)
target_include_directories(fpm_test_support PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  # White-box access to the internal transport helpers lets the protocol suite
  # drive a real socket with a hand-built frame. Internal headers are never
  # installed.
  "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(fpm_test_support PUBLIC SummonSoftwareLabs::FabricPartitionManager)
fpm_apply_warnings(fpm_test_support)

set(FPM_TEST_SOURCES test_main.cpp)
foreach(suite ${FPM_TEST_SUITES})
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_${suite}.cpp")
    message(FATAL_ERROR "the test suite test_${suite}.cpp is missing")
  endif()
  list(APPEND FPM_TEST_SOURCES test_${suite}.cpp)
endforeach()

if(FPM_BUILD_DISTRIBUTED_TESTS AND FPM_BUILD_APPS)
  list(APPEND FPM_TEST_SOURCES test_distributed.cpp)
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/test_executables.hpp" CONTENT
"#pragma once
#define FPM_COORDINATOR_EXE \"$<TARGET_FILE:fabric-partition-coordinator>\"
#define FPM_PUBLISHER_EXE \"$<TARGET_FILE:fabric-partition-publisher>\"
#define FPM_CLI_EXE \"$<TARGET_FILE:fabric-partition-cli>\"
")
  set_source_files_properties(test_distributed.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}")
endif()

add_executable(fpm_tests ${FPM_TEST_SOURCES})
target_link_libraries(fpm_tests PRIVATE fpm_test_support)
fpm_apply_warnings(fpm_tests)
if(FPM_BUILD_DISTRIBUTED_TESTS AND FPM_BUILD_APPS)
  add_dependencies(fpm_tests fabric-partition-coordinator fabric-partition-publisher
                   fabric-partition-cli)
endif()

add_test(NAME fpm_tests COMMAND fpm_tests)
