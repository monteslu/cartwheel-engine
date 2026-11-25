# Game-tree search

Gate: `unit-search`.

A computer player for a turn-based game (a board game, a card game, a puzzle) looks ahead: it tries moves, the replies to them, the replies to those, and plays the move whose future looks best. [cartwheel/search.h](header:search.h) does that over a few callbacks the game writes once. The same rules then run the match, the CPU's thinking, the tests and the replay checker. Nothing here is a node or draws anything.

## The game

A [cw_search_game](api:cw_search_game) describes the rules to the search:

| Callback | Answer |
|---|---|
| `moves` | the legal moves of the player to move, in a fixed order; never none while the game goes on (a player with nothing to do gets a pass move) |
| `apply` | plays a move in place, on the search's own copy of the state |
| `turn` | who decides next, `0` to `players - 1`, or `CW_SEARCH_CHANCE` when a die roll or a draw is due |
| `terminal` | the game is over |
| `evaluate` | every player's value of the state, each from that player's own view, larger better |
| `hash` (optional) | the transposition table's key; by default the state's bytes are hashed |
| `outcomes`, `chance` (optional) | the results of a pending random event: listed with integer weights, or resolved from the search's random generator |
| `undo`, `copy`, `priority`, `playout` (optional) | take a move back instead of copying, a deep copy, move ordering, a playout policy for MCTS |

A state is a block of `state_size` bytes. Keep it flat: no pointers, padding zeroed with `memset` before it is filled, and the game's own random generator inside it, so copies and hashes are exact. A move is a 64-bit code the game packs (a piece, where from, where to, which gems).

A turn made of several decisions (take three gems, then give one back) is one decision per move, and the same player moves several times in a row. The search asks `turn` at every node and never assumes turns alternate.

At a terminal state `evaluate` gives the result. Put wins and losses in bands no heuristic reaches ([CW_SEARCH_WIN](api:CW_SEARCH_WIN), [CW_SEARCH_LOSS](api:CW_SEARCH_LOSS)) and subtract the number of moves played from a win, so a faster win scores higher and a loss is put off; the search does not adjust values by depth itself.

## Chance

When the next thing to happen is random (dice, a card from a hidden deck, a market refill), `turn` answers `CW_SEARCH_CHANCE`. Never decide a hidden draw early (shuffle the deck in the state and peek): the search would read the future. `outcomes` lists what can happen as move codes with integer weights (a die: six outcomes of weight 1; a bag of three red and two blue: two outcomes of weight 3 and 2), and the search plays one with the ordinary `apply`. `chance` instead resolves the event itself from the search's random generator.

Minimax averages over every listed outcome by weight (expectimax), or samples one through `chance` when no list is given. MCTS samples, every iteration anew. A chance node is never a player's decision: it takes no part in mistakes, move ordering or the root ranking, and [cw_search_start](api:cw_search_start) refuses a state where a roll is due (resolve it in the match first).

## The two searches

**Minimax** (`CW_SEARCH_MINIMAX`, a [cw_search_kind](api:cw_search_kind)) looks at every line to a depth, cutting lines that cannot matter (alpha-beta), one depth after another (iterative deepening) so that an answer is ready whenever the budget ends. A transposition table remembers positions reached by different move orders, and its best move is tried first; the game's `priority` orders the rest. With two players it plays for `score[me] - score[other]`. With three or four, every player picks what is best for itself (max-n) by default; `paranoid` assumes everyone plays against the searching player instead, which prunes far more and plays more cautiously. Use it for games with few random events and a good `evaluate`: placement and movement games, abstract strategy.

**MCTS** (`CW_SEARCH_MCTS`), Monte Carlo tree search, plays many quick games to the end (or `playout_depth` moves, then `evaluate`) from the current state, and grows a tree toward the moves that win most often (UCT). It needs no heuristic beyond who is ahead, handles chance and more players naturally, and is the better start for card drafting, dice and engine-building games whose positions are hard to score. What a playout is worth to a player is its margin by default (see [The MCTS reward](#the-mcts-reward)), so a win is 1, a loss 0, and a player behind still plays to close the gap.

**Greedy** (`CW_SEARCH_GREEDY`) applies each move once and evaluates it (chance averaged): an easy opponent, or a baseline.

### The MCTS reward

MCTS scores each playout for every player with `options.reward` (a [cw_search_reward](api:cw_search_reward)):

- `CW_SEARCH_REWARD_MARGIN` (the default): a logistic of the player's lead over the best other player, `1 / (1 + e^(-lead / reward_scale))`. Ahead it climbs toward 1, behind it falls toward 0, and every point of the gap counts on the way.
- `CW_SEARCH_REWARD_PLACE`: the player's place between the lowest and highest score (0..1), the only reward before v0.84.0. The last of three or four players, and the player behind of two, scores 0 whatever it does, so a CPU that falls behind plays at random and falls further behind.

`reward_scale` is in `evaluate`'s units: the lead worth 0.73. Set it to about a fifth of a typical winning margin in the game's points; the default, 4, is that for a race to 20. Too small and every lead past a few points saturates to 1 or 0, which makes a player far behind blind again; too large and the rewards flatten toward 0.5, so UCT needs more nodes to tell moves apart.

The default comes from measurement on the dice game of `tests/unit/test_search.c` (a race to 20, 4000 nodes a move). With one margin seat against place seats and one place seat against margin seats, over 480, 720 and 960 games for two, three and four players, scale 4 was even with two players (242 and 239 wins of a fair 240) and stronger with three (300 against 202 of 240) and four (287 against 192); against random seats both rewards won the same (365 and 363 of 400 with two players, 517 and 517 of 600 with three, 643 and 651 of 800 with four). Scale 8 won more head to head with two players but lost 2-player games against random seats that the place wins (329 against 363 of 400); 16 and 32 lost 4-player games. A seat 30 points behind with no way to win chose the move that closes the gap most in 17 of 20 searches with four players and 18 with two on the margin, and 0 of 20 on the place. `CW_SEARCH_SCALE` and `CW_SEARCH_GAMES` re-run the measurement in a native `unit_search`.

## Spreading a search over ticks

```c
cw_search_options o;
cw_search_level_options(cpu_level, CW_SEARCH_MCTS, &o);   /* the shell's CPU Easy, Normal or Hard */
o.seed = match_seed ^ (uint64_t)turn_number;
cpu = cw_search_new(&rules, &o);

/* when the CPU's turn begins */
cw_search_start(cpu, &state);

/* every fixed_update until it is done: at most 2 ms and 4000 nodes of thinking a tick */
if (cw_search_step(cpu, 2.0f, 4000)) {
  cw_search_move m;
  cw_search_result(cpu, &m);
  /* press it through the seat's actions (cw_input_set_actions), as a person would */
}
```

[cw_search_step](api:cw_search_step) runs one slice and returns true when the answer is final. [cw_search_run](api:cw_search_run) does everything at once (tests, headless matches). [cw_search_start_background](api:cw_search_start_background) runs the whole search on the job pool's background queue while the game keeps drawing; poll [cw_search_done](api:cw_search_done). The callbacks then run on a worker thread, so they must touch only the state they are given.

## Determinism and budgets

Work is counted in nodes: one `apply` or one `evaluate`, in every kind. Given the same state, options and seed, the answer depends only on `max_nodes`, `max_depth` and the game: never on how the work is split into slices or which host runs it. A slice stops only between whole units (a node in minimax, a whole playout in MCTS), so a search spread over twenty ticks plays the move an all-at-once run plays.

A millisecond limit (a slice's `ms`, or `time_ms` over the whole search) ends work by the wall clock, so with one the answer can differ per host and per run. That is fine for play, as long as the CPU's move reaches the game as seat actions: the input recording then replays the match without searching again. Gates, tests and anything replayed by searching keep to node budgets.

## Difficulty

A weaker player should still look like it is trying. Difficulty is seeded mistakes over a real search: once the root moves are ranked, with probability `mistake` the move played is another one, drawn from the next `mistake_spread` best and weighted toward the better ones. Minimax searches every root move with a full window when mistakes are on, so the ranking is exact. A shallower `max_depth` and a smaller `max_nodes` weaken it further. [cw_search_level_options](api:cw_search_level_options) maps the shell's CPU levels:

| Level | mistake | minimax depth | nodes |
|---|---|---|---|
| Easy (0) | 0.35 | 2 | 20 000 |
| Normal (1) | 0.1 | 4 | 100 000 |
| Hard (2) | 0 | 64 | 200 000 |

[cw_search_get_stats](api:cw_search_get_stats) tells the best move before any mistake, the move played and whether a mistake was made, for logs and tests; [cw_search_root_moves](api:cw_search_root_moves) lists the root moves as ranked.

## Testing a CPU

The `unit-search` gate holds the module to independent answers: minimax keeps the game-theoretic value from every one of tic-tac-toe's 4520 open positions (so it never loses from any of them) and draws against itself; it finds the one winning move of connect-four positions whose win is seven plies away, checked by a brute-force search; a player who moves twice in a row is searched as one; the values at a die roll are the weighted averages; MCTS beats uniform random play at tic-tac-toe and at a dice game for two and for three players over many seeded games; a seat that cannot win still picks the move that closes the gap (the control runs it on the place reward, where it does not); a sliced search equals an all-at-once one; a millisecond budget is kept. Its control plays through a weakened player and a random baseline, and must fail.

For a game's own CPU, the same pattern works: play many seeded headless matches against random play and against the previous level, assert a margin, and keep the matches on node budgets so the numbers repeat on every host.
