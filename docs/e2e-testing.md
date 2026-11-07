# End-to-end testing for afterhours-online

Online bugs hide in combinations: which process started first, which client
joined first, whether a packet was lost above or below the transport, and
whether a backend reports failure synchronously, asynchronously, or not at
all. The test strategy therefore separates the layers and makes each axis an
explicit option instead of baking one happy path into one script.

## The four layers

| Layer | What runs | What it proves | Entry point |
| --- | --- | --- | --- |
| Codec/unit | In-process assertions | Byte layout, bounds checks, sequence helpers, fault wrapper mechanics | `tests/online_core_test.cpp` |
| Transport contract E2E | Server + clients in one test process | The same scenario contract over `LocalTransport` and real UDP/GNS | `tests/online_e2e_test.cpp` |
| Raw game-protocol E2E | Dedicated game server + fake wire client | Hello/Welcome/Bye, malformed and hostile packets | Consumer's `net-probe` |
| Game process E2E | Dedicated/listen server + real game clients | Side assignment, snapshots, prediction inputs, scoring, disconnect/rejoin, process exit | Consumer's process harness |

The first two layers live in this repository. The last two are consumer-side
because only the game knows its wire protocol and process CLI; Pumpkin
Beachball is the reference implementation (`tests/net_probe.cpp` and
`tests/online_e2e.py` in that repo).

## Quick start

From Pumpkin Beachball, which already has Make targets wired:

```sh
make online-e2e-test      # LocalTransport matrix (24 scenarios)
make online-e2e-gns-test  # Local + GNS over real UDP loopback
make online-e2e           # transport matrix + default game-process scenarios
make online-test          # the above plus the original GNS smoke test
```

Standalone, the library has no build system; compile the test like any other
consumer TU:

```sh
# Local only — no dependencies.
clang++ -std=c++23 -Wall -Wextra -Wpedantic -Werror \
  tests/online_e2e_test.cpp -o online-e2e-test

# Local + GNS — needs the static GNS build from scripts/build-gns.sh.
clang++ -std=c++23 -Wall -Wextra -Wpedantic -Werror -DONLINE_E2E_WITH_GNS \
  -Ivendor/gns/include tests/online_e2e_test.cpp src/online/gns.cpp \
  vendor/gns/lib/libGameNetworkingSockets_s.a \
  $(pkg-config --libs protobuf) -lcrypto -lssl -o online-e2e-gns-test
```

## Transport runner options

`online_e2e_test --help` is authoritative. The important axes:

### Selection

- `--backend=local|gns|both`
- `--scenario=name[,name]` (repeatable), `--filter=text`, `--list`
- `--repeat=N`, `--fail-fast`, `--json`, `--verbose`
- `--strict-gaps` turns known gaps (`XFAIL`) into a failing exit code.

### Setup

- `--clients=N` for generic multi-client scenarios.
- `--host=127.0.0.1`, `--port-base=28100`. The GNS driver scans upward from
  the base until a listen succeeds, so a busy developer machine or a
  previous crashed run does not pick the test result.
- `--timeout-ms`, `--poll-ms` bound every wait. No scenario should sleep a
  fixed "hopefully long enough" duration and then assert; it should pump
  until a predicate holds or the timeout expires.
- `--flood-count`, `--payload-max` scale stress and boundary scenarios
  down for laptops or up for soak runs.
- `--seed` makes every randomized fault reproducible. CI failures should
  always print the seed; the runner banner does.

### Fault injection

`--fault-profile=clean|laggy|lossy|chaos|custom` configures the profile used
by `fault_profile_soak`. Individual overrides are order-sensitive and layer
on top of the profile named before them:

- Loss: `--drop-first-sends`, `--drop-send-rate`, `--drop-receive-rate`,
  `--dropped-send-reports-success`
- Duplication: `--duplicate-first-sends`, `--duplicate-send-rate`
- Corruption: `--corruption=none|flip|truncate`, `--corrupt-first-sends`,
  `--corrupt-send-rate`, `--corrupt-offset`, `--truncate-to`
- Time: `--latency-ms`, `--jitter-ms`, `--reorder-every`,
  `--reorder-extra-ms`, `--max-queued-events`
- Channel confusion: `--force-channel=reliable|unreliable|nodelay|none`
- Lifecycle events: `--apply-to-lifecycle-events=true` also delays/drops
  Connected/Disconnected, not just packets.

Example soak runs:

```sh
online-e2e-gns-test --scenario fault_profile_soak --fault-profile lossy --seed 7
online-e2e-gns-test --scenario fault_profile_soak --fault-profile custom \
  --drop-send-rate 0.3 --duplicate-send-rate 0.1 --latency-ms 80 --jitter-ms 40
```

`src/online/testing.h` provides the reusable pieces: `EventPump` with
predicate waits, event matchers, a checksummed sequence envelope +
`SequenceTracker` (dedupe/inversion/missing accounting), and
`FaultTransport`, a decorator that wraps any `Transport`.

**Layer caveat:** `FaultTransport` sits above the real transport. Dropping a
send there models an application packet that never entered the wire. It does
not model UDP datagram loss underneath GNS retransmission. For that layer,
put a UDP proxy or OS traffic shaping between processes; the game-process
harness is the place to add it.

## Result semantics

- `PASS` — the portable contract held.
- `FAIL` — a scenario expectation failed; the run reports every failed
  expectation collected before the abort point, not just the first.
- `SKIP` — reserved for setups a scenario cannot express.
- `XFAIL` — a known gap was observed exactly as documented. The run is green
  by default; `--strict-gaps` makes it red. This keeps gaps visible in every
  CI log without either hiding them or blocking unrelated work.

There are currently no `XFAIL`s on GNS. The two former early-close gaps
(`connect_without_server` and `capacity_and_extra_client`) closed on
2026-11-05: a client connection that ends before `Connected` now queues
exactly one terminal `Disconnected` event. GNS's unreachable-endpoint
timeout is about 10 seconds on this platform, so games that want a
shorter or handshake-specific deadline still need their own timer.

## Backend differences are data, not `#ifdef`s

`LocalTransport` and `GnsTransport` intentionally diverge in a few places.
The runner encodes those differences in one `BackendCaps` table per driver;
scenarios assert the portable parts unconditionally and branch only on caps:

| Behavior | Local | GNS |
| --- | --- | --- |
| Client peer id | Assigned inside `connect()` | Invalid until the game learns it from its Welcome |
| Connect with no server | `connect()` returns false | Async; terminal `Disconnected` after GNS initial timeout (~10 s here) |
| Client → client send | Routed directly by the hub | Server-bound; the peer argument does not select a client |
| Client broadcast | Reaches server and clients | Reaches the server only |
| Received `UnreliableNoDelay` label | Preserved | Reported as `Unreliable` (GNS message flags only expose reliable) |
| Peer capacity | Unlimited | 4 remote peers |
| Unreliable order/loss on loopback | Complete and FIFO | No contract; scenarios only assert progress and no duplication |

When a new backend lands, filling in its caps table is most of the porting
work for the whole scenario matrix.

## Handling setups and failure modes

These are the recurring ways online E2E goes flaky or misleading, and how the
harnesses above defuse each one:

- **Port collisions.** Library tests scan from `--port-base`; the game
  harness reserves an ephemeral UDP port from the OS per scenario
  (`--port 0`, the default). Fixed ports are opt-in for debugging only.
- **Startup races.** Never sleep-and-hope. The game harness waits for log
  readiness (`dedicated listening`, `joined as left`, ...) before spawning
  the next process. Transport scenarios pump until `Connected` predicates
  hold.
- **Join order decides sides.** The first Hello gets Left. Sequential
  scenarios wait for the server's join log between spawns; the simultaneous
  scenario deliberately does not, and its scripts must be side-agnostic
  (Pumpkin Beachball uses a `hold_away_from_center` E2E command that keys
  off the assigned side instead of assuming process A is Left).
- **Peer-id mapping under concurrency.** Server `Connected` order is not a
  portable client-index mapping. The simultaneous scenario has each client
  announce a token and maps `Packet.from` back to the client index.
- **The game always sends a correct Hello.** That makes handshake error
  paths unreachable from the real binary. The raw `net-probe` fills the gap:
  one small binary, many `--mode`s (wrong version/hash, truncated Hello,
  duplicate Hello, garbage first, input before Hello, wrong-direction
  packets, input flood, slow/no Hello).
- **A process asserting before its peer sees the result.** An authoritative
  host can observe a score and exit before the next snapshot reaches the
  guest. Scripts that drive shared state need a trailing wait after their
  final assertion so peers can observe the same state before teardown.
- **Frame counts are not wall time.** Headless clients can burn scripted
  frames far faster than a real-time dedicated server advances, while real
  windows are vsync-paced. Give score/state waits enough wall-clock margin
  and keep process timeouts above the slowest expected frame burn; a
  too-short wait reports the old state, and a too-long wait can itself hit
  the harness deadline under load.
- **Graceful vs ungraceful death.** `close()`, process exit through cleanup,
  `terminate`, and `kill` produce different wire behavior. Scenarios should
  name which one they mean; "server exits" (timed, graceful) and
  "port occupied" are different tests from a hard kill.
- **Cleanup.** Every process the harness spawns is tracked and stopped in
  reverse order even on failure, with per-process logs written under
  `build/online-e2e-logs/` for failed scenarios.

## Writing a transport scenario

1. Add a `scenario_*` function and register it in `all_scenarios()` with a
   one-line description.
2. Build state through `Session` (it owns endpoints, learns peer ids, and
   keeps one cumulative event log). Use `ctx.require` for setup steps and
   `ctx.expect` for assertions so one failure does not hide the rest.
3. Wait with `pump_until(predicate)` on the recorded log. Copy packets out
   of the log before pumping again (the vector can reallocate).
4. If behavior legitimately differs by backend, add or reuse a `BackendCaps`
   flag — do not branch on the backend name inside the scenario.
5. If the desired behavior does not hold yet, assert it with `ctx.gap(...)`
   so the run reports `XFAIL` instead of silently encoding the bug as the
   expectation.
6. Make traffic observable: use the sequence envelope so loss, duplicates,
   and reordering are countable facts, not log archaeology.

## Game-process harness options (Pumpkin Beachball reference)

`tests/online_e2e.py --help` lists everything; the shape worth copying:

- `--scenario` / `--all` / `--list`, `--repeat`, `--fail-fast`
- `--exe`, `--probe` to point at alternate builds
- `--windowed` with `--windowed-exe` registers real-window client
  scenarios; they need a live display and write per-process screenshots
- `--port` (0 = dynamic per scenario), `--timeout`, `--startup-timeout`
- `--characters LEFT,RIGHT`, `--probe-modes`, `--hello-delay-ms`,
  `--input-count`, `--server-seconds`, `--server-exit-seconds`
- `--verbose`, `--keep-logs`, `--log-dir`

Scenarios cover dedicated and listen-host topologies, sequential / reverse /
simultaneous joins, third-client rejection, unreachable and unparseable
addresses, port conflicts, graceful server exit, client leave + replacement,
same-process rejoin, and the raw-probe handshake matrix. They also include a
menu-driven headless match (real Online menu, typed join target, character
ready-up) and, under `--windowed`, dedicated and listen-host matches in two
real client windows.

## Suggested CI tiers

| Tier | Command | When |
| --- | --- | --- |
| Fast | `online-e2e-test --backend=local` + core unit test | Every change; no GNS build needed |
| Real transport | `online-e2e-gns-test --backend=both` | Every change on a runner with the static GNS build |
| Game smoke | `online_e2e.py` (default scenarios) | Every change touching netcode or the game protocol |
| Windowed game | `online_e2e.py --windowed --scenario dedicated_windowed,listen_windowed` | Display-equipped runner or manual pre-release check; validates real rendering plus networking |
| Full matrix | `online_e2e.py --all` | Nightly or pre-release; several minutes of real time |
| Soak | `online-e2e-gns-test --repeat 5 --fault-profile chaos` | Nightly; watch for new `FAIL`s and shrinking `XFAIL`s |

If the GNS static library is absent, the fast tier still validates the full
scenario logic against `LocalTransport`; only the wire itself is unproven.
That distinction should stay explicit in CI output rather than being hidden
behind a single green "tests pass".
