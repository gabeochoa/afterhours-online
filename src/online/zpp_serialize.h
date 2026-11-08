#pragma once

// The one include point for zpp::bits in this library. It pins the two
// things a wire protocol must never decide per call site:
//
// - Endianness: archives are little-endian on the wire. zpp::bits
//   spells that as a native/swapped option relative to the host, so
//   WireEndian resolves it once, here.
// - Warnings: the vendored header trips -Wshadow on clang (the same
//   suppression Pharmasea's wrapper uses), kept local to this include.
//
// Everything else about zpp::bits (struct serialize functions, errc
// results, position checks) stays visible at the consumer's call
// sites. This header does not replace serialize.h: the explicit
// little-endian Writer/Reader remain for callers that want primitive
// control without a template archive.

#include <bit>
#include <cstddef>
#include <span>
#include <type_traits>
#include <vector>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wshadow"
#endif
#include "third_party/zpp_bits/zpp_bits.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace afterhours::online {

using WireEndian =
    std::conditional_t<std::endian::native == std::endian::little,
                       zpp::bits::endian::native, zpp::bits::endian::swapped>;

using WireOut = zpp::bits::out<std::vector<std::byte>, WireEndian>;
using WireIn = zpp::bits::in<std::span<const std::byte>, WireEndian>;

[[nodiscard]] inline WireOut make_wire_out(std::vector<std::byte> &buffer) {
  return WireOut{buffer, WireEndian{}};
}

[[nodiscard]] inline WireIn make_wire_in(std::span<const std::byte> bytes) {
  return WireIn{bytes, WireEndian{}};
}

} // namespace afterhours::online
