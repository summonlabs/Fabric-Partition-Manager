// Fabric Partition Manager 1.0.0 - Summon Software Labs
#include "fabric_partition_manager/limits.hpp"

#include <limits>

namespace fabric_partition_manager {

const Limits& default_limits() noexcept {
  static const Limits limits;
  return limits;
}

std::optional<std::uint64_t> checked_add(std::uint64_t lhs, std::uint64_t rhs) noexcept {
  if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
    return std::nullopt;
  }
  return lhs + rhs;
}

std::optional<std::uint64_t> checked_mul(std::uint64_t lhs, std::uint64_t rhs) noexcept {
  if (lhs == 0 || rhs == 0) {
    return 0;
  }
  if (lhs > std::numeric_limits<std::uint64_t>::max() / rhs) {
    return std::nullopt;
  }
  return lhs * rhs;
}

std::optional<std::uint32_t> checked_narrow_u32(std::uint64_t value) noexcept {
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(value);
}

std::optional<std::size_t> checked_size_add(std::size_t lhs, std::size_t rhs) noexcept {
  if (rhs > std::numeric_limits<std::size_t>::max() - lhs) {
    return std::nullopt;
  }
  return lhs + rhs;
}

std::optional<std::size_t> checked_size_mul(std::size_t lhs, std::size_t rhs) noexcept {
  if (lhs == 0 || rhs == 0) {
    return 0;
  }
  if (lhs > std::numeric_limits<std::size_t>::max() / rhs) {
    return std::nullopt;
  }
  return lhs * rhs;
}

std::optional<std::uint64_t> unordered_pair_count(std::uint64_t count) noexcept {
  if (count < 2) {
    return 0;
  }
  const std::uint64_t even = (count % 2 == 0) ? count : (count - 1);
  const std::uint64_t odd = (count % 2 == 0) ? (count - 1) : count;
  const auto half = checked_mul(even / 2, odd);
  return half;
}

std::optional<std::uint64_t> ordered_pair_count(std::uint64_t count) noexcept {
  if (count < 2) {
    return 0;
  }
  return checked_mul(count, count - 1);
}

}  // namespace fabric_partition_manager
