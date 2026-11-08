# How the GNS backend works

This is a reading guide for `src/online/gns_impl.h`. It explains the
moving parts in the order a packet and a connection actually encounter
them. Open `docs/gaps.md` alongside it for known limits; this file is
about current behavior, not the backlog.

## The one-sentence model

`GnsTransport` translates between two vocabularies:

- library vocabulary: `PeerId`, `Channel`, and `Event`
  (`Connected`, `Disconnected`, `Packet`);
- GNS vocabulary: listen sockets, poll groups, connection handles,
  status callbacks, and `SteamNetworkingMessage_t`.

Everything in `gns_impl.h` is state or glue for crossing that boundary.
It does not know about Hello packets, sides, characters, snapshots, or
any game rule. Those belong to the consumer.

## Files and ownership

| File | Role |
| --- | --- |
| `src/online/core.h` | The portable contract every backend implements. |
| `src/online/gns.h` | Public `GnsTransport` declaration. Safe to include without GNS headers or link flags. |
| `src/online/gns.cpp` | The one opt-in TU. Defines the standalone macro, includes the impl, and forwards each virtual call. |
| `src/online/gns_impl.h` | The actual GNS work. Included only by `gns.cpp`. |
| `src/online/local.h` | The in-process backend with the same event shape. Useful as the simple reference implementation. |

`GnsTransport` owns one `Impl` through `unique_ptr`. An `Impl` is one
endpoint in exactly one role at a time: server after `listen()`, or
client after `connect()`. Loopback tests therefore construct two
transports even when both live in one process.

## State worth knowing

Most confusion in the impl comes from three kinds of identifiers being
alive at once:

| Name | Meaning |
| --- | --- |
| `HSteamNetConnection` | GNS's native connection handle. Only GNS and callbacks know this value. |
| `HSteamListenSocket` | GNS's native listening-socket handle (server only). |
| `PeerId` | The portable ID games see. The server is `0`; accepted remote peers start at `10000`. |

The server keeps the two directions of the same map:

- `conn_to_peer`: native handle to `PeerId`, used when a message or
  callback arrives and the game-facing sender must be identified.
- `peer_to_conn`: `PeerId` to native handle, used when game code calls
  `send(peer, ...)`.

Clients need no map for their one server connection (`client_conn`),
but their *own* `local_id` starts invalid. GNS does not tell a client
which server-allocated ID it received, so the game handshake must do
that and then call `set_local_peer_id()`.

`pending` is the endpoint's portable event queue. Callbacks and the
message drain append to it; each `poll()` call returns at most one
event from the front. Callers are expected to loop until `poll()`
returns nothing.

## Process lifetime: `detail::gns_acquire` / `gns_release`

GNS initialization is process-wide, while transports are per-endpoint.
The `detail` namespace therefore keeps:

- a refcount (`gns_refcount`) so a server and client in one loopback
  test process can share one initialized GNS instance;
- the singleton interface pointer (`gns_iface`);
- two callback registries, by listen socket and by connection handle.

The registries are intentionally leaked heap maps. A transport can be
destroyed during another translation unit's static destruction at
process exit; function-local static maps could already be dead by
then. Leaking two empty-by-exit maps is the deliberate tradeoff.

Today acquisition is standalone-specific: `gns_acquire()` itself calls
`GameNetworkingSockets_Init`, `SteamNetworkingSockets_LibV12()`,
utils, and `GameNetworkingSockets_Kill()`. A future Steam TU cannot
reuse that function as-is. That bootstrap split is item 8 in
`docs/gaps.md`; the connection logic after acquisition is the shared
part.

## Why there is a global callback and registries

GNS reports connection lifecycle through one C-style status callback.
It does not call a virtual method on "the right transport" for us.
So `gns_status_callback` routes manually:

1. If the connection handle is in `gns_conn_registry`, that `Impl`
   handles the status change.
2. Otherwise, if the event carries a listen socket, the listening
   `Impl` handles it. This is how a brand-new incoming connection,
   which has no `PeerId` yet, reaches the server that must accept or
   reject it.

Every listen/connect path registers its handles before GNS can report
on them, and close/disconnect paths erase them before closing so a
late callback cannot find a dying `Impl`.

## Server flow

`listen(port)`:

1. Acquire the shared GNS instance.
2. Create a listen socket and one poll group. The poll group is how
   the server receives messages from all accepted connections through
   one call instead of polling each connection.
3. Register the listen socket, mark this endpoint as server, and set
   its local ID to `kServerPeer`.

When a remote client starts connecting, GNS invokes the global status
callback with state `Connecting`. The listen-socket registry routes it
to `Impl::on_status_changed`, which either:

- closes it with the capacity end reason when four peers are mapped;
- closes it if `AcceptConnection` or poll-group assignment fails; or
- accepts it, allocates the next `PeerId`, records both handle maps,
  and registers the connection handle for later callbacks.

Accepting is not the same as reporting `Connected`. The portable
`Connected` event is queued only when GNS later reports the
`Connected` state for that mapped connection.

## Client flow

`connect(host, port)`:

1. Acquire GNS and format `host:port` for
   `SteamNetworkingIPAddr::ParseString`. A parse failure is the only
   common synchronous `false` return.
2. Call `ConnectByIPAddress`. Success here means "attempt started",
   not "server reached."
3. Register the single client connection handle. `local_id` remains
   invalid until the game's Welcome-equivalent supplies the
   server-allocated ID.
4. On GNS `Connected`, queue portable `Connected{kServerPeer}`.
5. On closed/problem states, queue `Disconnected{kServerPeer, reason}`
   even if `Connected` never arrived, then invalidate the handle.

An unreachable loopback endpoint was measured taking about 10.0
seconds to reach the terminal state. That is GNS's initial timeout
working, not polling overhead.

## Sending

`send()` is a lookup plus one GNS call: resolve `PeerId` to a native
connection (or use the client's only connection), map `Channel` to the
exact GNS send flag, and call `SendMessageToConnection`. Nothing is
upgraded to reliable behind the caller.

`broadcast()` is not a native GNS broadcast. On a server it copies the
current peer list and sends once per peer. On a client it degrades to
sending to the server, matching what a client can actually address.

## Polling, in order

Each `poll()` does three things:

1. `RunCallbacks()`: GNS invokes the global status callback, which may
   accept a connection and/or append lifecycle events to `pending`.
2. `drain_messages()`: receive up to eight batches of sixteen native
   messages. Each payload is copied into a portable `Packet`, tagged
   with the mapped sender and a reliable/unreliable channel, and the
   native message is released immediately.
3. Pop and return one queued event, if any.

Two consequences are easy to miss:

- A lifecycle event generated by `RunCallbacks()` can be queued before
  a packet drained in the same `poll()`, even when the peer sent that
  packet before closing. Games that care about a final explanation
  packet should finish draining a poll batch before acting on a
  lifecycle event. Pumpkin Beachball does exactly that so a reliable
  `Bye` can beat the generic disconnect status.
- Received packets cannot report `UnreliableNoDelay`: GNS message
  flags on receipt expose only the reliable bit, so both unreliable
  flavors arrive labeled `Unreliable`.

## Closing, without double events

There are three different close paths:

- `disconnect(peer)` is an explicit one-peer drop. It erases callback
  routing first, closes the native connection, then synthesizes the
  local endpoint's own `Disconnected{..., LocalClose}`. Erasing first
  is what prevents GNS's callback from reporting the same drop twice.
- A remote close arrives through the status callback. It erases the
  handle from both maps and queues one `Disconnected` with a mapped
  portable reason.
- `close()` tears down the whole endpoint. It unregisters and closes
  every connection, destroys the poll group and listen socket, clears
  queued events, and releases the process-wide GNS refcount. It does
  not synthesize events for the closing endpoint.

## Stats and reasons

`connection_stats()` resolves the native handle the same way `send()`
does, calls `GetConnectionRealTimeStatus`, and copies the portable
fields. It is a query, not an event. Note that GNS clears its
max-jitter high-water mark when status is fetched.

Disconnect reasons are mapped in `detail::map_end_reason` from GNS's
numeric ranges: this backend's capacity code becomes `Rejected`,
timeouts become `Unreachable`, local ranges become `LocalProblem`,
and application/remote closes become `ClosedByPeer`. The native debug
string is still printed to stderr in the callback and is not part of
the portable event; see item 3 follow-up in `docs/gaps.md` if that
ever needs to change.

## Reading order for a change

1. `gns.h` for the contract games see.
2. `Impl::listen` and `Impl::connect` for handle creation.
3. `Impl::poll` and `drain_messages` for inbound data.
4. `Impl::on_status_changed` for lifecycle translation.
5. `close` and `disconnect` last; most ordering bugs live in what gets
   unregistered before a native close call.

