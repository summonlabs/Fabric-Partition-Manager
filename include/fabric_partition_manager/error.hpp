// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// Error taxonomy. Fabric Partition Manager distinguishes malformed input from
// unsupported input, stale authority from absent authority, integrity failure
// from ordinary absence, and indeterminate results from negative results.
#ifndef FABRIC_PARTITION_MANAGER_ERROR_HPP
#define FABRIC_PARTITION_MANAGER_ERROR_HPP

#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

namespace fabric_partition_manager {

enum class ErrorCode : std::uint16_t {
  Ok = 0,
  InvalidArgument = 1,
  MalformedEncoding = 2,
  UnsupportedVersion = 3,
  UnsupportedFeature = 4,
  IntegrityFailure = 5,
  SequenceRegression = 6,
  StaleAuthority = 7,
  EpochMismatch = 8,
  BootMismatch = 9,
  SessionMismatch = 10,
  LimitExceeded = 11,
  ArithmeticOverflow = 12,
  NotFound = 13,
  Conflict = 14,
  Indeterminate = 15,
  Duplicate = 16,
  Fenced = 17,
  IoFailure = 18,
  Closed = 19,
  ProtocolViolation = 20,
  OutOfRange = 21,
  Unsupported = 22,
};

[[nodiscard]] std::string_view error_code_name(ErrorCode code) noexcept;

class PartitionError : public std::exception {
 public:
  PartitionError(ErrorCode code, std::string context, std::string detail);

  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& context() const noexcept { return context_; }
  [[nodiscard]] const std::string& detail() const noexcept { return detail_; }
  [[nodiscard]] std::string render() const;

  const char* what() const noexcept override;

 private:
  ErrorCode code_;
  std::string context_;
  std::string detail_;
  mutable std::string rendered_;
};

// Convenience construction used throughout the library.
[[noreturn]] void throw_error(ErrorCode code, std::string context, std::string detail);

}  // namespace fabric_partition_manager

#endif  // FABRIC_PARTITION_MANAGER_ERROR_HPP
