#pragma once

// Typed-packet codec over zpp::bits (OG-18, was Pumpkin ON-1).
//
// Pumpkin Beachball hand-rolled peek_type plus encode/decode pairs
// (type byte + exact-length check) around make_wire_out/make_wire_in
// for its seven messages. This header is that discipline, once:
//
// - encode_typed archives the type byte first, then the message.
// - decode_typed validates the type byte, requires a clean archive
//   read, and requires exact exhaustion (the v1 wire-evolution rule
//   in OG-11: trailing bytes are a different, incompatible packet).
// - peek_typed reads the type byte without decoding the message.
//
// Message types keep their own serialize(archive, msg) free function
// archiving FIELDS ONLY (no type byte; the codec owns it), found by
// ADL exactly as zpp::bits expects. Semantic range checks (enum
// bounds, bool bytes, character indices) stay at the call site, as
// they do in the game's decoders today: decode_typed proves shape,
// the game proves meaning.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

#include "zpp_serialize.h"

namespace afterhours::online {

template <typename Enum>
  requires std::is_enum_v<Enum>
[[nodiscard]] inline std::optional<Enum>
peek_typed(std::span<const std::byte> bytes) {
  if (bytes.empty())
    return std::nullopt;
  return static_cast<Enum>(
      std::to_integer<std::underlying_type_t<Enum>>(bytes.front()));
}

template <typename Enum, typename Message>
  requires std::is_enum_v<Enum>
[[nodiscard]] inline std::vector<std::byte> encode_typed(Enum type,
                                                         Message &message) {
  std::vector<std::byte> bytes;
  auto out = make_wire_out(bytes);
  auto type_byte = static_cast<std::uint8_t>(type);
  (void)out(type_byte, message);
  return bytes;
}

template <typename Enum, typename Message>
  requires std::is_enum_v<Enum>
[[nodiscard]] inline std::optional<Message>
decode_typed(std::span<const std::byte> bytes, Enum expected) {
  if (peek_typed<Enum>(bytes) != expected)
    return std::nullopt;
  Message message{};
  auto in = make_wire_in(bytes);
  std::uint8_t type_byte = 0;
  if (zpp::bits::failure(in(type_byte, message)) ||
      in.position() != bytes.size())
    return std::nullopt;
  return message;
}

} // namespace afterhours::online
