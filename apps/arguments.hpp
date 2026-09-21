// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// Shared command-line parsing for the coordinator, publisher and CLI programs.
//
// The grammar is deliberately small: an option is either "--name=value",
// "--name value" or a bare "--name" flag whose value is the literal text
// "true". Anything else is a positional argument. Parsing never throws and
// never validates a value; validation belongs to the tool, so that each tool
// reports its own failure with its own exit code.
#ifndef FABRIC_PARTITION_MANAGER_APPS_ARGUMENTS_HPP
#define FABRIC_PARTITION_MANAGER_APPS_ARGUMENTS_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fpm_apps {

struct Arguments {
  std::vector<std::string> positional;
  std::map<std::string, std::string> options;

  [[nodiscard]] bool has(const std::string& name) const;
  [[nodiscard]] std::string get(const std::string& name,
                                const std::string& fallback = std::string()) const;
  [[nodiscard]] std::optional<std::uint64_t> get_u64(const std::string& name) const;
};

// Parses argv[1..argc). argv[0] is the program name and is ignored.
[[nodiscard]] Arguments parse_arguments(int argc, char** argv);

// True when every option that was supplied is named in known. Otherwise the
// first unrecognised option is copied into unknown and false is returned, so a
// tool refuses a misspelled option instead of silently ignoring it.
[[nodiscard]] bool options_known(const Arguments& arguments,
                                 const std::vector<std::string>& known, std::string& unknown);

}  // namespace fpm_apps

#endif  // FABRIC_PARTITION_MANAGER_APPS_ARGUMENTS_HPP
