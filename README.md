# afterhours-online

Drop-in multiplayer plumbing for afterhours games.

The core is **header-only**: add this folder to a game's include path and
include only what the game uses. The core does not add a `.cpp` file, a link
flag, Steamworks, or GNS to a build.

| Header | What it provides |
| --- | --- |
| `src/online/core.h` | `Transport`, peer IDs, channels, packets, events |
| `src/online/serialize.h` | Tiny explicit little-endian packet `Writer` / `Reader` |
| `src/online/local.h` | In-process server/client transport for single-player and tests |
| `src/online/online.h` | Convenience include for all of the above |

Backends that need a real library (standalone GNS first, Steamworks later)
are intentionally **opt-in**: a game that wants one adds that backend's one
`.cpp` file and one link flag to its own build. Games that do not want it
never compile or link it.

## GNS backend (direct IP)

- `src/online/gns.h` — public `GnsTransport` (no GNS headers leak through).
- `src/online/gns_impl.h` — implementation, included by `gns.cpp` only.
- `src/online/gns.cpp` — the one TU a game adds, plus the static library.
- `scripts/build-gns.sh` — builds pinned GNS v1.6.1 (interface
  `SteamNetworkingSockets012` / `SteamNetworkingSockets_LibV12()`) into
  `vendor/gns/` in ~1–2 minutes. Idempotent; nothing rebuilds per game.
- `tests/online_gns_test.cpp` — server+client loopback over real UDP:
  connect, reliable/unreliable delivery, peer ids, channel mapping, drop.

Server allocates peer ids (`kServerPeer`, then `kFirstRemotePeer` up, cap
4); a client's local id stays invalid until the game handshake calls
`set_local_peer_id`. Channels map 1:1 to GNS send flags. Known dev-machine
caveat: `libGameNetworkingSockets_s.a` is static, but Homebrew ships no
static protobuf/Abseil, so local builds still link those as dylibs; the
release pipeline should build protobuf statically too (see script header).

`pumpkin-beachball` (Slime Volleyball) is the first consumer. This folder is
kept self-contained so it can be split into its own repository without
changing the public header surface.
