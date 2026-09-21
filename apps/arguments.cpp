// Fabric Partition Manager 1.0.0 - Summon Software Labs
#include "arguments.hpp"

#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>

namespace fpm_apps {
namespace {

constexpr std::string_view kOptionPrefix = "--";

[[nodiscard]] bool starts_with_option_prefix(const std::string& text) noexcept {
  return text.size() >= kOptionPrefix.size() &&
         text.compare(0, kOptionPrefix.size(), kOptionPrefix) == 0;
}

}  // namespace

bool Arguments::has(const std::string& name) const {
  return options.find(name) != options.end();
}

std::string Arguments::get(const std::string& name, const std::string& fallback) const {
  const auto found = options.find(name);
  if (found == options.end()) {
    return fallback;
  }
  return found->second;
}

std::optional<std::uint64_t> Arguments::get_u64(const std::string& name) const {
  const auto found = options.find(name);
  if (found == options.end()) {
    return std::nullopt;
  }
  const std::string& text = found->second;
  if (text.empty()) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  const char* const begin = text.data();
  const char* const end = text.data() + text.size();
  const std::from_chars_result parsed = std::from_chars(begin, end, value);
  if (parsed.ec != std::errc() || parsed.ptr != end) {
    return std::nullopt;
  }
  return value;
}

Arguments parse_arguments(int argc, char** argv) {
  Arguments arguments;
  bool positional_only = false;
  for (int index = 1; index < argc; ++index) {
    if (argv[index] == nullptr) {
      continue;
    }
    const std::string token(argv[index]);
    if (!positional_only && token == "--") {
      positional_only = true;
      continue;
    }
    if (positional_only || !starts_with_option_prefix(token) || token.size() == 2) {
      arguments.positional.push_back(token);
      continue;
    }
    const std::size_t separator = token.find('=');
    const std::size_t name_end = separator == std::string::npos ? token.size() : separator;
    const std::string name = token.substr(kOptionPrefix.size(), name_end - kOptionPrefix.size());
    if (separator != std::string::npos) {
      arguments.options[name] = token.substr(separator + 1);
      continue;
    }
    const bool next_is_value = index + 1 < argc && argv[index + 1] != nullptr &&
                               !starts_with_option_prefix(std::string(argv[index + 1]));
    if (next_is_value) {
      ++index;
      arguments.options[name] = std::string(argv[index]);
      continue;
    }
    arguments.options[name] = "true";
  }
  return arguments;
}

bool options_known(const Arguments& arguments, const std::vector<std::string>& known,
                   std::string& unknown) {
  for (const auto& entry : arguments.options) {
    bool matched = false;
    for (const std::string& candidate : known) {
      if (candidate == entry.first) {
        matched = true;
        break;
      }
    }
    if (!matched) {
      unknown = entry.first;
      return false;
    }
  }
  return true;
}

}  // namespace fpm_apps
