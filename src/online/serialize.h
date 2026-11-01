#pragma once

// A tiny, explicit little-endian packet codec. It is deliberately not a
// general serialization framework: packet layouts stay visible at call sites,
// and malformed/truncated input returns nullopt instead of allocating or
// throwing across a network boundary.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace afterhours::online {

class Writer {
public:
  void u8(std::uint8_t value) { bytes_.push_back(static_cast<std::byte>(value)); }

  void u16(std::uint16_t value) {
    u8(static_cast<std::uint8_t>(value & 0xffu));
    u8(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
  }

  void u32(std::uint32_t value) {
    u16(static_cast<std::uint16_t>(value & 0xffffu));
    u16(static_cast<std::uint16_t>((value >> 16u) & 0xffffu));
  }

  void u64(std::uint64_t value) {
    u32(static_cast<std::uint32_t>(value & 0xffffffffu));
    u32(static_cast<std::uint32_t>((value >> 32u) & 0xffffffffu));
  }

  void i32(std::int32_t value) {
    u32(static_cast<std::uint32_t>(value));
  }

  void f32(float value) { u32(std::bit_cast<std::uint32_t>(value)); }

  void boolean(bool value) { u8(value ? 1u : 0u); }

  void raw(std::span<const std::byte> data) {
    bytes_.insert(bytes_.end(), data.begin(), data.end());
  }

  void string(std::string_view value) {
    u32(static_cast<std::uint32_t>(value.size()));
    for (char c : value)
      bytes_.push_back(static_cast<std::byte>(c));
  }

  [[nodiscard]] std::span<const std::byte> span() const { return bytes_; }
  [[nodiscard]] const std::vector<std::byte> &bytes() const { return bytes_; }
  std::vector<std::byte> take() { return std::move(bytes_); }

private:
  std::vector<std::byte> bytes_;
};

class Reader {
public:
  explicit Reader(std::span<const std::byte> data) : data_(data) {}

  [[nodiscard]] std::optional<std::uint8_t> u8() {
    if (!take(1))
      return std::nullopt;
    return std::to_integer<std::uint8_t>(data_[offset_ - 1]);
  }

  [[nodiscard]] std::optional<std::uint16_t> u16() {
    auto lo = u8();
    auto hi = u8();
    if (!lo || !hi)
      return std::nullopt;
    return static_cast<std::uint16_t>(*lo | (static_cast<unsigned>(*hi) << 8u));
  }

  [[nodiscard]] std::optional<std::uint32_t> u32() {
    auto lo = u16();
    auto hi = u16();
    if (!lo || !hi)
      return std::nullopt;
    return static_cast<std::uint32_t>(*lo) |
           (static_cast<std::uint32_t>(*hi) << 16u);
  }

  [[nodiscard]] std::optional<std::uint64_t> u64() {
    auto lo = u32();
    auto hi = u32();
    if (!lo || !hi)
      return std::nullopt;
    return static_cast<std::uint64_t>(*lo) |
           (static_cast<std::uint64_t>(*hi) << 32u);
  }

  [[nodiscard]] std::optional<std::int32_t> i32() {
    auto value = u32();
    if (!value)
      return std::nullopt;
    return static_cast<std::int32_t>(*value);
  }

  [[nodiscard]] std::optional<float> f32() {
    auto value = u32();
    if (!value)
      return std::nullopt;
    return std::bit_cast<float>(*value);
  }

  [[nodiscard]] std::optional<bool> boolean() {
    auto value = u8();
    if (!value || *value > 1u)
      return std::nullopt;
    return *value != 0u;
  }

  [[nodiscard]] std::optional<std::string> string() {
    auto size = u32();
    if (!size || !take(*size))
      return std::nullopt;
    const std::byte *begin = data_.data() + offset_ - *size;
    return std::string{reinterpret_cast<const char *>(begin), *size};
  }

  [[nodiscard]] bool exhausted() const { return offset_ == data_.size(); }
  [[nodiscard]] std::size_t remaining() const { return data_.size() - offset_; }

private:
  bool take(std::size_t count) {
    if (offset_ + count < offset_ || offset_ + count > data_.size())
      return false;
    offset_ += count;
    return true;
  }

  std::span<const std::byte> data_;
  std::size_t offset_ = 0;
};

} // namespace afterhours::online
