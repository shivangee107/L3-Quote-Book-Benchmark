I drafted this README to match your current code and interview story: five variants, fixed-size L3 messages, synthetic replay, latency histograms, checksum validation, and production-style tradeoffs. 

# Low-Latency L3 Quote Book Benchmark

A C++ benchmarking system for comparing multiple **L3 quote-book implementations** under deterministic market-data replay.

The project processes fixed-size binary market-data messages containing **new**, **cancel**, **modify**, and **trade** events. It maintains FIFO order queues at each price level, supports orderId-based cancel/modify, and benchmarks multiple price-level indexing strategies for best bid/ask recovery.

## Key Features

* Fixed-size 64-byte L3 market-data message format
* Supports `N`ew, `X` cancel, `M`odify, and `T`rade events
* Maintains FIFO order queues per price level
* Supports fast orderId-based cancel/modify
* Implements five order-book variants:

  * `std::map` based book
  * linked-list price-level book
  * array-scan book
  * array + Fenwick tree book
  * array + bitset book
* Generates deterministic synthetic L3 market-data replay
* Simulates deep books, cancels, modifies, crossing orders, and trades
* Measures:

  * parse latency
  * book update latency
  * best bid/ask lookup latency
  * total processing latency
  * throughput
* Reports histogram-based p50/p90/p99 latency buckets
* Uses checksum validation to compare correctness across implementations

## Project Motivation

In an L3 order book, market-data updates are order-specific. A cancel or modify event refers to an exact orderId, and trades consume resting liquidity according to price-time priority.

This benchmark explores how different data structures affect:

* update latency
* best bid/ask recovery
* cache locality
* pointer chasing
* tail latency
* memory usage
* sparse vs dense price-level behavior

The goal is not only to compare Big-O complexity, but also to understand practical low-latency behavior under realistic workloads.

## High-Level Architecture

```text
Synthetic L3 Feed Generator
        |
        v
Fixed-size Binary Message Stream
        |
        v
Parser
        |
        v
Book Update Engine
        |
        v
Price-Level Index Variant
        |
        v
Best Bid / Best Ask Lookup
        |
        v
Latency Histograms + Checksum
```

## L3 Book Model

Each book maintains individual orders, not just aggregate quantity.

At each price level:

```text
Price Level
    total quantity
    order count
    FIFO queue of orders
```

Each order stores:

```text
orderId
side
price
quantity
FIFO position
```

The book also maintains an orderId index:

```text
orderId -> exact order node
```

This allows cancel and modify events to locate the exact order efficiently.

## Supported Events

| Opcode | Event  | Description                          |
| ------ | ------ | ------------------------------------ |
| `N`    | New    | Adds a new order to the book         |
| `X`    | Cancel | Removes an existing order by orderId |
| `M`    | Modify | Updates an existing order            |
| `T`    | Trade  | Represents a generated trade event   |

## Order Book Variants

### 1. Map Book

Uses sorted maps for price levels.

```text
std::map<Price, PriceLevel>
```

Best bid/ask lookup is simple because prices are naturally sorted.

Strengths:

* simple correctness baseline
* handles sparse price levels naturally
* no fixed price range required

Weaknesses:

* tree node pointer chasing
* poor cache locality
* branch-heavy traversal
* higher tail-latency risk

### 2. Linked-List Price-Level Book

Maintains active price levels using linked-list style indexing.

Strengths:

* simple baseline
* explicit active-level management

Weaknesses:

* poor cache locality
* pointer chasing
* linear search/recovery cost

This variant is useful as a comparison point. Linked lists are good for FIFO queues inside a price level, but they are usually not ideal for price-level indexing.

### 3. Array-Scan Book

Uses direct price-to-index mapping.

```text
index = price - minPrice
```

Best bid/ask is found by scanning the array.

Strengths:

* O(1) price-level access
* contiguous memory
* cache-friendly
* simple implementation

Weaknesses:

* requires bounded price range
* may scan many empty levels
* wastes memory for sparse wide ranges

### 4. Array + Fenwick Tree Book

Uses an array for price levels and a Fenwick tree to track active levels.

The Fenwick tree stores:

```text
1 if price level is active
0 if price level is empty
```

Strengths:

* O(log R) active-level update
* O(log R) best-level lookup
* avoids full array scan
* predictable lookup cost

Weaknesses:

* more complex than array scan
* higher constant overhead than bitset
* still requires bounded price range

### 5. Array + Bitset Book

Uses an array for price levels and a compact bitset for active-level tracking.

Each bit represents one price level:

```text
1 = active
0 = empty
```

Best lookup scans 64-bit words and uses CPU bit operations:

```text
ctz -> find lowest set bit
clz -> find highest set bit
```

Strengths:

* O(1) activate/deactivate
* scans 64 levels at a time
* compact active-level representation
* very fast best bid/ask recovery for bounded ranges

Weaknesses:

* requires bounded price range
* huge sparse ranges can still waste memory
* must handle zero words carefully before ctz/clz

## Fixed-Size Market Data Format

Each message is encoded as a fixed-size 64-byte binary record.

Fields include:

```text
op_code
side
symbol
sequence number
send timestamp
orderId
price tick
quantity
tradeId
```

Fixed-size messages make parsing predictable and avoid string-heavy parsing in the benchmark loop.

## Synthetic Feed Generation

The benchmark includes a deterministic feed generator.

It supports:

* configurable random seed
* configurable book depth
* initial deep book seeding
* valid cancels and modifies using active order tracking
* crossing orders
* generated trade messages
* realistic bid/ask price generation around a mid price

The same generated byte stream is replayed across all book variants for fair comparison.

## Benchmark Metrics

For each book implementation, the benchmark measures:

| Metric            | Description                        |
| ----------------- | ---------------------------------- |
| Parse latency     | Raw bytes to parsed L3 message     |
| Update latency    | Applying event to book             |
| Best-call latency | Querying best bid and best ask     |
| Total latency     | Parse + update + best lookup       |
| Throughput        | Messages processed per second      |
| Checksum          | Correctness signal across variants |
| Work units        | Price-level recovery/scanning work |

Latency is recorded using fixed histogram buckets and reported as:

```text
average
p50
p90
p99
```

## Correctness Validation

The benchmark validates correctness using:

* final best bid/ask comparison
* checksum comparison
* active order count
* active bid/ask level count
* crossed-book count
* invalid-message count
* book structure stats

All implementations should produce the same logical final book state for the same replay.

## Build

A typical build command:

```bash
g++ -std=c++17 -O3 -march=native -DNDEBUG \
    -Iinclude \
    src/main.cpp \
    -o l3_quote_book_benchmark
```

Or using CMake if the project is organized with a `CMakeLists.txt`:

```bash
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
```

## Run

```bash
./l3_quote_book_benchmark
```

With custom parameters:

```bash
./l3_quote_book_benchmark \
    --messages 1000000 \
    --warmup-messages 100000 \
    --seed 42 \
    --levels-per-side 10000 \
    --min-spread-ticks 2
```

## Command-Line Options

| Option               |   Default | Description                          |
| -------------------- | --------: | ------------------------------------ |
| `--messages`         | `1000000` | Number of measured messages          |
| `--warmup-messages`  |  `100000` | Number of warm-up messages           |
| `--seed`             |      `42` | Random seed for deterministic replay |
| `--levels-per-side`  |   `10000` | Initial depth per side               |
| `--min-spread-ticks` |       `2` | Minimum bid/ask spread in ticks      |

## Example Output

The benchmark prints separate latency tables:

```text
1) Parse latency
2) Book update latency
3) Best level call latency
4) Parse + update + best latency
```

It also prints:

```text
Throughput and final book state
Book structure stats
Message mix observed by books
```

Example output format:

```text
book                          messages      avg_ns             p50             p90             p99
MapOrderBook                   1000000        ...
LinkedListOrderBook            1000000        ...
ArrayScanOrderBook             1000000        ...
ArrayFenwickBook               1000000        ...
ArrayBitsetBook                1000000        ...
```

## Important Design Tradeoffs

| Variant     | Best For                                | Main Tradeoff                    |
| ----------- | --------------------------------------- | -------------------------------- |
| Map         | sparse price levels                     | pointer chasing and cache misses |
| Linked-list | baseline comparison                     | poor price-level locality        |
| Array scan  | dense bounded range                     | may scan empty levels            |
| Fenwick     | sparse active levels in bounded range   | O(log R) update/lookup overhead  |
| Bitset      | bounded range with frequent best lookup | range-bound memory model         |

## Low-Latency Considerations

The benchmark highlights several low-latency design concerns:

* Big-O alone is not enough
* cache locality matters heavily
* pointer chasing increases tail latency
* dynamic allocation can distort benchmark results
* histogram buckets are cheaper than storing every sample
* deterministic replay is required for fair comparison
* checksum prevents the compiler from optimizing away useful work
* p99 latency is more important than average latency in market-data systems

## Production Extensions

To move this benchmark closer to a production market-data book builder, the following can be added:

* real exchange feed decoder
* sequence number validation
* gap detection
* snapshot + incremental recovery
* one book per instrument
* token-based routing instead of symbol lookup
* single-writer ownership per book
* SPSC publication to strategy
* preallocated order pools
* fixed-capacity hash map for orderId lookup
* low-overhead latency tracing
* book state: valid / stale / recovering

## Interview Summary

This project benchmarks different active price-level indexing strategies for an L3 quote book.

The common L3 core maintains:

```text
FIFO queues per price level
orderId index for cancel/modify
aggregate quantity per level
best bid/ask lookup
```

The five implementations differ in how active price levels are indexed and how best bid/ask is recovered.

The main learning is that in low-latency order books, performance depends not only on algorithmic complexity, but also on memory layout, cache locality, pointer chasing, allocation behavior, and tail latency.
