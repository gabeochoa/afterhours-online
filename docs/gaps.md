# afterhours-online gaps and backlog

Last reviewed: 2026-11-05, against this repository at `fb97f1c` and
its first consumer, Pumpkin Beachball at `73de546`.

This file is the library-owned backlog. Pumpkin Beachball keeps the
consumer-side evidence and game-integration limits in its
`docs/afterhours-online-gaps.md`; entries below should stay at the
transport/API level and should not grow game-specific packet schemas.

## Current surface

- Header-only `core.h`, `serialize.h`, `local.h`, `online.h`, and
  optional `testing.h`.
- Standalone GNS through one opt-in translation unit, `gns.cpp`.
- No ECS dependency in the core. A game supplies its own session state,
  handshake, snapshots, menus, and HUD.
- Local and GNS transports share one event model: `Connected`,
  `Disconnected`, and opaque `Packet`s over reliable, unreliable, and
  unreliable-no-delay channels.

## Confirmed library gaps

### 1. Early client failure has no terminal event

The GNS client queues `Disconnected` for a closing connection only if
it previously reached `Connected`. An unreachable endpoint can
therefore leave a consumer waiting forever unless the game adds its
own deadline and cancellation UI.

Resolution (2026-11-05): closed. The GNS client now queues
`Disconnected{kServerPeer}` when its connection reaches a closed/problem
state before `Connected`; local close paths unregister the connection
first, so they do not synthesize a second event. A loopback probe
observed GNS's own initial timeout event at 10.012 seconds, and
`connect_without_server` now waits up to 12 seconds and expects the
event under `--strict-gaps`. Games that want a shorter UX deadline
should still impose their own; Pumpkin Beachball uses 12 seconds of
frame dt as a backstop for endpoints that accept the transport
connection but never complete the game handshake.

### 2. Capacity rejection is silent at the transport layer

When the GNS server already has four remote peers, it closes a new
connection during the Connecting callback. The accepted clients keep
working, but the rejected client gets no explicit terminal event and
the server gets no dedicated rejection event.

The transport matrix records this as `capacity_and_extra_client`
XFAIL. A game can cover lower game-specific caps with its own Bye
message after Hello, as Pumpkin Beachball does for its third player,
but that does not help a fifth transport peer or another consumer
whose game cap equals the backend cap.

Resolution (2026-11-05): closed for the terminal-event guarantee.
The rejected client now receives `Disconnected{kServerPeer}` from its
own closed connection, and `capacity_and_extra_client` expects it
under `--strict-gaps`. Distinguishing capacity rejection from other
closes still needs the reason field in item 3.

### 3. Disconnect reasons and connection stats stay inside GNS

Resolution (2026-11-05): closed. `Disconnected` now carries a
portable `DisconnectReason` (`Unknown`, `ClosedByPeer`, `Unreachable`,
`Rejected`, `LocalProblem`, `LocalClose`); the GNS backend maps its
native end reasons onto it, including a dedicated app-range code for
capacity rejection, and LocalTransport reports `LocalClose` to the
initiator and `ClosedByPeer` to everyone else. `Transport` also gained
an optional `connection_stats()` query (ping, quality, rates, pending
bytes, queue time, jitter); GNS fills it from real-time status and
LocalTransport reports a perfect in-process link. Games may still want
their own application-level ping as an end-to-end game-loop check;
Pumpkin Beachball keeps its Ping/Pong for exactly that reason.

### 4. Address parsing is narrower than the API suggests

`Transport::connect(host, port)` takes a host string, but the GNS
backend passes `host:port` to `SteamNetworkingIPAddr::ParseString`.
There is no DNS resolution in the library. A hostname therefore fails
as a bad address even though the parameter is named `host`, not `ip`.

IPv6 is also not exercised end to end: GNS address types support IPv6
and bracketed literals, but consumers may impose their own single-colon
`host:port` parser before the library sees the value.

Decide and document one endpoint contract:

- numeric IP only, with a type/name that says so and bracketed IPv6
  tests; or
- endpoint strings that may be DNS names, resolved by the backend with
  explicit resolution errors and an IPv4/IPv6 selection policy.

### 5. The ECS layer from the original plan is absent

The core deliberately has no ECS dependency. That part works: a game
can include the core without changing its link line. What does not
exist is the optional second layer from the original design:
`plugin.h`/`ecs.h`, a session singleton, peer/input components, and
poll/send/session systems built only on public Afterhours APIs.

Pumpkin Beachball is therefore both the first consumer and the de
facto reference integration. A second game would copy its session,
peer mapping, handshake state, remote-intent latching, cleanup, and
status plumbing.

Decision needed: build that optional ECS adapter, or explicitly keep
this repository transport-only and stop describing it as an ECS
plugin. The adapter should live in its own header so `core.h` remains
dependency-free.

### 6. Snapshot sampling and send pacing are repeated by consumers

The library provides transport, serialization, a sequence envelope for
tests, and fault injection. It does not provide the pieces every
snapshot game needs next:

- a bounded snapshot buffer sampled at a configurable delay;
- stale/duplicate/out-of-order accounting for application snapshots;
- an adaptive interpolation delay based on arrival jitter;
- a wall-clock rate gate for inputs, snapshots, or probes;
- input acknowledgement or redundancy helpers for one-frame edges.

Keep schemas, authority, movement prediction, and reconciliation in
the game. The missing reusable boundary is timing and buffer
bookkeeping, not a universal snapshot type.

### 7. Distribution is still a local macOS/Homebrew build

`scripts/build-gns.sh` is the only acquisition path. It clones GNS
`v1.6.1`, assumes CMake and Homebrew protobuf/Abseil/OpenSSL paths,
and builds on the developer machine. There is no CI workflow, release
tag, published per-platform artifact, checksum file, or verified
`fetch-gns.sh`.

The resulting GNS archive is static, but Homebrew protobuf and Abseil
still link dynamically on the current dev machine. A shipped game
must inspect its final binary rather than assume the archive made the
whole dependency chain static.

Release work still needed:

1. CI builds with fully static protobuf/Abseil/OpenSSL where the
   platform permits it.
2. Versioned release assets and SHA256 sums.
3. A fetch script that selects the current OS/arch and fails clearly
   when no artifact exists.
4. A fresh-clone job that fetches, compiles the core and GNS tests,
   and runs loopback tests without building GNS from source.
5. A top-level `LICENSE` for this repository. The only license file in
   a built checkout today belongs to the downloaded GNS tree under
   ignored `vendor/gns/`; it does not license afterhours-online.
6. A compatibility table: library revision, tested Afterhours
   revision, GNS/interface version, OS/arch, and compiler.

### 8. The Steam sharing seam needs a bootstrap refactor

There is no Steamworks backend yet. More importantly, the shared
implementation is not quite backend-agnostic today:
`gns_acquire()` calls the standalone init/kill functions and
`SteamNetworkingSockets_LibV12()` itself, despite the file header
saying the including translation unit hands it an interface pointer.

Split acquisition/lifetime from connection logic before adding
`steam.cpp`. Each backend TU should own init, shutdown, utils access,
and accessor selection. Add an SDK-gated or fake-interface compile
check so the seam cannot silently hardcode the standalone path again.

### 9. Session admission is accept-first, game-reject-later

The GNS server accepts a connection during the Connecting callback if
capacity remains. Identity, password, role, and game-version checks
can only happen after the game receives its first application packet.
That is workable for a small trusted game, but the library exposes no
pre-accept policy hook, connection user data, or structured rejection
reason for consumers that need one.

This is not a call to put game passwords in the transport. It is a
missing extension point: a consumer callback could inspect native
connection info (and later an application hello) and accept, reject,
or park the connection without forking `gns_impl.h`.

### 10. Serialization strategy is not settled: hand codec or zpp::bits

Status: decision/spike needed. This is not yet a defect in the
current two-player protocol.

Why `serialize.h` exists today:

- The transport contract is opaque bytes. The library needed a tiny,
  dependency-free way for tests and consumers to put integers, floats,
  bools, and strings on the wire without adopting a schema framework.
- Its layout is explicit and stable: little-endian primitives,
  length-prefixed strings, and readers that return `nullopt` when the
  buffer is short. Packet layouts remain visible in each game's
  encode/decode functions, and raw-probe tools can inspect them.
- It is only about 145 lines and has no third-party header, template
  archive, or warning-suppression wrapper to carry into every game.

Why zpp::bits is attractive:

- Pharmasea already uses it for variant packets and, more importantly,
  entity/world snapshot blobs. It removes repetitive field-by-field
  code once schemas contain nested structs, containers, variants,
  bitsets, or component masks.
- It returns `std::errc` instead of throwing across the network
  boundary, and it can be configured for explicit little-endian I/O.

Why replacing `serialize.h` wholesale is not an obvious win today:

- Pharmasea's vendored header is about 5,713 lines and needs a local
  clang `-Wshadow` suppression wrapper in that project.
- Default zpp::bits archives are native-endian. Every archive in a
  wire protocol must deliberately use `zpp::bits::endian::little`;
  forgetting it changes the bytes by host.
- It does not make wire evolution free. Field order, enum order,
  container encodings, and archive options are still protocol
  contracts and still need golden-byte tests.
- Pumpkin Beachball currently has seven small, fixed-layout game
  messages. The hand codec's call sites are short, exact-length
  checked, and already covered by unit and hostile-packet tests.

Recommendation: keep `serialize.h` as the low-level stable wire
primitive for transport/simple game packets. Do not migrate the
existing protocol only to save a few field writes. Reconsider
zpp::bits at the snapshot/ECS layer, where nested component data is
the actual problem. Before adopting it, run a spike on Hello, Input,
and Snapshot and compare:

1. golden bytes and malformed/truncated-input behavior;
2. explicit little-endian configuration at every archive site;
3. call-site size and readability against the current codec;
4. compile time and warnings under `-Werror`;
5. multi-translation-unit/include behavior;
6. vendoring, license, and version-pinning cost.

### 11. The public headers do not read like an Afterhours plugin

Status: design/style decision needed; do not fix by formatting alone.

The mismatch is structural, not just indentation:

- Afterhours features are usually a lowercase plugin struct inheriting
  `developer::Plugin`, with nested `Provides*`/`Has*` components,
  static singleton registration, and systems. `online/core.h` is a
  conventional abstract `Transport` class with virtual methods,
  standalone event structs, and free helper functions.
- Afterhours headers are commonly dense, implementation-heavy headers
  built around `BaseComponent`, `Entity`, `SystemManager`, concepts,
  and project type aliases. The online core intentionally uses only
  standard-library types and says it has no ECS dependency.
- There is no `.clang-format` in either checkout to settle the visible
  differences (including this repo's two-space style versus the
  four-space style used through much of the pinned Afterhours tree).

Recommended direction: keep `core.h` as the neutral transport contract
and put the Afterhours-native surface in the optional ECS adapter from
item 5. That adapter should follow the pinned library's plugin shape.
A repository-wide reformat of stable transport headers would create a
large diff without making the API feel integrated. If the ECS layer is
formally rejected instead, rename the expectation in the README/plan
from "plugin" to "transport library" and then align only new code.

### 12. `gns_impl.h` is still dense for a first-time maintainer

Status: partially mitigated on 2026-11-05; remaining work is tied to
the Steam bootstrap refactor in item 8.

The file is about 510 lines because several different jobs share one
impl: process-wide GNS lifetime, two global callback registries, one
endpoint that can act as either server or client, native-handle maps,
message copying, disconnect-reason mapping, stats, and the GNS state
machine. The global callback and the two inverse server maps are the
parts most likely to stop a first-time reader.

Mitigation already in place:

- `docs/gns-internals.md` walks the file in runtime order: lifetime,
  callback routing, server accept, client connect, send, poll/drain,
  and the three close paths.
- The header now opens with the same reading order and labels the
  process-wide state, callback fall-back, endpoint role, server maps,
  poll ordering, and lifecycle translation at the relevant code.

Do not split the file only to make it look smaller. The meaningful
remaining decomposition is the item 8 bootstrap split: backend
acquisition out, shared connection logic in. If that refactor cannot
make the ownership and callback route obvious without the guide,
revisit separating lifetime/registry state from endpoint logic then.

## Future scope, not current defects

- NAT hole punching, relay, room codes, and rendezvous.
- Steamworks/SDR, lobbies, friends/identity, and matchmaking.
- Rollback/lockstep. The demonstrated model is authoritative snapshots.
- More than the GNS backend's current four remote peers.
- A second real consumer. Until one adopts the library, its API has
  only been proven against a two-player, full-snapshot game.

## Test evidence limits

Covered: codec/local units, a 24-scenario transport contract matrix
over Local and real GNS loopback, fault injection above the transport,
and Pumpkin Beachball's multi-process game scenarios.

Not covered: another physical machine, a real LAN or public internet
path, NAT and firewall behavior, Windows/Linux artifacts, UDP loss
below GNS retransmission, Steam, and hours-long server soak. The
detailed playbook and current XFAIL semantics stay in
`docs/e2e-testing.md`.

## Checked: not gaps

- Channels map to distinct GNS send flags; no path silently upgrades
  everything to reliable.
- Peer IDs are allocated by the server transport, and a client remains
  invalid until the game handshake teaches it the assigned ID. That is
  documented behavior, not a missing ID.
- LocalTransport/GNS differences are represented as backend
  capabilities in the contract runner instead of scattered `#ifdef`s.
- The core really is header-only and does not leak GNS or Steam
  headers. Only consumers that add `gns.cpp` pay the backend link.
- The game label hash is a compatibility guard only. It was never an
  authentication mechanism, and the source says so.

## Maintenance rule

When a gap closes, change its status in place, name the commit, and
name the test that proves it. If an item becomes an accepted permanent
boundary, move it to future scope or record the decision where the
evidence is. Do not let `XFAIL` become the only place a behavior is
documented.

