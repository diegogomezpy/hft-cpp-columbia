# HW1 — Order-Book Metrics (mid, spread, microprice, OBI)

Implementation: [`scratch/book_metrics.cpp`](scratch/book_metrics.cpp)
Sample input: [`scratch/snapshots.txt`](scratch/snapshots.txt)

## What it does

Reads a top-of-book snapshot — arrays of `(price, qty)` levels for bids and
asks — and computes:

- **Best bid / best ask**: max bid price, min ask price (found with
  `max_element`/`min_element`, so input order doesn't matter).
- **Mid**: `(best_bid + best_ask) / 2`.
- **Spread**: `best_ask - best_bid`. A book where `best_ask <= best_bid` is
  flagged `crossed`/locked instead of silently printing a negative spread.
- **Microprice** (size-weighted touch): mid pulled toward the thinner side —

  ```
  micro = (best_ask * bid_qty + best_bid * ask_qty) / (bid_qty + ask_qty)
  ```

- **Order-book imbalance (OBI)**, in `[-1, 1]`:

  ```
  obi = (bid_qty - ask_qty) / (bid_qty + ask_qty)
  ```

  `+1` = all resting size is on the bid, `-1` = all on the ask.

Given a sequence of snapshots, it prints mid and OBI for each one plus the
change (`d_mid`, `d_obi`) from the previous snapshot.

### Stretch goal: snapshots from a file

`read_snapshots(path)` parses a small text format instead of hardcoding data:

```
BID <price> <qty>
ASK <price> <qty>
---              # separates one snapshot from the next
# lines starting with '#' are comments
```

Run it either way:

```bash
g++ -std=c++17 -O2 scratch/book_metrics.cpp -o /tmp/bm
/tmp/bm                        # falls back to 3 hardcoded snapshots
/tmp/bm scratch/snapshots.txt  # stretch: reads snapshots from a file
```

## Results

**Hardcoded snapshots** (balanced → bids build up → sellers take over):

```
snap  0: best_bid=100.0000 best_ask=100.0200 mid=100.0100 spread=0.0200 micro=100.0100 obi=+0.0000
snap  1: best_bid=100.0000 best_ask=100.0200 mid=100.0100 spread=0.0200 micro=100.0160 obi=+0.6000
         d_mid=+0.0000 d_obi=+0.6000
snap  2: best_bid=99.9800 best_ask=100.0000 mid=99.9900 spread=0.0200 micro=99.9830 obi=-0.7000
         d_mid=-0.0200 d_obi=-1.3000
```

**From `scratch/snapshots.txt`** (balanced → bids build up → build further →
sellers take over):

```
snap  0: best_bid=100.0000 best_ask=100.0200 mid=100.0100 spread=0.0200 micro=100.0100 obi=+0.0000
snap  1: best_bid=100.0000 best_ask=100.0200 mid=100.0100 spread=0.0200 micro=100.0160 obi=+0.6000
         d_mid=+0.0000 d_obi=+0.6000
snap  2: best_bid=100.0000 best_ask=100.0200 mid=100.0100 spread=0.0200 micro=100.0181 obi=+0.8095
         d_mid=+0.0000 d_obi=+0.2095
snap  3: best_bid=99.9800 best_ask=100.0000 mid=99.9900 spread=0.0200 micro=99.9830 obi=-0.7000
         d_mid=-0.0200 d_obi=-1.5095
```

Sanity checks (per the lab's "Your turn" list):
- Balanced book (`bid_qty == ask_qty`, snapshot 0): `obi == 0` and
  `micro == mid`. ✅
- Crossed/locked guard: `compute()` sets `crossed = true` when
  `best_ask <= best_bid` instead of returning a negative/zero spread as if
  nothing were wrong.

## Interpretation: what a rising OBI suggests

As resting bid size grows relative to ask size (snapshots 0→1→2), OBI climbs
from `0.00` to `+0.60` to `+0.81`, and the microprice climbs *with* it —
moving away from the mid and leaning toward the ask (`100.0100` →
`100.0160` → `100.0181`). That lean is the signal: buyers are crowding the
touch, so the next trade is more likely to lift the offer than hit the bid,
and the mid is more likely to tick **up** than down.

The reverse shows up sharply in the last snapshot: once ask size dominates,
OBI flips to `-0.70` and both microprice and mid drop. Because microprice
reacts to size changes *before* the mid actually moves, it's a better
short-horizon predictor of the next mid than the mid itself — this lean is
the edge HFT strategies built on OBI/microprice are trying to trade.
