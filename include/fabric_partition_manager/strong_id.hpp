// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// Strongly typed identities.
//
// Fabric Partition Manager never uses bare integers or interchangeable strings
// for identity. Every identity below is a distinct C++ type: it cannot be
// constructed from a malformed encoding, it cannot be silently converted into
// an identity of another domain, it serializes canonically and it renders
// stably. Comparison is lexicographic over the canonical text so that ordering
// is independent of container, insertion or discovery order.
#ifndef FABRIC_PARTITION_MANAGER_STRONG_ID_HPP
#define FABRIC_PARTITION_MANAGER_STRONG_ID_HPP

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace fabric_partition_manager {

namespace detail {

// Canonical identifier alphabet: [a-z0-9] plus the separators '.', '_', '-'
// and ':'. Path separators, backslashes, wildcards, whitespace, control
// characters, uppercase and non-ASCII bytes are all rejected, so an accepted
// identifier can never escape a directory or address a foreign namespace.
[[nodiscard]] bool is_id_alphanumeric(char value) noexcept;
[[nodiscard]] bool is_id_separator(char value) noexcept;
[[nodiscard]] bool is_hex_lower(char value) noexcept;

// Fills the buffer with bytes drawn from the operating system entropy source.
// This is used for uniqueness of boot/process/session identities, not for
// cryptographic secrecy; Fabric Partition Manager does not claim secure
// transport (see the README security model section).
void fill_random_bytes(std::byte* out, std::size_t count) noexcept;

// Monotonic nanoseconds since an unspecified per-host origin. Only differences
// are meaningful.
[[nodiscard]] std::uint64_t monotonic_nanos() noexcept;

// Current process identifier, rendered as an unsigned value.
[[nodiscard]] std::uint64_t process_identifier() noexcept;

}  // namespace detail

// ---------------------------------------------------------------------------
// Text identifier
// ---------------------------------------------------------------------------

template <typename Tag, std::size_t MaxLength>
class TextId {
 public:
  static constexpr std::size_t max_length = MaxLength;
  using tag_type = Tag;

  TextId() noexcept = default;

  [[nodiscard]] static bool is_valid(std::string_view text) noexcept {
    if (text.empty() || text.size() > MaxLength) {
      return false;
    }
    if (!detail::is_id_alphanumeric(text.front()) || !detail::is_id_alphanumeric(text.back())) {
      return false;
    }
    for (const char value : text) {
      if (!detail::is_id_alphanumeric(value) && !detail::is_id_separator(value)) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] static std::optional<TextId> parse(std::string_view text) noexcept {
    if (!is_valid(text)) {
      return std::nullopt;
    }
    return from_validated(text);
  }

  // Pre-validated construction. The caller guarantees is_valid(text); the
  // value is truncated defensively so a programming error degrades to a wrong
  // identity rather than a buffer overrun.
  [[nodiscard]] static TextId from_validated(std::string_view text) noexcept {
    TextId result;
    const std::size_t count = text.size() < MaxLength ? text.size() : MaxLength;
    result.length_ = static_cast<std::uint16_t>(count);
    for (std::size_t index = 0; index < count; ++index) {
      result.data_[index] = text[index];
    }
    return result;
  }

  [[nodiscard]] static const TextId& nil() noexcept {
    static const TextId value;
    return value;
  }

  [[nodiscard]] bool is_nil() const noexcept { return length_ == 0; }

  [[nodiscard]] std::string_view view() const noexcept {
    return std::string_view(data_.data(), static_cast<std::size_t>(length_));
  }

  [[nodiscard]] std::string str() const { return std::string(view()); }

  [[nodiscard]] const char* data() const noexcept { return data_.data(); }

  [[nodiscard]] std::size_t size() const noexcept { return static_cast<std::size_t>(length_); }

  friend bool operator==(const TextId& lhs, const TextId& rhs) noexcept {
    return lhs.view() == rhs.view();
  }

  friend std::strong_ordering operator<=>(const TextId& lhs, const TextId& rhs) noexcept {
    return lhs.view() <=> rhs.view();
  }

 private:
  std::array<char, MaxLength> data_{};
  std::uint16_t length_ = 0;
};

// ---------------------------------------------------------------------------
// Monotonic counter identifier
// ---------------------------------------------------------------------------

template <typename Tag>
class Counter {
 public:
  using tag_type = Tag;
  using value_type = std::uint64_t;

  static constexpr value_type max_value = 0xFFFF'FFFF'FFFF'FFFFull;

  constexpr Counter() noexcept = default;

  [[nodiscard]] static constexpr Counter from_value(value_type value) noexcept {
    Counter result;
    result.value_ = value;
    return result;
  }

  [[nodiscard]] constexpr value_type value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }

  // Checked successor. Returns nullopt at the top of the range rather than
  // wrapping, so no generation or sequence can silently regress to a value
  // that already carried authority.
  [[nodiscard]] constexpr std::optional<Counter> next() const noexcept {
    if (value_ == max_value) {
      return std::nullopt;
    }
    return from_value(value_ + 1);
  }

  // Checked advancement by a caller-supplied delta.
  [[nodiscard]] constexpr std::optional<Counter> advance(value_type delta) const noexcept {
    if (delta > max_value - value_) {
      return std::nullopt;
    }
    return from_value(value_ + delta);
  }

  [[nodiscard]] std::string render() const { return std::to_string(value_); }

  friend constexpr bool operator==(const Counter&, const Counter&) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(const Counter&, const Counter&) noexcept = default;

 private:
  value_type value_ = 0;
};

// ---------------------------------------------------------------------------
// Fixed-width binary identity (boot / process incarnation)
// ---------------------------------------------------------------------------

template <typename Tag>
class BinaryId {
 public:
  using tag_type = Tag;

  static constexpr std::size_t byte_count = 16;
  static constexpr std::size_t hex_length = byte_count * 2;

  BinaryId() noexcept = default;

  [[nodiscard]] static BinaryId generate() noexcept;

  [[nodiscard]] static std::optional<BinaryId> parse_hex(std::string_view text) noexcept {
    if (text.size() != hex_length) {
      return std::nullopt;
    }
    BinaryId result;
    for (std::size_t index = 0; index < byte_count; ++index) {
      const char high = text[index * 2];
      const char low = text[index * 2 + 1];
      if (!detail::is_hex_lower(high) || !detail::is_hex_lower(low)) {
        return std::nullopt;
      }
      const auto high_value = static_cast<unsigned>(high <= '9' ? high - '0' : high - 'a' + 10);
      const auto low_value = static_cast<unsigned>(low <= '9' ? low - '0' : low - 'a' + 10);
      result.bytes_[index] = static_cast<std::byte>((high_value << 4) | low_value);
    }
    return result;
  }

  [[nodiscard]] bool is_nil() const noexcept {
    for (const std::byte value : bytes_) {
      if (value != std::byte{0}) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] std::string hex() const {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string result;
    result.reserve(hex_length);
    for (const std::byte value : bytes_) {
      const auto raw = static_cast<unsigned>(value);
      result.push_back(kDigits[(raw >> 4) & 0xFu]);
      result.push_back(kDigits[raw & 0xFu]);
    }
    return result;
  }

  [[nodiscard]] const std::array<std::byte, byte_count>& bytes() const noexcept { return bytes_; }

  friend bool operator==(const BinaryId&, const BinaryId&) noexcept = default;
  friend std::strong_ordering operator<=>(const BinaryId&, const BinaryId&) noexcept = default;

 private:
  std::array<std::byte, byte_count> bytes_{};
};

// ---------------------------------------------------------------------------
// Identity tags and aliases
// ---------------------------------------------------------------------------

struct ComponentIdTag {};
struct PartitionIdTag {};
struct LineageIdTag {};
struct EvidenceIdTag {};
struct PolicyIdTag {};
struct TopologyIdTag {};
struct DecisionIdTag {};
struct FenceIdTag {};
struct SessionIdTag {};
struct ScopeIdTag {};
struct PublisherIdTag {};
struct ProvenanceTag {};
struct AttemptIdTag {};

struct TopologyGenerationTag {};
struct ReachabilityGenerationTag {};
struct PolicyGenerationTag {};
struct PartitionGenerationTag {};
struct CoordinatorEpochTag {};
struct AuthoritySequenceTag {};
struct EvidenceSequenceTag {};
struct DecisionSequenceTag {};
struct AttemptSequenceTag {};
struct BootSequenceTag {};
struct LineageSequenceTag {};
struct FenceSequenceTag {};
struct RevalidationSequenceTag {};

struct CoordinatorBootIdTag {};
struct ProcessIncarnationIdTag {};
struct PublisherBootIdTag {};

using ComponentId = TextId<ComponentIdTag, 96>;
using PartitionId = TextId<PartitionIdTag, 96>;
using LineageId = TextId<LineageIdTag, 96>;
using EvidenceId = TextId<EvidenceIdTag, 96>;
using PolicyId = TextId<PolicyIdTag, 96>;
using TopologyId = TextId<TopologyIdTag, 96>;
using DecisionId = TextId<DecisionIdTag, 96>;
using FenceId = TextId<FenceIdTag, 96>;
using SessionId = TextId<SessionIdTag, 64>;
using ScopeId = TextId<ScopeIdTag, 96>;
using PublisherId = TextId<PublisherIdTag, 96>;
using Provenance = TextId<ProvenanceTag, 64>;
using AttemptId = TextId<AttemptIdTag, 64>;

using TopologyGeneration = Counter<TopologyGenerationTag>;
using ReachabilityGeneration = Counter<ReachabilityGenerationTag>;
using PolicyGeneration = Counter<PolicyGenerationTag>;
using PartitionGeneration = Counter<PartitionGenerationTag>;
using CoordinatorEpoch = Counter<CoordinatorEpochTag>;
using AuthoritySequence = Counter<AuthoritySequenceTag>;
using EvidenceSequence = Counter<EvidenceSequenceTag>;
using DecisionSequence = Counter<DecisionSequenceTag>;
using AttemptSequence = Counter<AttemptSequenceTag>;
using BootSequence = Counter<BootSequenceTag>;
using LineageSequence = Counter<LineageSequenceTag>;
using FenceSequence = Counter<FenceSequenceTag>;
using RevalidationSequence = Counter<RevalidationSequenceTag>;

using CoordinatorBootId = BinaryId<CoordinatorBootIdTag>;
using ProcessIncarnationId = BinaryId<ProcessIncarnationIdTag>;
using PublisherBootId = BinaryId<PublisherBootIdTag>;

}  // namespace fabric_partition_manager

// ---------------------------------------------------------------------------
// Hashing support for the unordered containers used on lookup-dominated paths.
// The digest is derived from the canonical text so it agrees with equality.
// ---------------------------------------------------------------------------

namespace std {

template <typename Tag, size_t MaxLength>
struct hash<fabric_partition_manager::TextId<Tag, MaxLength>> {
  size_t operator()(const fabric_partition_manager::TextId<Tag, MaxLength>& value) const noexcept {
    // FNV-1a over the canonical bytes.
    size_t result = 1469598103934665603ull;
    for (const char byte : value.view()) {
      result ^= static_cast<size_t>(static_cast<unsigned char>(byte));
      result *= 1099511628211ull;
    }
    return result;
  }
};

template <typename Tag>
struct hash<fabric_partition_manager::Counter<Tag>> {
  size_t operator()(const fabric_partition_manager::Counter<Tag>& value) const noexcept {
    return std::hash<uint64_t>{}(value.value());
  }
};

template <typename Tag>
struct hash<fabric_partition_manager::BinaryId<Tag>> {
  size_t operator()(const fabric_partition_manager::BinaryId<Tag>& value) const noexcept {
    size_t result = 1469598103934665603ull;
    for (const std::byte byte : value.bytes()) {
      result ^= static_cast<size_t>(static_cast<unsigned char>(byte));
      result *= 1099511628211ull;
    }
    return result;
  }
};

}  // namespace std

#endif  // FABRIC_PARTITION_MANAGER_STRONG_ID_HPP
