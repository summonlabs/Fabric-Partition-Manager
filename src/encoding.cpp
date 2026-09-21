// Fabric Partition Manager 1.0.0 - Summon Software Labs
#include "fabric_partition_manager/encoding.hpp"

#include <cstring>

namespace fabric_partition_manager {

void CanonicalWriter::u8(std::uint8_t value) { out_->push_back(static_cast<std::byte>(value)); }

void CanonicalWriter::u16(std::uint16_t value) {
  u8(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
  u8(static_cast<std::uint8_t>(value & 0xFFu));
}

void CanonicalWriter::u32(std::uint32_t value) {
  u8(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
  u8(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
  u8(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
  u8(static_cast<std::uint8_t>(value & 0xFFu));
}

void CanonicalWriter::u64(std::uint64_t value) {
  u32(static_cast<std::uint32_t>((value >> 32) & 0xFFFF'FFFFu));
  u32(static_cast<std::uint32_t>(value & 0xFFFF'FFFFu));
}

void CanonicalWriter::text(std::string_view value) {
  const std::size_t size = value.size();
  u32(static_cast<std::uint32_t>(size & 0xFFFF'FFFFu));
  bytes(reinterpret_cast<const std::byte*>(value.data()), size);
}

void CanonicalWriter::bytes(const std::byte* data, std::size_t count) {
  if (count == 0) {
    return;
  }
  const std::size_t base = out_->size();
  out_->resize(base + count);
  std::memcpy(out_->data() + base, data, count);
}

void CanonicalWriter::digest(const Digest& value) {
  bytes(value.bytes.data(), value.bytes.size());
}

void CanonicalWriter::fixed_id(const std::byte* data, std::size_t count) { bytes(data, count); }

bool CanonicalReader::need(std::size_t count) noexcept {
  if (failed_) {
    return false;
  }
  if (count > size_ - offset_) {
    failed_ = true;
    return false;
  }
  return true;
}

bool CanonicalReader::u8(std::uint8_t& out) noexcept {
  if (!need(1)) {
    return false;
  }
  out = static_cast<std::uint8_t>(data_[offset_]);
  ++offset_;
  return true;
}

bool CanonicalReader::u16(std::uint16_t& out) noexcept {
  std::uint8_t high = 0;
  std::uint8_t low = 0;
  if (!u8(high) || !u8(low)) {
    return false;
  }
  out = static_cast<std::uint16_t>((static_cast<std::uint16_t>(high) << 8) | low);
  return true;
}

bool CanonicalReader::u32(std::uint32_t& out) noexcept {
  std::uint16_t high = 0;
  std::uint16_t low = 0;
  if (!u16(high) || !u16(low)) {
    return false;
  }
  out = (static_cast<std::uint32_t>(high) << 16) | static_cast<std::uint32_t>(low);
  return true;
}

bool CanonicalReader::u64(std::uint64_t& out) noexcept {
  std::uint32_t high = 0;
  std::uint32_t low = 0;
  if (!u32(high) || !u32(low)) {
    return false;
  }
  out = (static_cast<std::uint64_t>(high) << 32) | static_cast<std::uint64_t>(low);
  return true;
}

bool CanonicalReader::boolean(bool& out) noexcept {
  std::uint8_t raw = 0;
  if (!u8(raw)) {
    return false;
  }
  if (raw > 1u) {
    failed_ = true;
    return false;
  }
  out = raw == 1u;
  return true;
}

bool CanonicalReader::text(std::string_view& out) noexcept {
  std::uint32_t length = 0;
  if (!u32(length)) {
    return false;
  }
  if (static_cast<std::size_t>(length) > size_ - offset_) {
    failed_ = true;
    return false;
  }
  out = std::string_view(reinterpret_cast<const char*>(data_ + offset_),
                         static_cast<std::size_t>(length));
  offset_ += static_cast<std::size_t>(length);
  return true;
}

bool CanonicalReader::bounded_text(std::size_t max_bytes, std::string_view& out) noexcept {
  std::uint32_t length = 0;
  if (!u32(length)) {
    return false;
  }
  const auto declared = static_cast<std::size_t>(length);
  if (declared > max_bytes) {
    failed_ = true;
    return false;
  }
  if (declared > size_ - offset_) {
    failed_ = true;
    return false;
  }
  out = std::string_view(reinterpret_cast<const char*>(data_ + offset_), declared);
  offset_ += declared;
  return true;
}

bool CanonicalReader::bytes(std::size_t count, const std::byte*& out) noexcept {
  if (!need(count)) {
    return false;
  }
  out = data_ + offset_;
  offset_ += count;
  return true;
}

bool CanonicalReader::digest(Digest& out) noexcept {
  const std::byte* raw = nullptr;
  if (!bytes(digest_bytes, raw)) {
    return false;
  }
  std::memcpy(out.bytes.data(), raw, digest_bytes);
  return true;
}

bool CanonicalReader::take_region(std::size_t count, CanonicalReader& out) noexcept {
  if (!need(count)) {
    return false;
  }
  out = CanonicalReader(data_ + offset_, count);
  offset_ += count;
  return true;
}

}  // namespace fabric_partition_manager
