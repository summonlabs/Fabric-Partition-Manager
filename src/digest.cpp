// Fabric Partition Manager 1.0.0 - Summon Software Labs
#include "fabric_partition_manager/digest.hpp"

#include <cstring>

namespace fabric_partition_manager {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

constexpr std::array<std::uint32_t, 8> kInitialState = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u,
                                                        0xa54ff53au, 0x510e527fu, 0x9b05688cu,
                                                        0x1f83d9abu, 0x5be0cd19u};

[[nodiscard]] constexpr std::uint32_t rotate_right(std::uint32_t value, unsigned bits) noexcept {
  return (value >> bits) | (value << (32u - bits));
}

struct Crc64Table {
  std::uint64_t entries[256] = {};

  Crc64Table() noexcept {
    // The reflected form of the CRC-64/XZ polynomial 0x42F0E1EBA9EA3693. A
    // reflected (least-significant-bit-first) table must be built from the
    // reversed polynomial; using the forward constant here silently produces a
    // different, non-standard CRC.
    constexpr std::uint64_t kPolynomial = 0xC96C5795D7870F42ull;
    for (std::uint32_t index = 0; index < 256; ++index) {
      std::uint64_t value = static_cast<std::uint64_t>(index);
      for (int bit = 0; bit < 8; ++bit) {
        value = ((value & 1u) != 0) ? ((value >> 1) ^ kPolynomial) : (value >> 1);
      }
      entries[index] = value;
    }
  }
};

[[nodiscard]] const Crc64Table& crc64_table() noexcept {
  static const Crc64Table table;
  return table;
}

}  // namespace

std::string Digest::hex() const {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string result;
  result.reserve(digest_bytes * 2);
  for (const std::byte value : bytes) {
    const auto raw = static_cast<unsigned>(value);
    result.push_back(kDigits[(raw >> 4) & 0xFu]);
    result.push_back(kDigits[raw & 0xFu]);
  }
  return result;
}

bool Digest::is_zero() const noexcept {
  for (const std::byte value : bytes) {
    if (value != std::byte{0}) {
      return false;
    }
  }
  return true;
}

std::string_view Digest::prefix8() const noexcept {
  static thread_local std::string rendered;
  rendered = hex().substr(0, 16);
  return rendered;
}

Sha256::Sha256() noexcept : state_(kInitialState) {}

void Sha256::compress(const std::uint8_t* block) noexcept {
  std::uint32_t schedule[64] = {};
  for (std::size_t index = 0; index < 16; ++index) {
    schedule[index] = (static_cast<std::uint32_t>(block[index * 4]) << 24) |
                      (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16) |
                      (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8) |
                      static_cast<std::uint32_t>(block[index * 4 + 3]);
  }
  for (std::size_t index = 16; index < 64; ++index) {
    const std::uint32_t s0 = rotate_right(schedule[index - 15], 7) ^
                             rotate_right(schedule[index - 15], 18) ^ (schedule[index - 15] >> 3);
    const std::uint32_t s1 = rotate_right(schedule[index - 2], 17) ^
                             rotate_right(schedule[index - 2], 19) ^ (schedule[index - 2] >> 10);
    schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t index = 0; index < 64; ++index) {
    const std::uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
    const std::uint32_t choice = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + s1 + choice + kRoundConstants[index] + schedule[index];
    const std::uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(const void* data, std::size_t size) noexcept {
  if (finalized_ || size == 0 || data == nullptr) {
    return;
  }
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  total_bytes_ += size;
  std::size_t offset = 0;
  if (buffer_used_ > 0) {
    const std::size_t available = 64 - buffer_used_;
    const std::size_t take = size < available ? size : available;
    std::memcpy(buffer_.data() + buffer_used_, bytes, take);
    buffer_used_ += take;
    offset += take;
    if (buffer_used_ == 64) {
      compress(buffer_.data());
      buffer_used_ = 0;
    }
  }
  while (size - offset >= 64) {
    compress(bytes + offset);
    offset += 64;
  }
  if (offset < size) {
    std::memcpy(buffer_.data(), bytes + offset, size - offset);
    buffer_used_ = size - offset;
  }
}

void Sha256::update_u8(std::uint8_t value) noexcept { update(&value, 1); }

void Sha256::update_be16(std::uint16_t value) noexcept {
  const std::uint8_t encoded[2] = {static_cast<std::uint8_t>((value >> 8) & 0xFFu),
                                   static_cast<std::uint8_t>(value & 0xFFu)};
  update(encoded, 2);
}

void Sha256::update_be32(std::uint32_t value) noexcept {
  const std::uint8_t encoded[4] = {static_cast<std::uint8_t>((value >> 24) & 0xFFu),
                                   static_cast<std::uint8_t>((value >> 16) & 0xFFu),
                                   static_cast<std::uint8_t>((value >> 8) & 0xFFu),
                                   static_cast<std::uint8_t>(value & 0xFFu)};
  update(encoded, 4);
}

void Sha256::update_be64(std::uint64_t value) noexcept {
  update_be32(static_cast<std::uint32_t>((value >> 32) & 0xFFFF'FFFFu));
  update_be32(static_cast<std::uint32_t>(value & 0xFFFF'FFFFu));
}

void Sha256::update_text(std::string_view text) noexcept {
  const std::uint64_t length = static_cast<std::uint64_t>(text.size());
  update_be32(static_cast<std::uint32_t>(length & 0xFFFF'FFFFu));
  update(text.data(), text.size());
}

Digest Sha256::finish() noexcept {
  Digest result;
  if (!finalized_) {
    const std::uint64_t bit_length = total_bytes_ * 8u;
    const std::uint8_t marker = 0x80u;
    const std::uint8_t zero = 0u;
    update(&marker, 1);
    while (buffer_used_ != 56) {
      update(&zero, 1);
    }
    std::uint8_t encoded_length[8] = {};
    for (std::size_t index = 0; index < 8; ++index) {
      encoded_length[index] =
          static_cast<std::uint8_t>((bit_length >> ((7u - index) * 8u)) & 0xFFu);
    }
    update(encoded_length, 8);
    finalized_ = true;
  }
  for (std::size_t index = 0; index < 8; ++index) {
    const std::uint32_t word = state_[index];
    result.bytes[index * 4] = static_cast<std::byte>((word >> 24) & 0xFFu);
    result.bytes[index * 4 + 1] = static_cast<std::byte>((word >> 16) & 0xFFu);
    result.bytes[index * 4 + 2] = static_cast<std::byte>((word >> 8) & 0xFFu);
    result.bytes[index * 4 + 3] = static_cast<std::byte>(word & 0xFFu);
  }
  return result;
}

Digest sha256_bytes(const void* data, std::size_t size) noexcept {
  Sha256 hasher;
  hasher.update(data, size);
  return hasher.finish();
}

Digest sha256_text(std::string_view text) noexcept {
  return sha256_bytes(text.data(), text.size());
}

void Crc64::update(const void* data, std::size_t size) noexcept {
  if (size == 0 || data == nullptr) {
    return;
  }
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  const Crc64Table& table = crc64_table();
  std::uint64_t value = value_;
  for (std::size_t index = 0; index < size; ++index) {
    const auto slot = static_cast<std::uint8_t>((value ^ bytes[index]) & 0xFFu);
    value = table.entries[slot] ^ (value >> 8);
  }
  value_ = value;
}

void Crc64::update_u8(std::uint8_t value) noexcept { update(&value, 1); }

void Crc64::update_be32(std::uint32_t value) noexcept {
  const std::uint8_t encoded[4] = {static_cast<std::uint8_t>((value >> 24) & 0xFFu),
                                   static_cast<std::uint8_t>((value >> 16) & 0xFFu),
                                   static_cast<std::uint8_t>((value >> 8) & 0xFFu),
                                   static_cast<std::uint8_t>(value & 0xFFu)};
  update(encoded, 4);
}

void Crc64::update_be64(std::uint64_t value) noexcept {
  update_be32(static_cast<std::uint32_t>((value >> 32) & 0xFFFF'FFFFu));
  update_be32(static_cast<std::uint32_t>(value & 0xFFFF'FFFFu));
}

std::uint64_t crc64_bytes(const void* data, std::size_t size) noexcept {
  Crc64 crc;
  crc.update(data, size);
  return crc.value() ^ 0xFFFF'FFFF'FFFF'FFFFull;
}

std::uint64_t crc64_text(std::string_view text) noexcept {
  return crc64_bytes(text.data(), text.size());
}

}  // namespace fabric_partition_manager
