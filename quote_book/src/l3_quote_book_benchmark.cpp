#include "BenchmarkUtil.hpp"
#include "FixedHistogram.hpp"
#include "L3FeedGenerator.hpp"
#include "L3MarketDataMessage.hpp"
#include "L3OrderBook.hpp"
#include "TimeUtil.hpp"

#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Config {
    std::uint64_t messages = 1'000'000;
    std::uint64_t warmup_messages = 100'000;
    std::uint32_t seed = 42;
    int levels_per_side = 10'000;
    int min_spread_ticks = 2;
};

std::uint64_t parseU64(const std::string& text, const std::string& flag) {
    try {
        std::size_t parsed = 0;
        const auto value = std::stoull(text, &parsed);
        if (parsed != text.size()) throw std::invalid_argument("trailing characters");
        return value;
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid value for " + flag + ": " + text);
    }
}

Config parseArgs(int argc, char** argv) {
    Config cfg;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto valueAfter = [&](const std::string& flag) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("Missing value after " + flag);
            return argv[++i];
        };

        if (arg == "--messages") {
            cfg.messages = parseU64(valueAfter(arg), arg);
        } else if (arg == "--warmup-messages") {
            cfg.warmup_messages = parseU64(valueAfter(arg), arg);
        } else if (arg == "--seed") {
            cfg.seed = static_cast<std::uint32_t>(parseU64(valueAfter(arg), arg));
        } else if (arg == "--levels-per-side") {
            cfg.levels_per_side = static_cast<int>(parseU64(valueAfter(arg), arg));
        } else if (arg == "--min-spread-ticks") {
            cfg.min_spread_ticks = static_cast<int>(parseU64(valueAfter(arg), arg));
        } else if (arg == "--help") {
            std::cout << "Usage: l3_quote_book_benchmark "
                      << "[--messages 1000000] [--warmup-messages 100000] "
                      << "[--seed 42] [--levels-per-side 10000] [--min-spread-ticks 2]\n";
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown argument: " + arg);
        }
    }

    if (cfg.messages == 0) throw std::runtime_error("--messages must be positive");
    if (cfg.warmup_messages == 0) throw std::runtime_error("--warmup-messages must be positive");
    if (cfg.levels_per_side <= 0) throw std::runtime_error("--levels-per-side must be positive");
    if (cfg.min_spread_ticks < 2) throw std::runtime_error("--min-spread-ticks must be at least 2");
    return cfg;
}

struct Percentiles {
    std::string p50;
    std::string p90;
    std::string p99;
};

Percentiles percentiles(const FixedHistogram& hist) {
    return {hist.percentileLabel(50.0), hist.percentileLabel(90.0), hist.percentileLabel(99.0)};
}

struct BookResult {
    std::string name;
    std::uint64_t measured_messages{};

    FixedHistogram parse_hist{FixedHistogram::fineLatencyBounds()};
    FixedHistogram update_hist{FixedHistogram::fineLatencyBounds()};
    FixedHistogram best_hist{FixedHistogram::fineLatencyBounds()};
    FixedHistogram total_hist{FixedHistogram::fineLatencyBounds()};

    std::uint64_t parse_sum_ns{};
    std::uint64_t update_sum_ns{};
    std::uint64_t best_sum_ns{};
    std::uint64_t total_sum_ns{};

    double total_ms{};
    double messages_per_second{};
    std::optional<Level> final_bid;
    std::optional<Level> final_ask;
    std::uint64_t crossed{};
    std::uint64_t invalid{};
    std::uint64_t checksum{};
    L3BookStats stats{};
};

std::vector<char> generateStream(const Config& cfg) {
    L3FeedGenerator generator({cfg.seed, cfg.levels_per_side, cfg.min_spread_ticks});
    const std::uint64_t total_messages = cfg.messages + cfg.warmup_messages;

    std::vector<char> stream;
    stream.reserve(static_cast<std::size_t>(total_messages * l3wire::kMessageSize));

    for (std::uint64_t i = 0; i < total_messages; ++i) {
        const L3Message msg = generator.next(i + 1, 0);
        l3wire::appendSerialized(stream, msg);
    }
    return stream;
}

template <typename Book>
BookResult runBook(const std::vector<char>& stream, const Config& cfg) {
    Book book;
    BookResult result;
    result.name = book.name();
    result.measured_messages = cfg.messages;

    std::uint64_t wall_start_ns = 0;
    std::uint64_t wall_end_ns = 0;
    const std::uint64_t total_messages = cfg.messages + cfg.warmup_messages;

    for (std::uint64_t i = 0; i < total_messages; ++i) {
        const char* raw = stream.data() + i * l3wire::kMessageSize;
        const bool measure = i >= cfg.warmup_messages;
        if (measure && wall_start_ns == 0) wall_start_ns = nowNs();

        const std::uint64_t parse_start = measure ? nowNs() : 0;
        const auto msg = l3wire::parseOne(raw, l3wire::kMessageSize);
        const std::uint64_t parse_done = measure ? nowNs() : 0;

        if (!msg) {
            if (measure) ++result.invalid;
            continue;
        }

        const std::uint64_t update_start = measure ? nowNs() : 0;
        book.apply(*msg);
        const std::uint64_t update_done = measure ? nowNs() : 0;

        const auto bid = book.bestBid();
        const auto ask = book.bestAsk();
        const std::uint64_t best_done = measure ? nowNs() : 0;

        if (measure) {
            const std::uint64_t parse_ns = parse_done - parse_start;
            const std::uint64_t update_ns = update_done - update_start;
            const std::uint64_t best_ns = best_done - update_done;
            const std::uint64_t total_ns = best_done - parse_start;

            result.parse_hist.record(parse_ns);
            result.update_hist.record(update_ns);
            result.best_hist.record(best_ns);
            result.total_hist.record(total_ns);

            result.parse_sum_ns += parse_ns;
            result.update_sum_ns += update_ns;
            result.best_sum_ns += best_ns;
            result.total_sum_ns += total_ns;
            wall_end_ns = best_done;

            if (bid && ask && bid->price >= ask->price) ++result.crossed;
        }

        mixBestLevelsIntoChecksum(result.checksum, bid, ask);
    }

    result.final_bid = book.bestBid();
    result.final_ask = book.bestAsk();
    result.stats = book.stats();
    result.total_ms = static_cast<double>(wall_end_ns - wall_start_ns) / 1'000'000.0;
    result.messages_per_second = static_cast<double>(cfg.messages) / (result.total_ms / 1000.0);
    return result;
}

void printLatencyTable(const std::string& title,
                       const std::vector<BookResult>& results,
                       const FixedHistogram BookResult::*hist,
                       std::uint64_t BookResult::*sum) {
    std::cout << "\n" << title << "\n";
    std::cout << std::left << std::setw(28) << "book"
              << std::right << std::setw(14) << "messages"
              << std::setw(12) << "avg_ns"
              << std::setw(16) << "p50"
              << std::setw(16) << "p90"
              << std::setw(16) << "p99" << '\n';

    for (const auto& result : results) {
        const FixedHistogram& h = result.*hist;
        const Percentiles p = percentiles(h);
        const double avg = h.count() == 0 ? 0.0 : static_cast<double>(result.*sum) / static_cast<double>(h.count());

        std::cout << std::left << std::setw(28) << result.name
                  << std::right << std::setw(14) << h.count()
                  << std::setw(12) << std::fixed << std::setprecision(1) << avg
                  << std::setw(16) << p.p50
                  << std::setw(16) << p.p90
                  << std::setw(16) << p.p99 << '\n';
    }
}

void printThroughputTable(const std::vector<BookResult>& results) {
    std::cout << "\nThroughput and final book state\n";
    std::cout << std::left << std::setw(28) << "book"
              << std::right << std::setw(12) << "total_ms"
              << std::setw(14) << "msgs/sec"
              << std::setw(18) << "final_bid"
              << std::setw(18) << "final_ask"
              << std::setw(10) << "crossed"
              << std::setw(10) << "invalid"
              << std::setw(22) << "checksum" << '\n';

    for (const auto& r : results) {
        std::cout << std::left << std::setw(28) << r.name
                  << std::right << std::setw(12) << std::fixed << std::setprecision(2) << r.total_ms
                  << std::setw(14) << std::fixed << std::setprecision(0) << r.messages_per_second
                  << std::setw(18) << levelToString(r.final_bid)
                  << std::setw(18) << levelToString(r.final_ask)
                  << std::setw(10) << r.crossed
                  << std::setw(10) << r.invalid
                  << std::setw(22) << checksumToHex(r.checksum) << '\n';
    }
}

void printStatsTable(const std::vector<BookResult>& results) {
    std::cout << "\nBook structure stats\n";
    std::cout << std::left << std::setw(28) << "book"
              << std::right << std::setw(14) << "orders"
              << std::setw(12) << "bid_lvls"
              << std::setw(12) << "ask_lvls"
              << std::setw(12) << "bid_inv"
              << std::setw(12) << "ask_inv"
              << std::setw(14) << "work_units"
              << std::setw(12) << "work/msg" << '\n';

    for (const auto& r : results) {
        const double work_per_msg = r.measured_messages == 0
            ? 0.0
            : static_cast<double>(r.stats.price_level_work_units) / static_cast<double>(r.measured_messages);

        std::cout << std::left << std::setw(28) << r.name
                  << std::right << std::setw(14) << r.stats.active_orders
                  << std::setw(12) << r.stats.active_bid_levels
                  << std::setw(12) << r.stats.active_ask_levels
                  << std::setw(12) << r.stats.best_bid_invalidations
                  << std::setw(12) << r.stats.best_ask_invalidations
                  << std::setw(14) << r.stats.price_level_work_units
                  << std::setw(12) << std::fixed << std::setprecision(2) << work_per_msg << '\n';
    }
}

void printMessageMix(const BookResult& r) {
    const auto& s = r.stats;
    std::cout << "\nMessage mix observed by books, including warmup\n"
              << "new=" << s.new_messages
              << " cancel=" << s.cancel_messages
              << " modify=" << s.modify_messages
              << " trade=" << s.trade_messages
              << " ignored_trade=" << s.ignored_trade_messages
              << " crossing_new=" << s.crossing_new_orders
              << " crossing_modify=" << s.crossing_modify_orders
              << " match_events=" << s.match_events
              << " traded_qty=" << s.traded_qty << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Config cfg = parseArgs(argc, argv);
        const std::vector<char> stream = generateStream(cfg);

        std::cout << "Generated L3 byte stream total_messages=" << (cfg.messages + cfg.warmup_messages)
                  << " measured=" << cfg.messages
                  << " warmup=" << cfg.warmup_messages
                  << " levels_per_side=" << cfg.levels_per_side
                  << " seed=" << cfg.seed
                  << " message_size=" << l3wire::kMessageSize << " bytes\n";
        std::cout << "fine_latency_buckets="
                  << FixedHistogram::describeBounds(FixedHistogram::fineLatencyBounds()) << "\n";

        std::vector<BookResult> results;
        results.push_back(runBook<L3MapOrderBook>(stream, cfg));
        results.push_back(runBook<L3LinkedListOrderBook>(stream, cfg));
        results.push_back(runBook<L3ArrayScanOrderBook>(stream, cfg));
        results.push_back(runBook<L3ArrayFenwickBook>(stream, cfg));
        results.push_back(runBook<L3ArrayBitsetBook>(stream, cfg));

        std::cout << "\nL3 quote book benchmark: char* parse + FIFO order book update + best-level lookup\n";
        printLatencyTable("1) Parse latency", results, &BookResult::parse_hist, &BookResult::parse_sum_ns);
        printLatencyTable("2) Book update latency", results, &BookResult::update_hist, &BookResult::update_sum_ns);
        printLatencyTable("3) Best level call latency", results, &BookResult::best_hist, &BookResult::best_sum_ns);
        printLatencyTable("4) Parse + update + best latency", results, &BookResult::total_hist, &BookResult::total_sum_ns);
        printThroughputTable(results);
        printStatsTable(results);
        if (!results.empty()) printMessageMix(results.front());

        std::cout << "\nNotes:\n"
                  << "- Every book stores FIFO orders at each price level using std::list<Order>.\n"
                  << "- Checksums and final best levels should match across all books.\n"
                  << "- The books differ only in price-level indexing and best-level recovery.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << '\n';
        return 1;
    }
}
