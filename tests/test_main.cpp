// Fabric Partition Manager 1.0.0 - Summon Software Labs
#include <iostream>
#include <string>

#include "test_harness.hpp"

int main(int argc, char** argv) {
  std::string filter;
  std::string skip;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument.rfind("--filter=", 0) == 0) {
      filter = argument.substr(9);
    } else if (argument.rfind("--skip=", 0) == 0) {
      skip = argument.substr(7);
    }
  }
  return fpm_test::run_all(filter, skip);
}
