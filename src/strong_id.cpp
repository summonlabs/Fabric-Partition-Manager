// Fabric Partition Manager 1.0.0 - Summon Software Labs
#include "fabric_partition_manager/strong_id.hpp"

#include <atomic>
#include <chrono>
#include <random>

#include "platform.hpp"

namespace fabric_partition_manager {
namespace detail {
namespace {

std::uint64_t mix64(std::uint64_t value) noexcept {
  value += 0x9E3779B97F4A7C15ull;
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
  return value ^ (value >> 31);
}

std::uint64_t seed_entropy() noexcept {
  std::uint64_t seed = 0;
  try {
    std::random_device device;
    seed = (static_cast<std::uint64_t>(device()) << 32) ^ static_cast<std::uint64_t>(device());
  } catch (...) {
    seed = 0;
  }
  seed ^= monotonic_nanos();
  seed ^= process_identifier() * 0x9E3779B97F4A7C15ull;
  static std::atomic<std::uint64_t> counter{0};
  seed ^= counter.fetch_add(1, std::memory_order_relaxed) * 0xD1B54A32D192ED03ull;
  return mix64(seed);
}

std::uint64_t next_entropy() noexcept {
  thread_local std::uint64_t state = seed_entropy();
  state = mix64(state);
  return state;
}

}  // namespace

bool is_id_alphanumeric(char value) noexcept {
  return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9');
}

bool is_id_separator(char value) noexcept {
  return value == '.' || value == '_' || value == '-' || value == ':';
}

bool is_hex_lower(char value) noexcept {
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
}

void fill_random_bytes(std::byte* out, std::size_t count) noexcept {
  std::size_t index = 0;
  while (index < count) {
    const std::uint64_t word = next_entropy();
    for (int shift = 0; shift < 8 && index < count; ++shift) {
      out[index] = static_cast<std::byte>((word >> (shift * 8)) & 0xFFu);
      ++index;
    }
  }
}

std::uint64_t monotonic_nanos() noexcept {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

std::uint64_t process_identifier() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

}  // namespace detail

template <typename Tag>
BinaryId<Tag> BinaryId<Tag>::generate() noexcept {
  BinaryId<Tag> result;
  detail::fill_random_bytes(result.bytes_.data(), result.bytes_.size());
  return result;
}

template class BinaryId<CoordinatorBootIdTag>;
template class BinaryId<ProcessIncarnationIdTag>;
template class BinaryId<PublisherBootIdTag>;

}  // namespace fabric_partition_manager
