# Networking

`cartwheel/net.h` moves datagrams between peers, turns them into players with reliable and unreliable channels, replicates nodes, snapshots whole scene trees and runs rollback netcode on the deterministic fixed step. The wire format is the engine's and the same on every host, so a native build and a cart speak the same protocol.

For a step-by-step build of a rollback game, follow the [multiplayer tutorial](tutorial:multiplayer-rollback). This page explains the layers, what each costs and where the edges are.

## The layers

| Layer | What it gives you | Main API |
|---|---|---|
| transport | datagrams between *links* (one link per remote endpoint) | [cw_net_transport](api:cw_net_transport) |
| session | players by *slot* (0 to 7), a handshake, reliable-ordered and unreliable channels, acks, resends, RTT, timeouts | [cw_net_session_new](api:cw_net_session_new) |
| scene | authority per node, RPC, the `NetSynchronizer` and `NetSpawner` node types | [cw_net_attach](api:cw_net_attach) |
| state | a snapshot of a whole tree through reflection, restorable in place, with a host-stable hash | [cw_state_save](api:cw_state_save) |
| rollback | input delay, prediction, restore and re-simulate, time sync, desync detection | [cw_rollback_new](api:cw_rollback_new) |

Each layer only uses the one below it. A game that wants plain messages stops at the session; a turn-based game may use RPC and replication; an action game with tight timing uses rollback on top of a session. Everything runs on the main thread, and every time value is whatever clock the caller passes to [cw_net_poll](api:cw_net_poll). A test passes a virtual clock, which is what makes a simulated network reproducible bit for bit.

The full reference is [net.h](header:net.h).

## Transports

A transport is a table of five functions (`open`, `send`, `poll`, `close`, `destroy`). `open` dials an address and returns a link at once; a `CONNECT` event follows when the link is usable, and endpoints that reach you first appear as a `CONNECT` on a new link. You can bring your own by filling the table. The engine ships these:

- **UDP** ([cw_net_udp_new](api:cw_net_udp_new)): native builds only, IPv4, POSIX sockets and Winsock. Bind a port (0 picks a free one) and open `"host:port"`. A datagram from an unknown address opens a new link. Returns NULL in a cart and on bind failure.
- **wasmcart peer connection** ([cw_net_wcpeer_new](api:cw_net_wcpeer_new)): carts only, on the host's `wc_peer_*` imports. The host interprets the address (`ws://...` on the reference hosts). The app must set `.net = true` in its `cw_app_desc`, and the cart's manifest must grant the domain: in a CMake build that is `cw_add_app(name ... NET localhost)`. Without the grant `open` is refused and the log says why. One per cart, because the peer exports are global.
- **Loopback** ([cw_net_loopback_new](api:cw_net_loopback_new)): an in-process hub that joins endpoints by name (`"loop:a"`). Delivery is immediate and in order. Use it for tests, local split screen, and to run both peers of a match in one process.
- **Room relay** ([cw_net_relay_new](api:cw_net_relay_new)): a client for `tools/cwrelay.mjs`, layered on any base transport. Every URL path on the relay is a room; the relay assigns each member the lowest free slot and announces the others, each of which appears as a link whose id is its slot. It forwards datagrams as-is and never parses game traffic.
- **Network simulator** ([cw_net_sim_new](api:cw_net_sim_new)): wraps any transport and drops, duplicates, delays and jitters outgoing datagrams from a seeded random stream. Jitter reorders. Because time is whatever you pass to `poll`, the same calls give the same network on every host. [cw_net_sim_set](api:cw_net_sim_set) changes the conditions mid-run.

The relay is a dependency-free Node script:

```sh
node tools/cwrelay.mjs --port 47731 --verbose
```

## Sessions

A session ([cw_net_session_new](api:cw_net_session_new)) owns its transport. Each peer announces its slot and the game's `protocol` id (put a version in it) in a handshake; a peer with another protocol, or a slot that is already taken, is refused and the refusal is logged. Pass `local_player = -1` to take the slot a relay assigns.

Messages carry a type byte: 0 to 31 are the engine's, `CW_NET_MSG_GAME` (32) to 255 are yours. Register one handler per type with [cw_net_on_message](api:cw_net_on_message) and send with [cw_net_send](api:cw_net_send) to one slot or `CW_NET_ALL`.

- `CW_NET_RELIABLE` messages arrive exactly once and in order.
- `CW_NET_UNRELIABLE` messages may be lost or reordered: sequence them yourself if order matters.

Messages are queued and leave in packets on [cw_net_flush](api:cw_net_flush) or at the end of `cw_net_poll`. Acks ride on every packet and a keepalive goes out every 0.1 s, so RTT and loss are always measured: [cw_net_get_peer_stats](api:cw_net_get_peer_stats) reports smoothed RTT, packets lost, resends and bytes. A peer that sends nothing for `timeout` seconds (5 by default) is dropped, and [cw_net_on_peer](api:cw_net_on_peer) listeners hear about joins and leaves.

A datagram is at most `CW_NET_MAX_PACKET` (1200) bytes, which fits every MTU, and one message payload at most `CW_NET_MAX_MESSAGE` (1100). There is no fragmentation: a bigger message is refused with an error that says to split it.

Sessions do not forward traffic between peers. With UDP, every peer connects to every other peer it plays with; through a relay, every member of a room already has a link to every other member.

## Authority, RPC and replication

[cw_net_attach](api:cw_net_attach) ties a session to a scene tree. Node paths on the wire are relative to the attach root, so both peers must build the same tree under it. Attaching also registers the network node types; a game that never attaches links none of that code, and a game that loads scenes containing them before attaching calls [cw_net_register_types](api:cw_net_register_types) first.

**Authority** says which player simulates a node and may change its replicated state. It is inherited from the parent, and the session root defaults to player 0. [cw_net_set_authority](api:cw_net_set_authority) is local: every peer makes the same call, except for children a `NetSpawner` creates, which carry their authority with the spawn.

**RPC**: a node method registered with `cw_node_type_add_method` becomes callable from other peers once [cw_net_rpc_config](api:cw_net_rpc_config) names it. `CW_RPC_AUTHORITY` methods are accepted only from the node's authority (others are refused and logged); `CW_RPC_ANY_PEER` methods from anyone. [cw_net_rpc](api:cw_net_rpc) calls it on every connected peer (and locally when `call_local` is set), [cw_net_rpc_to](api:cw_net_rpc_to) on one. Inside the method, [cw_net_rpc_sender](api:cw_net_rpc_sender) is the caller's slot. Arguments are reflected values; the whole call must fit in one message.

**[NetSynchronizer](node:NetSynchronizer)** replicates reflected properties of the node at `root_path` (its parent by default). The authority sends the comma-separated `properties` every `interval` seconds (0 means every poll), on the unreliable channel unless `reliable` is set; the other peers apply only updates newer than the last one they applied, and only from that node's authority.

**[NetSpawner](node:NetSpawner)** replicates creation and removal of the children of `spawn_path`. When its authority adds a child instanced from a listed `*.cwscene` (or of a listed node type), every peer creates the same child with the same name and the `spawn_properties`; freeing it frees it everywhere. Peers that join later receive every live child.

This is the same model as Godot's MultiplayerSynchronizer and MultiplayerSpawner. No sample uses it yet; [tests/unit/test_net.c](repo:tests/unit/test_net.c) builds a host and a client over a lossy loopback, spawns scenes with a synchronizer inside, hands one of them to the client, calls an any-peer RPC, checks that an authority-only one is refused, and despawns a node everywhere.

## Tree snapshots

A [cw_state](api:cw_state) is the whole tree through reflection: every node's type, name, parent and child index, every writable reflected property, the tree's tick and its random state. [cw_state_load](api:cw_state_load) frees nodes the snapshot does not have, recreates the ones it has, and writes a property only when it differs, so restoring an unchanged tree costs reads only. Node identity is the node uid, and node-valued properties are remapped.

[cw_state_hash](api:cw_state_hash) covers types, structure and values (not uids, names or pointers), so it is equal on every host for equal state. Leave derived or cosmetic properties out of snapshots and hashes with [cw_state_skip_prop](api:cw_state_skip_prop), and save state a type keeps outside its reflected properties with [cw_state_type_hook](api:cw_state_type_hook). The physics nodes use that hook: a `CharacterBody2D` or `CharacterBody3D` carries its mover state and its floor outputs in the snapshot, and every body is moved back to its node's pose after a restore. A snapshot also carries the tree's 2D physics world as Box2D's own image of it ([cw_world2d_snapshot](api:cw_world2d_snapshot)), and the hash folds in Box2D's host-stable state hash of that world instead of the image's bytes. The image holds pointers, so a snapshot stays in the process that made it.

When two peers disagree, [cw_state_dump](api:cw_state_dump) writes a snapshot as text, one line per node and per value, so you can diff the two.

## Rollback

[cw_rollback_new](api:cw_rollback_new) drives a tree's fixed step from inputs. Each player's input for a tick is `input_size` opaque bytes (at most 32) that the game packs: buttons, quantized axes. The typical frame:

1. The game calls [cw_rollback_advance](api:cw_rollback_advance) once per platform tick with the current time.
2. It polls the session. Received remote inputs are compared with the predictions already used; if one differs, the tree is restored to the snapshot before that tick and re-simulated up to now.
3. It asks `sample_input` for the local input of tick `T + input_delay` and sends it to every peer, redundantly, on the unreliable channel.
4. It simulates one new tick, predicting each missing remote input as that player's last received one. It stalls instead when it would run more than `max_prediction` ticks past the last fully confirmed tick, and skips a tick when it is more than one tick ahead of the remote (time sync).
5. Every confirmed state is hashed, and every `hash_interval` ticks the hashes are exchanged and compared in-band, so a desync is detected on both sides.

Inside the simulation (a node's `fixed_update`), read inputs only through [cw_rollback_input](api:cw_rollback_input), never from devices, and draw every random number from the tree's `cw_tree_rng`. The rest of the [determinism rules](guide:conventions) apply too: `cw_sin` and friends instead of libm, no clocks. Effects that should not repeat during a re-simulation (sounds, particles) check [cw_rollback_resimulating](api:cw_rollback_resimulating).

The `net_arena` sample's player shows the input side: two bytes per tick, movement bits and an aim angle.

@include samples/net_arena/main.c#player

Its peers are a tree, a session and a rollback each:

@include samples/net_arena/main.c#peer

[cw_rollback_get_stats](api:cw_rollback_get_stats) reports rollbacks, re-simulated ticks, the deepest rollback, stalls, mispredictions, desyncs (and the first desynced tick) and the wall time spent saving and re-simulating. [cw_rollback_saved_state](api:cw_rollback_saved_state) returns the snapshot of a recent tick for `cw_state_dump`. Pass `session = NULL` for local multiplayer: every player is local and `sample_input` is asked for each.

### Cost

Rollback costs one snapshot per tick and, after a misprediction, a restore plus re-simulation of up to `max_prediction` ticks inside one frame. Both scale with the size of the rolled-back tree, so keep it to the simulation and leave cosmetics outside it. In `net_arena` (80 ms latency, 40 ms jitter, 10% loss) the peers roll back about once every four ticks, at most 8 ticks deep, and the snapshot and rollback each take a few microseconds per frame on a desktop. `CW_NET_BALLAST=N` adds N passive nodes to the simulated tree to see how the cost grows; `tests/net.mjs --only perf` prints the numbers.

### What re-simulates exactly

Character bodies (`move_and_slide`) against static and kinematic bodies, and the engine's own math, re-simulate bit-exactly in 2D and 3D.

In 2D everything else does too: the snapshot restores the Box2D world itself, so dynamic `RigidBody2D` piles come back with their velocities, contact warm-start caches and sleeping islands, and `Area2D` overlap signals with their sensor state. `unit-net` rolls a pile back 44 times with 0 desyncs, and the same run without the world image is the control that must desync.

In 3D two things do not, because Box3D has no snapshot and keeps their state where no restore reaches: **dynamic rigid bodies** (contact warm-start caches make a restored pile diverge) and **Area3D overlap signals** (begin and end events come from the library's sensor state). A 3D rollback game keeps simulated objects out of dynamic rigid bodies and detects overlaps with queries or its own math; cosmetic debris outside the rolled-back tree is fine. See [Physics](guide:physics).

## The net_arena sample

[samples/net_arena](repo:samples/net_arena/main.c) is a two-player arena: players, bullets spawned and freed as they fly, scores and random respawns, all driven only by rollback inputs. Bots play both sides until a pad or the keyboard moves. It runs in three modes.

**pair** (the default, every host) runs both peers in one process over the loopback hub, each endpoint wrapped in the simulator. The left half of the screen draws peer A's world, the right half peer B's, and the bottom line compares the confirmed hash at tick 600:

@include samples/net_arena/main.c#pair

Both peers advance from the same virtual clock, so the whole match, lost packets included, is identical on every host:

@include samples/net_arena/main.c#advance

```sh
node tools/run-cart.mjs --host native --app net_arena --frames 800 --state out/net.json --shot out/net.png
```

At frame 800 both peers report the same hash at tick 600 with 0 desyncs, after roughly 200 rollbacks each and 10% of about 1600 datagrams dropped.

**udp** (native) runs one peer per process. `CW_NET_UDP` is the port to bind, `CW_NET_PEER` the other side, `CW_NET_PLAYER` the slot; the simulator still adds latency and loss on top, on the real clock:

```sh
CW_NET_UDP=40001 CW_NET_PLAYER=0 build/native/samples/net_arena &
CW_NET_UDP=40002 CW_NET_PLAYER=1 CW_NET_PEER=127.0.0.1:40001 build/native/samples/net_arena
```

**peer** (carts) joins a relay room through the wasmcart peer connection: the debug command `net_join <ws url>`, or the `net_arena_peer` build, which joins `ws://localhost:47731/arena` at start. Start `tools/cwrelay.mjs` first.

Native environment knobs: `CW_NET_LATENCY_MS` (80), `CW_NET_JITTER_MS` (40), `CW_NET_LOSS` (0.1). Controls that must fail: `CW_NET_NO_ROLLBACK=1` (wrong predictions stay) and `CW_NET_DESYNC=1` (one peer miscounts a score once at tick 300). Debug fields: `net.a.hash` and `net.b.hash`, `net.match`, `net.a.*` and `net.b.*` rollback stats, `net.sim.*`, and `arena.*` for the world. `net.a.trace` holds the confirmed hash at 16 ticks, for bisecting where two runs part.

## How it is tested

- `unit-net` ([tests/unit/test_net.c](repo:tests/unit/test_net.c)): exactly-once in-order delivery over a lossy simulated network, a refused protocol, snapshots (structure, values, private state, a host-stable hash), RPC and replication, and rollback with a no-rollback control that must desync.
- `net-arena`: the pair mode on every host. Fields must match native exactly; the control disables rollback and must diverge.
- `net` ([tests/net.mjs](repo:tests/net.mjs)): native to native over UDP in two processes, and cart to cart through `tools/cwrelay.mjs` on romdev, wasmcart-native and two carts in one Chromium page. The controls (an injected desync on every transport, rollback disabled over UDP) must be caught.

```sh
node tests/gate.mjs net-arena
node tests/net.mjs --only pair,udp
```

See [Testing](guide:testing) for how gates and controls work.

## Known gaps

- **Native to cart**: a native build has UDP but no WebSocket client, and a cart has only the peer connection, so they do not reach each other yet. Native to native and cart to cart both work.
- **No fragmentation**: one message is at most 1100 bytes. Large state (a level, a join-in-progress snapshot) is split by the game.
- **Rollback and 3D physics**: 3D dynamic rigid bodies and Area3D signals do not re-simulate exactly (see above); 2D ones do.
- **IPv4 only** on the UDP transport; no NAT traversal, encryption or authentication. Use a relay to reach peers behind NAT.
- **No replicated authority changes**: `cw_net_set_authority` after a spawn is local to the peer that calls it.
- **No sample for RPC and replication** beyond the unit suite.
