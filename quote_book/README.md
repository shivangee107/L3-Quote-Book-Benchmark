# L3 Quote Book Benchmark

A small C++20 benchmark for comparing five L3 order book implementations under the same replayed market-data stream.

The feed is generated as fixed-size binary messages and parsed from a raw `char*` buffer. Messages support:

- `N` = new order
- `X` = cancel order
- `M` = modify order
- `T` = trade message

Each book stores individual orders at a price level in FIFO order using `std::list<Order>`.
The implementations differ only in how they index price levels and recover best bid/ask.

## Books compared

1. `L3MapBook` — `std::map<Price, PriceLevel>`
2. `L3LinkedListBook` — sorted linked list of price levels
3. `L3ArrayBookScan` — array indexed by price tick, scan for best recovery
4. `L3ArrayBookFenwick` — array + Fenwick tree of active levels
5. `L3ArrayBookBitset` — array + bitset of active levels

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Run

Small sanity test:

```bash
./build/l3_quote_book_benchmark \
  --messages 100000 \
  --warmup-messages 20000 \
  --seed 42 \
  --levels-per-side 1000
```

Full comparison:

```bash
./build/l3_quote_book_benchmark \
  --messages 5000000 \
  --warmup-messages 100000 \
  --seed 42 \
  --levels-per-side 10000
```

Larger run:

```bash
./build/l3_quote_book_benchmark \
  --messages 20000000 \
  --warmup-messages 100000 \
  --seed 42 \
  --levels-per-side 10000
```

## Useful output checks

- `crossed` should be `0` for every book.
- `invalid` should be `0` for every book.
- `checksum` should match across all five books.
- `final_bid` and `final_ask` should match across all five books.
- `work/msg` explains why linked-list and scan-based approaches become expensive.
