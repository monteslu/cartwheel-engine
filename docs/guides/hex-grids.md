# Hex grids

Sample: `hex_board`. Gates: `unit-hex`, `hex-board-replay`.

Boards made of hexagons (a tile-laying game, a hive of pieces, a strategy map) need the same small set of answers again and again: which hexes touch this one, how far apart two are, what lies on a ring or a line, where a hex is drawn and which hex a click or a 3D pick landed on. [cartwheel/hex.h](header:hex.h) gives them as plain functions over integer coordinates, plus a map that keeps a value per hex. Nothing here is a node or draws anything; the 2D tile maps keep their own hex layout for paths ([Navigation](guide:navigation)).

## Coordinates

A hex is a [cw_axial](api:cw_axial): axial coordinates `q` and `r`. The cube form's third coordinate is `s = -q - r` ([cw_hex_s](api:cw_hex_s)), so every rule written for cube coordinates works: the distance between two hexes is `(|dq| + |dr| + |ds|) / 2` ([cw_hex_distance](api:cw_hex_distance)).

The six directions are numbered 0 to 5 from `+q`, counterclockwise as drawn with y down: `(+1, 0)`, `(+1, -1)`, `(0, -1)`, `(-1, 0)`, `(-1, +1)`, `(0, +1)`. With pointy hexes they are east, north-east, north-west, west, south-west and south-east; with flat hexes south-east, north-east, north, north-west, south-west and south. Any int is taken modulo 6, so `dir + 1` is always the next direction counterclockwise and `dir + 3` the opposite one.

| Call | Answer |
|---|---|
| [cw_hex_neighbor](api:cw_hex_neighbor), [cw_hex_direction](api:cw_hex_direction) | the hex one step that way, the step itself |
| [cw_hex_diagonal_neighbor](api:cw_hex_diagonal_neighbor) | the hex two steps away between two directions |
| [cw_hex_distance](api:cw_hex_distance), [cw_hex_length](api:cw_hex_length) | steps between two hexes, steps from (0, 0) |
| [cw_hex_direction_to](api:cw_hex_direction_to) | the direction pointing closest to another hex |
| [cw_hex_rotate](api:cw_hex_rotate) | turned about a centre by 60 degrees a step |
| [cw_hex_reflect](api:cw_hex_reflect) | mirrored across the q, r or s line through a centre |
| [cw_hex_round](api:cw_hex_round), [cw_hex_lerp](api:cw_hex_lerp) | the hex a fractional position is in, a position between two hexes |

Rotation and reflection are what a tile-laying game turns its pieces with: a piece is a list of hexes, and turning it is `cw_hex_rotate` on each about the piece's anchor.

## Shapes

Each shape call writes up to `cap` hexes and returns how many the whole shape has, so `out` can be NULL to ask the size first.

- [cw_hex_line](api:cw_hex_line): the hexes from `a` to `b`, both ends in, each next to the last (line of sight, a sliding piece).
- [cw_hex_ring](api:cw_hex_ring): the `6 * radius` hexes at exactly that distance, in order round the ring.
- [cw_hex_spiral](api:cw_hex_spiral): every hex within a radius, the centre first and then ring by ring; a hexagonal board of radius `r` is `cw_hex_spiral(center, r, ...)` with [cw_hex_area_count](api:cw_hex_area_count)`(r)` hexes.
- [cw_hex_reachable](api:cw_hex_reachable): every hex reachable in at most `steps` moves through hexes a callback allows, nearest first. It is a piece's moves round obstacles, or (with a large `steps` and a callback that allows one player's hexes) the connected group a hex belongs to, which is how `hex_board` scores:

@include samples/hex_board/main.c#score

## Offset coordinates

A rectangular board is often stored as columns and rows. [cw_hex_to_offset](api:cw_hex_to_offset) and [cw_hex_from_offset](api:cw_hex_from_offset) convert for the four usual conventions: `CW_HEX_ODD_R` and `CW_HEX_EVEN_R` (pointy hexes, odd or even rows pushed half a hex right) and `CW_HEX_ODD_Q` and `CW_HEX_EVEN_Q` (flat hexes, columns pushed down). They are also a pad cursor's friend: up and down by offset row keep a cursor in its column on a pointy board, which axial directions alone do not (`hex_board` moves its cursor this way).

## Layouts: 2D and 3D

A [cw_hex_layout](api:cw_hex_layout) places hexes in a plane. `orientation` is `CW_HEX_POINTY` (a corner at the top) or `CW_HEX_FLAT` (an edge at the top), `size` is the distance from a centre to a corner along x and along y (equal for regular hexagons, different for a board seen at a slant), and `origin` is where hex (0, 0)'s centre goes.

```c
cw_hex_layout lay = { CW_HEX_POINTY, { 40, 40 }, { 480, 290 } };
cw_vec2 at = cw_hex_to_point(&lay, h);             /* the hex's centre on the canvas */
cw_vec2 k[6];
cw_hex_corners(&lay, h, k);                       /* its outline: cw_r2d_polygon(k, 6, colour) */
cw_axial under = cw_hex_from_point(&lay, mouse);   /* the hex under a point */
```

In 3D the same layout lies on the ground: layout x is world X and layout y is world Z, at the height the call gives. [cw_hex_to_world](api:cw_hex_to_world) is where a piece stands, [cw_hex_world_corner](api:cw_hex_world_corner) the corners of a tile mesh, and [cw_hex_from_world](api:cw_hex_from_world) turns a point on the board (where a pick ray hit it) into the hex. A camera looking down `-Z` sees the board as the 2D layout draws it.

Corner `i` is at `60 i` degrees from +x toward +y (pointy: `60 i + 30`). The point conversions use only `+ - * /` and rounding, so a position and the hex found for it are the same on every host.

## The hex map

[cw_hex_map](api:cw_hex_map) keeps a value of fixed size per hex, for a board of any shape that may grow (tiles laid next to tiles) or be sparse (pieces on an open table):

```c
cw_hex_map *board = cw_hex_map_new(sizeof(int8_t));    /* owner per placed hex */
int8_t p = 2;
cw_hex_map_set(board, h, &p);
int8_t *o = CW_HEX_MAP_GET(board, h, int8_t);           /* NULL where nothing is */
cw_hex_map_remove(board, h);
```

Value size 0 makes a set of hexes. [cw_hex_map_keys](api:cw_hex_map_keys) lists the hexes sorted by r, then q, whatever order they went in, and [cw_hex_map_hash](api:cw_hex_map_hash) folds keys and values in that order: a board's state hash for a test or a replay check that agrees on every host. Pointers from `get` and `set` stay good until a new hex is added, one is removed, or the map is cleared or freed.

## Checks

`unit-hex` checks every call against an independent answer: distances against a breadth-first walk, the hex under a point against the nearest centre found by brute force (both orientations, squashed sizes, off origin), lines for adjacency and shortest paths, rings against a filter of a box, reach through a wall with one gap, all four offset conventions both ways, and the map through growth, removal, sorted keys and the contents hash. Its control (`CW_TEST_HEX_NAIVE=1`) runs the point and distance checks through the two usual mistakes, rounding q and r on their own and distance as the larger of `|dq|` and `|dr|`, which must fail.
