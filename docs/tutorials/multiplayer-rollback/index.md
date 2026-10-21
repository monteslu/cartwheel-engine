# Multiplayer with rollback

You build Rollback Duel: two racers chase a coin, each player on their own
peer, kept in sync by rollback netcode. Both peers run in one process over
a simulated bad network (80 ms latency, jitter, 10% loss), so you can watch
them agree, test them on every host, and break them on purpose. The same
code then runs as two processes over UDP.

What you learn: the layers of [net.h](header:net.h) (transports, sessions,
rollback), input delay versus rollback, what has to be deterministic, and
how to prove two peers converge: their state hashes are equal.

The finished project is `docs/tutorials/multiplayer-rollback/project/`.

## Start from the template

Copy the empty 2D template and name the game:

```sh
cp -r editor/templates/empty_2d duel
node tools/cwproject.mjs build duel all
```

Edit `project.cwproj`: the name, `app` (the executable and cart name), the
second scene, and four move actions (keyboard, d-pad and left stick):

@include docs/tutorials/multiplayer-rollback/project/project.cwproj

Add `world.cwscene` to `roots.txt` too, so the pack ships it:

@include docs/tutorials/multiplayer-rollback/project/roots.txt

`CMakeLists.txt` and `.gitignore` stay as the template made them.

## The world is a scene

Everything the network keeps in sync lives in one scene, `world.cwscene`:
the floor, the coin and two racers. Every peer instances its own copy into a
private tree. The main scene (`main.cwscene`) only holds the HUD labels; it
is not simulated by the netcode.

@include docs/tutorials/multiplayer-rollback/project/world.cwscene

`Racer` is a node type of the game. It extends `Polygon2D`, so it draws as
one, and adds the state the game needs:

@include docs/tutorials/multiplayer-rollback/project/src/main.c#racer

Three things in this code are what rollback depends on:

- **All game state is reflected.** `player`, `score` and `vel` are
  properties (`CW_PROP_FIELD`). Rollback restores a tree from a snapshot of
  its reflected properties (`cw_state_save`); a `static` counter or a field
  left out of `racer_props` would not roll back, and the peers would drift.
- **Input comes from `cw_rollback_input`**, the input of the tick being
  simulated. The same tick may run several times (a re-simulation after a
  late input), so the simulation never reads a device, a clock or anything
  else that changes between runs.
- **Randomness comes from the tree**: `cw_tree_rng` is part of the snapshot,
  so a re-simulated coin lands where the first run put it. The two
  `cw_rng_range` calls are separate statements on purpose: C does not fix
  the order in which function arguments are evaluated, and x86 and wasm
  compilers really do pick different orders.

The full list is in [the conventions](repo:docs/CONVENTIONS.md) (Determinism
rules): randomness from `cw_rng` only, time from the fixed `dt` only, trig
and friends from `cartwheel/math.h` (`cw_sin`, `cw_atan2`) because libm
differs between platforms, iteration in insertion order, no `-ffast-math`.
Physics is covered at the top of [net.h](header:net.h): a snapshot carries
the 2D physics world whole, so 2D rigid bodies and Area2D overlaps
re-simulate exactly. 3D has one limit: dynamic rigid bodies and Area3D
overlap signals do not, so a 3D rollback game moves characters with
`move_and_slide` or its own math. This game uses its own math.

## A peer: a tree, a session and a rollback

@include docs/tutorials/multiplayer-rollback/project/src/main.c#peer_state

@include docs/tutorials/multiplayer-rollback/project/src/main.c#world

Both peers seed their tree the same way. Then each gets a session (players
by slot, a handshake, channels) and a rollback driver on top of it:

@include docs/tutorials/multiplayer-rollback/project/src/main.c#make_peer

The session takes ownership of the transport. `protocol` is the game's id
and version: a peer that announces another one is refused, so an old build
never plays a new one.

`cw_rollback_new` is where the netcode is tuned:

- **`input_delay`** (2 ticks): the local input sampled now is scheduled for
  2 ticks later. If the other peer's input for a tick arrives within those
  2 ticks (about 33 ms), nothing was ever predicted and nothing rolls back.
  More delay hides more latency but makes your own controls feel late.
- **Rollback** covers the rest. A remote input that has not arrived is
  predicted (the player's last input repeats). When the real input arrives
  and differs, the tree is restored to the snapshot before that tick and
  re-simulated up to now, inside one frame. You never see the wrong
  prediction for longer than the latency.
- **`max_prediction`** (10 ticks): the farthest the simulation runs ahead of
  confirmed input. Past it the peer stalls (waits) instead of predicting
  further; `cw_rollback_get_stats` counts stalls.
- **`hash_interval`** (10 ticks): every 10th confirmed state is hashed and
  the hashes are exchanged in-band. A difference is a desync, logged with
  the tick (`DESYNC with player 1 at tick 310`) and counted.
- **`no_rollback`** is a test switch: wrong predictions are never
  corrected. It is how the gate below proves that rollback is doing work.

Each tick the driver asks for the local player's input. This runs outside
the simulation, so reading devices here is right:

@include docs/tutorials/multiplayer-rollback/project/src/main.c#input

The input is one byte per player per tick (`input_size = 1`); pack buttons
as bits and quantize analog axes. The buffer arrives zeroed. When nobody
holds a direction a small bot plays, so both racers move in a test run.

## Transports

A transport moves datagrams between links. The session and the rollback
code do not care which one they get:

- **Loopback** (`cw_net_loopback_new`, every host): a hub inside the
  process, endpoints joined by name (`"loop:a"`). Immediate and in order.
  This is how tests and local split-screen run two peers in one process.
- **UDP** (`cw_net_udp_new`, native builds): one socket, `"host:port"`
  addresses, one link per remote address. `NULL` in a cart.
- **Peer connections** (`cw_net_wcpeer_new`, carts): the wasmcart host's
  `wc_peer_*` connection, a WebSocket on the reference hosts. Carts cannot
  open sockets, so they meet through a room relay:
  `cw_net_relay_new(cw_net_wcpeer_new())` dials `tools/cwrelay.mjs`, which
  assigns each member a slot (`local_player = -1` takes it). The app sets
  `cw_app_desc.net`, and the cart's manifest must grant the relay's domain
  (`cw_add_app(... NET <domain>)`). `samples/net_arena` (`net_arena_peer`)
  and `tests/net.mjs` run carts this way on romdev, wasmcart-native and
  Chromium.
- **The simulator** (`cw_net_sim_new`) wraps any of them and adds latency,
  jitter (which reorders), loss and duplicates:

@include docs/tutorials/multiplayer-rollback/project/src/main.c#simulator

The simulator draws from a seeded generator and uses the time you pass to
the session, so the same run makes the same network, bit for bit, on every
host. That is what makes netcode testable.

`init` picks the transport. By default both peers live in this process;
with `DUEL_UDP` set, this process is one peer and dials the other:

@include docs/tutorials/multiplayer-rollback/project/src/main.c#transports

## The tick

@include docs/tutorials/multiplayer-rollback/project/src/main.c#tick

`cw_rollback_advance` is the whole loop: poll the session at `now`, fix
mispredictions (restore and re-simulate), then simulate one new tick unless
the peer must stall. It returns how many new ticks ran (0 or 1). The pair
runs on a virtual clock, ticks times `dt`, so the result does not depend on
how fast the machine is. Over UDP the clock is real (`cw_time`).

The worlds are private trees, so the game draws them itself: each into a
render target through its own camera, then both onto the screen.

@include docs/tutorials/multiplayer-rollback/project/src/main.c#draw

## Prove the peers converge

Equal state is equal hashes. `cw_rollback_state_hash` gives the hash of the
confirmed state after a tick (every input before it known), and the stats
say how much rollback happened:

@include docs/tutorials/multiplayer-rollback/project/src/main.c#sync_hash

The game publishes them as named debug values, so tests read state by name
instead of from pixels:

@include docs/tutorials/multiplayer-rollback/project/src/main.c#debug

Build and run it for 720 frames with a scripted pad (`input.json` steers
player 0 for the first 200 frames, then the bots take over):

```sh
node tools/cwproject.mjs build duel all
node tools/run-cart.mjs --host native --app rollback_duel \
  --exe duel/.cwbuild/native/project/rollback_duel \
  --cart duel/.cwbuild/cart/carts/rollback_duel.wasc \
  --frames 720 --input docs/tutorials/multiplayer-rollback/input.json \
  --state out/duel.json --shot out/duel.png
```

`out/duel.json` holds, among the engine's own values:

```text
"duel.hash_a":4184498402,"duel.hash_b":4184498402,"duel.match":1,
"duel.desyncs":0,"duel.checked":140,"duel.rollbacks":159,"duel.resim":749,
"duel.mispredictions":234,"duel.dropped":153,"duel.score":[9,14]
```

Both peers confirm the same state at tick 600 under loss and jitter, all
140 in-band hash checks agree, and that took 159 rollbacks (749 ticks
re-simulated). The HUD says `MATCH`. Run the same command with
`--host romdev`: the cart in the romdev host
reports the same values and the same picture, because nothing in the
simulation depends on the machine.

Now break it. Turn rollback off and the wrong predictions stay:

```sh
DUEL_NO_ROLLBACK=1 node tools/run-cart.mjs --host native ...   # same arguments
```

```text
"duel.hash_a":1647751129,"duel.hash_b":1122807664,"duel.match":0,"duel.desyncs":138
```

The hashes differ, and the in-band check catches it within 10 ticks. A
desync from a bug looks the same. `DUEL_DESYNC=1` makes peer B miscount one
score at tick 300, and both peers log the first check that disagrees:

```text
[error] rollback: DESYNC with player 1 at tick 310 (ours 7029409e, theirs 18186265)
[error] rollback: DESYNC with player 0 at tick 310 (ours 18186265, theirs 7029409e)
```

To find the difference, keep the snapshots near the reported tick
(`cw_rollback_saved_state`) and write each peer's with `cw_state_dump`: one
line per node and value, ready for `diff`.

The tutorial gate (`node tests/tutorials.mjs multiplayer-rollback`) runs
exactly this: the script on native and romdev, every `duel.*` value equal
on both, `duel.match` 1 with 0 desyncs and at least one rollback, and the
`DUEL_NO_ROLLBACK=1` control, which must fail.

## Two processes over UDP

The same binary plays one peer per process. Start two (one terminal each,
or both in the background):

```sh
E=duel/.cwbuild/native/project/rollback_duel
DUEL_UDP=47810 DUEL_PEER=127.0.0.1:47811 DUEL_PLAYER=0 CW_DEBUG_DUMP=out/p0.json $E &
DUEL_UDP=47811 DUEL_PEER=127.0.0.1:47810 DUEL_PLAYER=1 CW_DEBUG_DUMP=out/p1.json $E &
wait
```

Each process plays its own racer (keyboard or pad, or the bot when idle),
runs on the real clock with the simulator's latency and loss on top of the
real network, and quits a second after tick 600 is confirmed and compared.
Both dumps hold the same `duel.hash_a` and `"duel.desyncs":0`. With
`DUEL_DESYNC=1` on both, the player 1 process miscounts and both report the
desync at tick 310. `tests/net.mjs` runs `samples/net_arena` the same way,
including rollback disabled under real latency.

## Where to go next

- [net.h](header:net.h) also has RPC, `NetSynchronizer` and `NetSpawner`
  for games that replicate state from an authority instead of rolling back.
- [The networking guide](guide:networking) covers sessions and channels.
- `samples/net_arena` is a bigger rollback game: bullets spawned and freed
  inside the rolled-back tree, respawns from the tree's random numbers, and
  carts meeting through the relay.
