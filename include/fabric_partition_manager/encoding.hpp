// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// Canonical byte encoding.
//
// Every durable record, every wire payload and every identity digest is built
// from the same writer, so the byte image of a value is a pure function of the
// value. Integers are fixed-width big-endian; text and byte runs carry a
// 32-bit big-endian length prefix. Decoding is total: a reader never throws,
// never over-reads and never allocates on behalf of a declared length. A
// decoding failure is sticky, so a partially decoded record cannot be mistaken
// for a shorter valid one.
#ifndef FABRIC_PARTITION_MANAGER_ENCODING_HPP
#define FABRIC_PARTITION_MANAGER_ENCODING_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "fabric_partition_manager/digest.hpp"
#include "fabric_partition_manager/strong_id.hpp"

namespace fabric_partition_manager {

class CanonicalWriter {
 public:
  explicit CanonicalWriter(std::vector<std::byte>& out) noexcept : out_(&out) {}

  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void boolean(bool value) { u8(value ? 1u : 0u); }
  void text(std::string_view value);
  void bytes(const std::byte* data, std::size_t count);
  void digest(const Digest& value);
  void fixed_id(const std::byte* data, std::size_t count);

  template <typename Tag, std::size_t MaxLength>
  void text_id(const TextId<Tag, MaxLength>& value) {
    text(value.view());
  }

  template <typename Tag>
  void counter(const Counter<Tag>& value) {
    u64(value.value());
  }

  template <typename Tag>
  void binary_id(const BinaryId<Tag>& value) {
    fixed_id(value.bytes().data(), value.bytes().size());
  }

 private:
  std::vector<std::byte>* out_;
};

// Total, non-throwing, sticky-failure reader.
class CanonicalReader {
 public:
  CanonicalReader(const std::byte* data, std::size_t size) noexcept
      : data_(data), size_(size) {}

  [[nodiscard]] bool u8(std::uint8_t& out) noexcept;
  [[nodiscard]] bool u16(std::uint16_t& out) noexcept;
  [[nodiscard]] bool u32(std::uint32_t& out) noexcept;
  [[nodiscard]] bool u64(std::uint64_t& out) noexcept;
  [[nodiscard]] bool boolean(bool& out) noexcept;

  // Returns a view into the underlying buffer; no allocation is performed and
  // no declared length is trusted beyond the remaining bytes.
  [[nodiscard]] bool text(std::string_view& out) noexcept;
  [[nodiscard]] bool bounded_text(std::size_t max_bytes, std::string_view& out) noexcept;
  [[nodiscard]] bool bytes(std::size_t count, const std::byte*& out) noexcept;
  [[nodiscard]] bool digest(Digest& out) noexcept;

  template <typename Tag>
  [[nodiscard]] bool typed_digest(TypedDigest<Tag>& out) noexcept {
    Digest raw;
    if (!digest(raw)) {
      return false;
    }
    out = TypedDigest<Tag>::from_digest(raw);
    return true;
  }

  // A zero-length encoding decodes to the nil identity. Nil is a legitimate
  // state for an optional identity such as a parent lineage or an attempt
  // token; it is not a malformed encoding. Semantic validation rejects a nil
  // identity wherever one is actually required.
  template <typename Tag, std::size_t MaxLength>
  [[nodiscard]] bool text_id(TextId<Tag, MaxLength>& out) noexcept {
    std::string_view raw;
    if (!bounded_text(MaxLength, raw)) {
      return false;
    }
    if (raw.empty()) {
      out = TextId<Tag, MaxLength>{};
      return true;
    }
    const auto parsed = TextId<Tag, MaxLength>::parse(raw);
    if (!parsed.has_value()) {
      return false;
    }
    out = *parsed;
    return true;
  }

  template <typename Tag>
  [[nodiscard]] bool counter(Counter<Tag>& out) noexcept {
    std::uint64_t raw = 0;
    if (!u64(raw)) {
      return false;
    }
    out = Counter<Tag>::from_value(raw);
    return true;
  }

  // Fixed-width binary identities are decoded through the validating hex parser
  // so a malformed image takes exactly the same rejection path as a malformed
  // textual identifier.
  template <typename Tag>
  [[nodiscard]] bool binary_id(BinaryId<Tag>& out) noexcept {
    const std::byte* raw = nullptr;
    if (!bytes(BinaryId<Tag>::byte_count, raw)) {
      return false;
    }
    static constexpr char kDigits[] = "0123456789abcdef";
    char hex[BinaryId<Tag>::hex_length];
    for (std::size_t index = 0; index < BinaryId<Tag>::byte_count; ++index) {
      const auto raw_byte = static_cast<unsigned>(raw[index]);
      hex[index * 2] = kDigits[(raw_byte >> 4) & 0xFu];
      hex[index * 2 + 1] = kDigits[raw_byte & 0xFu];
    }
    const auto parsed = BinaryId<Tag>::parse_hex(
        std::string_view(hex, BinaryId<Tag>::hex_length));
    if (!parsed.has_value()) {
      return false;
    }
    out = *parsed;
    return true;
  }

  [[nodiscard]] bool exhausted() const noexcept { return offset_ == size_ && !failed_; }
  [[nodiscard]] bool failed() const noexcept { return failed_; }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return size_ - offset_; }
  [[nodiscard]] const std::byte* cursor() const noexcept { return data_ + offset_; }

  // Restricts the reader to the first count remaining bytes and reports the
  // tail that follows. Used to reject trailing garbage after a record.
  [[nodiscard]] bool take_region(std::size_t count, CanonicalReader& out) noexcept;

 private:
  [[nodiscard]] bool need(std::size_t count) noexcept;

  const std::byte* data_;
  std::size_t size_;
  std::size_t offset_ = 0;
  bool failed_ = false;
};

}  // namespace fabric_partition_manager

#endif  // FABRIC_PARTITION_MANAGER_ENCODING_HPP
