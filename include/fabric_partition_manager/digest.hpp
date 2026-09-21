// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// Deterministic digests.
//
// SHA-256 is used for canonical identity digests: a partition identity, a
// membership digest, a lineage record digest and a snapshot digest are all
// derived from the canonical byte encoding of their subject, so equivalent
// inputs produce byte-identical digests regardless of observation, insertion or
// container order.
//
// CRC-64/XZ is used for record, snapshot and frame integrity. It detects torn,
// truncated and bit-flipped bytes. Neither primitive provides authentication:
// Fabric Partition Manager does not claim cryptographic security and does not
// authenticate peers (see the README security model section).
#ifndef FABRIC_PARTITION_MANAGER_DIGEST_HPP
#define FABRIC_PARTITION_MANAGER_DIGEST_HPP

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace fabric_partition_manager {

inline constexpr std::size_t digest_bytes = 32;

struct Digest {
  std::array<std::byte, digest_bytes> bytes{};

  [[nodiscard]] std::string hex() const;
  [[nodiscard]] bool is_zero() const noexcept;
  [[nodiscard]] std::string_view prefix8() const noexcept;

  friend bool operator==(const Digest&, const Digest&) noexcept = default;
  friend std::strong_ordering operator<=>(const Digest& lhs, const Digest& rhs) noexcept {
    for (std::size_t index = 0; index < digest_bytes; ++index) {
      const auto left = static_cast<unsigned>(lhs.bytes[index]);
      const auto right = static_cast<unsigned>(rhs.bytes[index]);
      if (left != right) {
        return left <=> right;
      }
    }
    return std::strong_ordering::equal;
  }
};

// Streaming SHA-256. finish() may be called once; update() after finish() is
// ignored so a misuse cannot produce a digest over a partial stream.
class Sha256 {
 public:
  Sha256() noexcept;

  void update(const void* data, std::size_t size) noexcept;
  void update(std::string_view text) noexcept { update(text.data(), text.size()); }
  void update_u8(std::uint8_t value) noexcept;
  void update_be16(std::uint16_t value) noexcept;
  void update_be32(std::uint32_t value) noexcept;
  void update_be64(std::uint64_t value) noexcept;

  // Canonical framing: 4-byte big-endian length followed by the bytes. This is
  // what makes concatenated fields unambiguous.
  void update_text(std::string_view text) noexcept;

  [[nodiscard]] Digest finish() noexcept;

 private:
  void compress(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::uint64_t total_bytes_ = 0;
  std::size_t buffer_used_ = 0;
  bool finalized_ = false;
};

[[nodiscard]] Digest sha256_bytes(const void* data, std::size_t size) noexcept;
[[nodiscard]] Digest sha256_text(std::string_view text) noexcept;

// CRC-64/XZ: polynomial 0x42F0E1EBA9EA3693, initial value 0xFFFFFFFFFFFFFFFF,
// reflected input and output, final xor 0xFFFFFFFFFFFFFFFF. The check value for
// the ASCII input "123456789" is 0x995DC9BBDF1939FA.
inline constexpr std::uint64_t crc64_initial = 0xFFFF'FFFF'FFFF'FFFFull;

class Crc64 {
 public:
  Crc64() noexcept = default;

  void update(const void* data, std::size_t size) noexcept;
  void update(std::string_view text) noexcept { update(text.data(), text.size()); }
  void update_u8(std::uint8_t value) noexcept;
  void update_be32(std::uint32_t value) noexcept;
  void update_be64(std::uint64_t value) noexcept;

  [[nodiscard]] std::uint64_t value() const noexcept { return value_; }
  void reset() noexcept { value_ = crc64_initial; }

 private:
  std::uint64_t value_ = crc64_initial;
};

[[nodiscard]] std::uint64_t crc64_bytes(const void* data, std::size_t size) noexcept;
[[nodiscard]] std::uint64_t crc64_text(std::string_view text) noexcept;

// ---------------------------------------------------------------------------
// Domain-tagged digests. A membership digest is not interchangeable with a
// snapshot digest or an evidence-set digest even though all three are 32 bytes.
// ---------------------------------------------------------------------------

struct MembershipDigestTag {};
struct SnapshotDigestTag {};
struct EvidenceSetDigestTag {};
struct LineageDigestTag {};
struct PartitionPlanDigestTag {};

template <typename Tag>
class TypedDigest {
 public:
  using tag_type = Tag;

  TypedDigest() = default;
  explicit TypedDigest(Digest value) noexcept : value_(value) {}

  [[nodiscard]] static TypedDigest from_digest(Digest value) noexcept { return TypedDigest(value); }

  [[nodiscard]] const Digest& value() const noexcept { return value_; }
  [[nodiscard]] std::string hex() const { return value_.hex(); }
  [[nodiscard]] bool is_zero() const noexcept { return value_.is_zero(); }

  friend bool operator==(const TypedDigest&, const TypedDigest&) noexcept = default;
  friend std::strong_ordering operator<=>(const TypedDigest& lhs, const TypedDigest& rhs) noexcept {
    return lhs.value_ <=> rhs.value_;
  }

 private:
  Digest value_{};
};

using MembershipDigest = TypedDigest<MembershipDigestTag>;
using SnapshotDigest = TypedDigest<SnapshotDigestTag>;
using EvidenceSetDigest = TypedDigest<EvidenceSetDigestTag>;
using LineageDigest = TypedDigest<LineageDigestTag>;
using PartitionPlanDigest = TypedDigest<PartitionPlanDigestTag>;

}  // namespace fabric_partition_manager

namespace std {

template <typename Tag>
struct hash<fabric_partition_manager::TypedDigest<Tag>> {
  size_t operator()(const fabric_partition_manager::TypedDigest<Tag>& value) const noexcept {
    const auto& bytes = value.value().bytes;
    size_t result = 1469598103934665603ull;
    for (const std::byte byte : bytes) {
      result ^= static_cast<size_t>(static_cast<unsigned char>(byte));
      result *= 1099511628211ull;
    }
    return result;
  }
};

}  // namespace std

#endif  // FABRIC_PARTITION_MANAGER_DIGEST_HPP
