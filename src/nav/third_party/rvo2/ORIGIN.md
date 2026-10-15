# RVO2 (ORCA)

- Source: https://github.com/snape/RVO2
- Commit: 75822bba14462c2e872ca96423825017ecd24764
- License: Apache-2.0 (`LICENSE` here, copied from upstream)
- Copyright: 2008 University of North Carolina at Chapel Hill

## Taken

`orca.c` ports these parts of `src/Agent.cc`:
- the agent half of `Agent::computeNewVelocity` (one ORCA line per neighbour);
- `linearProgram1`, `linearProgram2` and `linearProgram3`.

## Changed

- C instead of C++. Lines are a plain struct, and the arrays are fixed-size
  instead of `std::vector`.
- Agent lines are built four neighbours at a time with SIMD (`src/nav/simd4.h`).
  Both branches are computed and the answer is selected per lane, and only
  operations that round the same everywhere are used.
- Each pair's share of the avoidance is a weight passed in by the crowd (from
  the agents' priorities), instead of the fixed 0.5.
- Agents exactly on top of each other with no relative velocity part along x,
  each to its own side, instead of dividing by zero.
- There are no static obstacle lines. The crowd keeps agents on the navmesh
  instead (`src/nav/crowd.c`).
- Neighbours are found by the crowd's grid instead of RVO2's k-d tree.
