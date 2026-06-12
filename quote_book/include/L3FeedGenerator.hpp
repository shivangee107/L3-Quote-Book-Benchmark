#pragma once

#include "Common.hpp"
#include "L3MarketDataMessage.hpp"
#include "L3OrderBook.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>
#include <random>
#include <vector>

struct FeedGeneratorConfig {
    std::uint32_t seed = 42;
    int levels_per_side = 10'000;
    int min_spread_ticks = 2;
};

// Generates a deterministic, realistic-enough L3 feed:
// - fixed-size binary messages are produced later by L3MarketDataMessage.hpp
// - N/X/M/T opcodes are present
// - N/M can cross and perform matching in the shadow book
// - T messages are emitted after crossing messages but are informational for the books
class L3FeedGenerator {
public:
    explicit L3FeedGenerator(FeedGeneratorConfig cfg)
        : cfg_(cfg), rng_(cfg.seed) {
        seedInitialBook();
    }

    L3Message next(std::uint64_t seq_num, std::uint64_t send_timestamp_ns) {
        if (!pending_.empty()) {
            L3Message msg = pending_.front();
            pending_.pop_front();
            msg.seq_num = seq_num;
            msg.send_timestamp_ns = send_timestamp_ns;
            return msg;
        }

        const int live_order_floor = std::max(100, 2 * cfg_.levels_per_side);
        if (static_cast<int>(live_order_ids_.size()) < live_order_floor) {
            return makeNew(seq_num, send_timestamp_ns, PriceMode::Wide);
        }

        const int roll = uniformInt(1, 100);
        if (roll <= 45) return makeModify(seq_num, send_timestamp_ns);
        if (roll <= 70) return makeNew(seq_num, send_timestamp_ns, PriceMode::Wide);
        if (roll <= 90) return makeCancel(seq_num, send_timestamp_ns);
        return makeCrossingNew(seq_num, send_timestamp_ns);
    }

private:
    enum class PriceMode { NearTouch, Wide };

    int uniformInt(int lo, int hi) {
        std::uniform_int_distribution<int> dist(lo, hi);
        return dist(rng_);
    }

    Qty randomQty() {
        return uniformInt(1, 100);
    }

    Side randomSide() {
        return uniformInt(0, 1) == 0 ? Side::Bid : Side::Ask;
    }

    Price halfSpread() const {
        return std::max<Price>(1, cfg_.min_spread_ticks / 2);
    }

    Price maxBidPrice() const {
        return kDefaultMidPriceTick - halfSpread();
    }

    Price minAskPrice() const {
        return kDefaultMidPriceTick + halfSpread();
    }

    Price randomPrice(Side side, PriceMode mode) {
        if (mode == PriceMode::NearTouch) {
            const int depth = uniformInt(1, 20);
            return side == Side::Bid
                ? static_cast<Price>(maxBidPrice() - depth)
                : static_cast<Price>(minAskPrice() + depth);
        }

        if (side == Side::Bid) {
            return static_cast<Price>(uniformInt(kDefaultMinPriceTick, maxBidPrice()));
        }
        return static_cast<Price>(uniformInt(minAskPrice(), kDefaultMaxPriceTick));
    }

    L3Message makeOrderMessage(char op,
                               Side side,
                               OrderId order_id,
                               Price price,
                               Qty qty,
                               std::uint64_t seq_num,
                               std::uint64_t send_timestamp_ns) const {
        return L3Message{op, side, "FOO", seq_num, send_timestamp_ns, order_id, price, qty, 0};
    }

    L3Message makeTradeMessage(Side aggressor_side,
                               Price price,
                               Qty qty,
                               std::uint64_t seq_num,
                               std::uint64_t send_timestamp_ns) {
        return L3Message{'T', aggressor_side, "FOO", seq_num, send_timestamp_ns,
                         0, price, qty, next_trade_id_++};
    }

    void seedInitialBook() {
        const int max_bid_levels = std::max(1, maxBidPrice() - kDefaultMinPriceTick);
        const int max_ask_levels = std::max(1, kDefaultMaxPriceTick - minAskPrice());
        const int levels = std::min({cfg_.levels_per_side, max_bid_levels, max_ask_levels});

        std::uint64_t seq = 1;
        for (int i = 1; i <= levels; ++i) {
            const Price bid_price = static_cast<Price>(maxBidPrice() - i);
            const Price ask_price = static_cast<Price>(minAskPrice() + i);

            L3Message bid = makeOrderMessage('N', Side::Bid, next_order_id_++, bid_price, randomQty(), seq++, 0);
            L3Message ask = makeOrderMessage('N', Side::Ask, next_order_id_++, ask_price, randomQty(), seq++, 0);

            applyAndRemember(bid);
            applyAndRemember(ask);
            pending_.push_back(bid);
            pending_.push_back(ask);
        }
    }

    void applyAndRemember(const L3Message& msg) {
        shadow_book_.apply(msg);
        if ((msg.op_code == 'N' || msg.op_code == 'M') && shadow_book_.hasOrder(msg.order_id)) {
            live_order_ids_.push_back(msg.order_id);
        }
    }

    std::optional<OrderId> randomLiveOrderId() {
        for (int attempts = 0; attempts < 20 && !live_order_ids_.empty(); ++attempts) {
            const std::size_t idx = static_cast<std::size_t>(uniformInt(0, static_cast<int>(live_order_ids_.size() - 1)));
            const OrderId order_id = live_order_ids_[idx];
            if (shadow_book_.hasOrder(order_id)) {
                return order_id;
            }
            live_order_ids_[idx] = live_order_ids_.back();
            live_order_ids_.pop_back();
        }
        return std::nullopt;
    }

    L3Message makeNew(std::uint64_t seq_num, std::uint64_t ts, PriceMode price_mode) {
        const Side side = randomSide();
        const Price price = randomPrice(side, price_mode);
        const OrderId order_id = next_order_id_++;
        L3Message msg = makeOrderMessage('N', side, order_id, price, randomQty(), seq_num, ts);
        applyAndRemember(msg);
        return msg;
    }

    L3Message makeCancel(std::uint64_t seq_num, std::uint64_t ts) {
        const auto order_id = randomLiveOrderId();
        if (!order_id) return makeNew(seq_num, ts, PriceMode::Wide);

        const auto snapshot = shadow_book_.getOrder(*order_id);
        if (!snapshot) return makeNew(seq_num, ts, PriceMode::Wide);

        L3Message msg = makeOrderMessage('X', snapshot->side, *order_id, 0, 0, seq_num, ts);
        shadow_book_.apply(msg);
        return msg;
    }

    L3Message makeModify(std::uint64_t seq_num, std::uint64_t ts) {
        const auto order_id = randomLiveOrderId();
        if (!order_id) return makeNew(seq_num, ts, PriceMode::Wide);

        const auto snapshot = shadow_book_.getOrder(*order_id);
        if (!snapshot) return makeNew(seq_num, ts, PriceMode::Wide);

        const PriceMode price_mode = uniformInt(1, 100) <= 70 ? PriceMode::Wide : PriceMode::NearTouch;
        const Price price = randomPrice(snapshot->side, price_mode);
        L3Message msg = makeOrderMessage('M', snapshot->side, *order_id, price, randomQty(), seq_num, ts);
        shadow_book_.apply(msg);
        if (shadow_book_.hasOrder(*order_id)) {
            live_order_ids_.push_back(*order_id);
        }
        return msg;
    }

    L3Message makeCrossingNew(std::uint64_t seq_num, std::uint64_t ts) {
        const Side side = randomSide();
        const auto best_bid = shadow_book_.bestBid();
        const auto best_ask = shadow_book_.bestAsk();

        if ((side == Side::Bid && !best_ask) || (side == Side::Ask && !best_bid)) {
            return makeNew(seq_num, ts, PriceMode::NearTouch);
        }

        Price order_price{};
        Price trade_price{};
        if (side == Side::Bid) {
            trade_price = best_ask->price;
            order_price = static_cast<Price>(best_ask->price + uniformInt(0, 3));
        } else {
            trade_price = best_bid->price;
            order_price = static_cast<Price>(best_bid->price - uniformInt(0, 3));
        }

        const Qty qty = randomQty();
        const OrderId order_id = next_order_id_++;
        L3Message msg = makeOrderMessage('N', side, order_id, order_price, qty, seq_num, ts);
        shadow_book_.apply(msg);
        if (shadow_book_.hasOrder(order_id)) {
            live_order_ids_.push_back(order_id);
        }
        pending_.push_back(makeTradeMessage(side, trade_price, qty, 0, 0));
        return msg;
    }

    FeedGeneratorConfig cfg_;
    std::mt19937 rng_;
    OrderId next_order_id_ = 1;
    TradeId next_trade_id_ = 1;
    L3OrderBook shadow_book_;
    std::vector<OrderId> live_order_ids_;
    std::deque<L3Message> pending_;
};
