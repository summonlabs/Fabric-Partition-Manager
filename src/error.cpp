// Fabric Partition Manager 1.0.0 - Summon Software Labs
#include "fabric_partition_manager/error.hpp"

#include <sstream>
#include <utility>

namespace fabric_partition_manager {

std::string_view error_code_name(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok: return "OK";
    case ErrorCode::InvalidArgument: return "INVALID_ARGUMENT";
    case ErrorCode::MalformedEncoding: return "MALFORMED_ENCODING";
    case ErrorCode::UnsupportedVersion: return "UNSUPPORTED_VERSION";
    case ErrorCode::UnsupportedFeature: return "UNSUPPORTED_FEATURE";
    case ErrorCode::IntegrityFailure: return "INTEGRITY_FAILURE";
    case ErrorCode::SequenceRegression: return "SEQUENCE_REGRESSION";
    case ErrorCode::StaleAuthority: return "STALE_AUTHORITY";
    case ErrorCode::EpochMismatch: return "EPOCH_MISMATCH";
    case ErrorCode::BootMismatch: return "BOOT_MISMATCH";
    case ErrorCode::SessionMismatch: return "SESSION_MISMATCH";
    case ErrorCode::LimitExceeded: return "LIMIT_EXCEEDED";
    case ErrorCode::ArithmeticOverflow: return "ARITHMETIC_OVERFLOW";
    case ErrorCode::NotFound: return "NOT_FOUND";
    case ErrorCode::Conflict: return "CONFLICT";
    case ErrorCode::Indeterminate: return "INDETERMINATE";
    case ErrorCode::Duplicate: return "DUPLICATE";
    case ErrorCode::Fenced: return "FENCED";
    case ErrorCode::IoFailure: return "IO_FAILURE";
    case ErrorCode::Closed: return "CLOSED";
    case ErrorCode::ProtocolViolation: return "PROTOCOL_VIOLATION";
    case ErrorCode::OutOfRange: return "OUT_OF_RANGE";
    case ErrorCode::Unsupported: return "UNSUPPORTED";
  }
  return "UNRECOGNIZED";
}

PartitionError::PartitionError(ErrorCode code, std::string context, std::string detail)
    : code_(code), context_(std::move(context)), detail_(std::move(detail)) {}

std::string PartitionError::render() const {
  std::ostringstream stream;
  stream << error_code_name(code_);
  if (!context_.empty()) {
    stream << " at " << context_;
  }
  if (!detail_.empty()) {
    stream << ": " << detail_;
  }
  return stream.str();
}

const char* PartitionError::what() const noexcept {
  if (rendered_.empty()) {
    try {
      rendered_ = render();
    } catch (...) {
      return "fabric_partition_manager::PartitionError";
    }
  }
  return rendered_.c_str();
}

void throw_error(ErrorCode code, std::string context, std::string detail) {
  throw PartitionError(code, std::move(context), std::move(detail));
}

}  // namespace fabric_partition_manager
