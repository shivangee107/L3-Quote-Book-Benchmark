#pragma once

#include "BenchmarkUtil.hpp"
#include "Common.hpp"
#include "L3MarketDataMessage.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct L3BookStats {
    std::uint64_t messages{};
    std::uint64_t new_messages{};
    std::uint64_t cancel_messages{};
    std::uint64_t modify_messages{};
    std::uint64_t trade_messages{};
    std::uint64_t ignored_trade_messages{};
    std::uint64_t crossing_new_orders{};
    std::uint64_t crossing_modify_orders{};
    std::uint64_t match_events{};
    std::uint64_t traded_qty{};
    std::uint64_t active_orders{};
    std::uint64_t active_bid_levels{};
    std::uint64_t active_ask_levels{};
    std::uint64_t best_bid_invalidations{};
    std::uint64_t best_ask_invalidations{};
    std::uint64_t price_level_work_units{};
};

struct L3OrderSnapshot {
    OrderId order_id{};
    Side side{};
    Price price{};
    Qty qty{};
};

namespace l3detail {

struct Order {
    OrderId order_id{};
    Qty qty{};
};

struct PriceLevel {
    Qty total_qty{};
    std::list<Order> orders;

    bool active() const {
        return total_qty > 0 && !orders.empty();
    }
};

inline int priceToIndex(Price price) {
    return static_cast<int>(price - kDefaultMinPriceTick);
}

inline Price indexToPrice(int index) {
    return static_cast<Price>(kDefaultMinPriceTick + index);
}

inline bool validIndex(int index) {
    return index >= 0 && index <= static_cast<int>(kDefaultMaxPriceTick - kDefaultMinPriceTick);
}

inline int priceRangeSize() {
    return static_cast<int>(kDefaultMaxPriceTick - kDefaultMinPriceTick + 1);
}

class FenwickTree {
public:
    explicit FenwickTree(int n = 0) : tree_(static_cast<std::size_t>(n + 1), 0) {}

    void reset(int n) {
        tree_.assign(static_cast<std::size_t>(n + 1), 0);
    }

    void add(int index_zero_based, int delta) {
        for (int i = index_zero_based + 1; i < static_cast<int>(tree_.size()); i += i & -i) {
            tree_[static_cast<std::size_t>(i)] += delta;
        }
    }

    int sumAll() const {
        int result = 0;
        for (int i = static_cast<int>(tree_.size()) - 1; i > 0; i -= i & -i) {
            result += tree_[static_cast<std::size_t>(i)];
        }
        return result;
    }

    int findByOrder(int order_one_based, std::uint64_t* work_units = nullptr) const {
        // Returns zero-based index whose prefix sum first reaches order_one_based.
        int idx = 0;
        int bit = 1;
        while ((bit << 1) < static_cast<int>(tree_.size())) {
            bit <<= 1;
        }

        int remaining = order_one_based;
        for (; bit != 0; bit >>= 1) {
            if (work_units) {
                ++(*work_units);
            }
            const int next = idx + bit;
            if (next < static_cast<int>(tree_.size()) && tree_[static_cast<std::size_t>(next)] < remaining) {
                idx = next;
                remaining -= tree_[static_cast<std::size_t>(next)];
            }
        }
        return idx;
    }

private:
    std::vector<int> tree_;
};

} // namespace l3detail

class L3MapOrderBook {
public:
    std::string name() const { return "L3MapBook"; }

    void apply(const L3Message& msg) {
        ++stats_.messages;
        switch (msg.op_code) {
            case 'N':
                ++stats_.new_messages;
                applyNew(msg.side, msg.order_id, msg.price_tick, msg.quantity, true);
                break;
            case 'X':
                ++stats_.cancel_messages;
                cancel(msg.order_id);
                break;
            case 'M':
                ++stats_.modify_messages;
                modify(msg.order_id, msg.price_tick, msg.quantity);
                break;
            case 'T':
                ++stats_.trade_messages;
                ++stats_.ignored_trade_messages;
                break;
            default:
                break;
        }
    }

    std::optional<Level> bestBid() const { return cached_best_bid_; }
    std::optional<Level> bestAsk() const { return cached_best_ask_; }

    bool crossed() const {
        return cached_best_bid_ && cached_best_ask_ && cached_best_bid_->price >= cached_best_ask_->price;
    }

    bool hasOrder(OrderId order_id) const {
        return order_refs_.find(order_id) != order_refs_.end();
    }

    std::optional<L3OrderSnapshot> getOrder(OrderId order_id) const {
        const auto ref_it = order_refs_.find(order_id);
        if (ref_it == order_refs_.end()) {
            return std::nullopt;
        }
        const auto& ref = ref_it->second;
        const auto& order = *ref.order_it;
        return L3OrderSnapshot{order.order_id, ref.side, ref.price, order.qty};
    }

    L3BookStats stats() const {
        L3BookStats result = stats_;
        result.active_orders = order_refs_.size();
        result.active_bid_levels = bids_.size();
        result.active_ask_levels = asks_.size();
        return result;
    }

private:
    using PriceLevel = l3detail::PriceLevel;
    using LevelMap = std::map<Price, PriceLevel>;
    using OrderIterator = std::list<l3detail::Order>::iterator;

    struct OrderRef {
        Side side{};
        Price price{};
        OrderIterator order_it{};
    };

    void applyNew(Side side, OrderId order_id, Price price, Qty qty, bool count_as_new_message) {
        if (qty <= 0 || order_id == 0) {
            return;
        }
        if (hasOrder(order_id)) {
            cancel(order_id);
        }
        const bool crossed_before = wouldCross(side, price);
        if (crossed_before) {
            count_as_new_message ? ++stats_.crossing_new_orders : ++stats_.crossing_modify_orders;
        }
        const Qty remaining = matchIncoming(side, price, qty);
        if (remaining > 0) {
            insertRestingOrder(side, order_id, price, remaining);
        }
    }

    bool wouldCross(Side side, Price price) const {
        if (side == Side::Bid) {
            return cached_best_ask_ && price >= cached_best_ask_->price;
        }
        return cached_best_bid_ && price <= cached_best_bid_->price;
    }

    Qty matchIncoming(Side incoming_side, Price limit_price, Qty incoming_qty) {
        Qty remaining = incoming_qty;
        if (incoming_side == Side::Bid) {
            while (remaining > 0 && !asks_.empty()) {
                auto level_it = asks_.begin();
                if (level_it->first > limit_price) break;
                remaining = consumeLevel(Side::Ask, level_it, remaining);
            }
        } else {
            while (remaining > 0 && !bids_.empty()) {
                auto level_it = std::prev(bids_.end());
                if (level_it->first < limit_price) break;
                remaining = consumeLevel(Side::Bid, level_it, remaining);
            }
        }
        return remaining;
    }

    Qty consumeLevel(Side resting_side, LevelMap::iterator level_it, Qty incoming_qty) {
        PriceLevel& level = level_it->second;
        while (incoming_qty > 0 && !level.orders.empty()) {
            auto& resting = level.orders.front();
            const Qty traded = std::min(incoming_qty, resting.qty);
            incoming_qty -= traded;
            resting.qty -= traded;
            level.total_qty -= traded;
            ++stats_.match_events;
            stats_.traded_qty += static_cast<std::uint64_t>(traded);
            if (resting.qty == 0) {
                order_refs_.erase(resting.order_id);
                level.orders.pop_front();
            }
        }
        if (level.orders.empty()) {
            const Price erased_price = level_it->first;
            if (resting_side == Side::Bid) {
                bids_.erase(level_it);
                if (cached_best_bid_ && cached_best_bid_->price == erased_price) {
                    ++stats_.best_bid_invalidations;
                    recoverBestBid();
                }
            } else {
                asks_.erase(level_it);
                if (cached_best_ask_ && cached_best_ask_->price == erased_price) {
                    ++stats_.best_ask_invalidations;
                    recoverBestAsk();
                }
            }
        } else {
            updateCachedLevel(resting_side, level_it->first, level.total_qty);
        }
        return incoming_qty;
    }

    void insertRestingOrder(Side side, OrderId order_id, Price price, Qty qty) {
        auto& levels = side == Side::Bid ? bids_ : asks_;
        PriceLevel& level = levels[price];
        level.total_qty += qty;
        level.orders.push_back(l3detail::Order{order_id, qty});
        order_refs_[order_id] = OrderRef{side, price, std::prev(level.orders.end())};
        if (side == Side::Bid) {
            if (!cached_best_bid_ || price > cached_best_bid_->price) {
                cached_best_bid_ = Level{price, level.total_qty};
            } else if (cached_best_bid_->price == price) {
                cached_best_bid_->qty = level.total_qty;
            }
        } else {
            if (!cached_best_ask_ || price < cached_best_ask_->price) {
                cached_best_ask_ = Level{price, level.total_qty};
            } else if (cached_best_ask_->price == price) {
                cached_best_ask_->qty = level.total_qty;
            }
        }
    }

    void cancel(OrderId order_id) {
        const auto ref_it = order_refs_.find(order_id);
        if (ref_it == order_refs_.end()) return;
        eraseByRef(ref_it);
    }

    void modify(OrderId order_id, Price new_price, Qty new_qty) {
        const auto ref_it = order_refs_.find(order_id);
        if (ref_it == order_refs_.end()) return;
        const Side side = ref_it->second.side;
        if (new_qty <= 0) {
            eraseByRef(ref_it);
            return;
        }
        eraseByRef(ref_it);
        applyNew(side, order_id, new_price, new_qty, false);
    }

    void eraseByRef(std::unordered_map<OrderId, OrderRef>::iterator ref_it) {
        const OrderRef ref = ref_it->second;
        auto& levels = ref.side == Side::Bid ? bids_ : asks_;
        auto level_it = levels.find(ref.price);
        if (level_it == levels.end()) {
            order_refs_.erase(ref_it);
            return;
        }
        PriceLevel& level = level_it->second;
        level.total_qty -= ref.order_it->qty;
        level.orders.erase(ref.order_it);
        order_refs_.erase(ref_it);
        ++stats_.price_level_work_units;

        if (level.orders.empty()) {
            levels.erase(level_it);
            if (ref.side == Side::Bid && cached_best_bid_ && cached_best_bid_->price == ref.price) {
                ++stats_.best_bid_invalidations;
                recoverBestBid();
            } else if (ref.side == Side::Ask && cached_best_ask_ && cached_best_ask_->price == ref.price) {
                ++stats_.best_ask_invalidations;
                recoverBestAsk();
            }
        } else {
            updateCachedLevel(ref.side, ref.price, level.total_qty);
        }
    }

    void updateCachedLevel(Side side, Price price, Qty qty) {
        if (side == Side::Bid && cached_best_bid_ && cached_best_bid_->price == price) cached_best_bid_->qty = qty;
        if (side == Side::Ask && cached_best_ask_ && cached_best_ask_->price == price) cached_best_ask_->qty = qty;
    }

    void recoverBestBid() {
        ++stats_.price_level_work_units;
        cached_best_bid_ = bids_.empty() ? std::optional<Level>{} : std::optional<Level>{Level{bids_.rbegin()->first, bids_.rbegin()->second.total_qty}};
    }

    void recoverBestAsk() {
        ++stats_.price_level_work_units;
        cached_best_ask_ = asks_.empty() ? std::optional<Level>{} : std::optional<Level>{Level{asks_.begin()->first, asks_.begin()->second.total_qty}};
    }

    LevelMap bids_;
    LevelMap asks_;
    std::unordered_map<OrderId, OrderRef> order_refs_;
    std::optional<Level> cached_best_bid_;
    std::optional<Level> cached_best_ask_;
    L3BookStats stats_;
};

class L3LinkedListOrderBook {
public:
    std::string name() const { return "L3LinkedListBook"; }

    void apply(const L3Message& msg) {
        ++stats_.messages;
        switch (msg.op_code) {
            case 'N': ++stats_.new_messages; applyNew(msg.side, msg.order_id, msg.price_tick, msg.quantity, true); break;
            case 'X': ++stats_.cancel_messages; cancel(msg.order_id); break;
            case 'M': ++stats_.modify_messages; modify(msg.order_id, msg.price_tick, msg.quantity); break;
            case 'T': ++stats_.trade_messages; ++stats_.ignored_trade_messages; break;
            default: break;
        }
    }

    std::optional<Level> bestBid() const { return best_bid_ == bid_levels_.end() ? std::nullopt : std::optional<Level>{Level{best_bid_->price, best_bid_->level.total_qty}}; }
    std::optional<Level> bestAsk() const { return best_ask_ == ask_levels_.end() ? std::nullopt : std::optional<Level>{Level{best_ask_->price, best_ask_->level.total_qty}}; }
    bool crossed() const { const auto b = bestBid(); const auto a = bestAsk(); return b && a && b->price >= a->price; }
    bool hasOrder(OrderId id) const { return order_refs_.find(id) != order_refs_.end(); }

    std::optional<L3OrderSnapshot> getOrder(OrderId id) const {
        const auto it = order_refs_.find(id);
        if (it == order_refs_.end()) return std::nullopt;
        const auto& ref = it->second;
        return L3OrderSnapshot{ref.order_it->order_id, ref.side, ref.level_it->price, ref.order_it->qty};
    }

    L3BookStats stats() const {
        L3BookStats result = stats_;
        result.active_orders = order_refs_.size();
        result.active_bid_levels = bid_levels_.size();
        result.active_ask_levels = ask_levels_.size();
        return result;
    }

private:
    struct LevelNode {
        Price price{};
        l3detail::PriceLevel level;
    };
    using LevelList = std::list<LevelNode>;
    using LevelIterator = LevelList::iterator;
    using OrderIterator = std::list<l3detail::Order>::iterator;
    struct OrderRef { Side side{}; LevelIterator level_it{}; OrderIterator order_it{}; };

    LevelList& levelsFor(Side side) { return side == Side::Bid ? bid_levels_ : ask_levels_; }
    const LevelList& levelsFor(Side side) const { return side == Side::Bid ? bid_levels_ : ask_levels_; }
    LevelIterator& bestItFor(Side side) { return side == Side::Bid ? best_bid_ : best_ask_; }

    bool betterOrEqual(Side side, Price lhs, Price rhs) const {
        return side == Side::Bid ? lhs >= rhs : lhs <= rhs;
    }

    bool betterThan(Side side, Price lhs, Price rhs) const {
        return side == Side::Bid ? lhs > rhs : lhs < rhs;
    }

    LevelIterator findOrInsertLevel(Side side, Price price) {
        LevelList& levels = levelsFor(side);
        for (auto it = levels.begin(); it != levels.end(); ++it) {
            ++stats_.price_level_work_units;
            if (it->price == price) return it;
            if (betterThan(side, price, it->price)) {
                return levels.insert(it, LevelNode{price, {}});
            }
        }
        return levels.insert(levels.end(), LevelNode{price, {}});
    }

    void applyNew(Side side, OrderId order_id, Price price, Qty qty, bool count_as_new_message) {
        if (qty <= 0 || order_id == 0) return;
        if (hasOrder(order_id)) cancel(order_id);
        const bool crossed_before = wouldCross(side, price);
        if (crossed_before) count_as_new_message ? ++stats_.crossing_new_orders : ++stats_.crossing_modify_orders;
        const Qty remaining = matchIncoming(side, price, qty);
        if (remaining > 0) insertRestingOrder(side, order_id, price, remaining);
    }

    bool wouldCross(Side side, Price price) const {
        const auto ask = bestAsk();
        const auto bid = bestBid();
        return side == Side::Bid ? (ask && price >= ask->price) : (bid && price <= bid->price);
    }

    Qty matchIncoming(Side incoming_side, Price limit_price, Qty qty) {
        Qty remaining = qty;
        const Side resting_side = incoming_side == Side::Bid ? Side::Ask : Side::Bid;
        LevelList& resting_levels = levelsFor(resting_side);
        LevelIterator& best = bestItFor(resting_side);
        while (remaining > 0 && best != resting_levels.end()) {
            if (incoming_side == Side::Bid && best->price > limit_price) break;
            if (incoming_side == Side::Ask && best->price < limit_price) break;
            remaining = consumeLevel(resting_side, best, remaining);
        }
        return remaining;
    }

    Qty consumeLevel(Side resting_side, LevelIterator level_it, Qty incoming_qty) {
        auto& level = level_it->level;
        while (incoming_qty > 0 && !level.orders.empty()) {
            auto& resting = level.orders.front();
            const Qty traded = std::min(incoming_qty, resting.qty);
            incoming_qty -= traded;
            resting.qty -= traded;
            level.total_qty -= traded;
            ++stats_.match_events;
            stats_.traded_qty += static_cast<std::uint64_t>(traded);
            if (resting.qty == 0) {
                order_refs_.erase(resting.order_id);
                level.orders.pop_front();
            }
        }
        if (level.orders.empty()) {
            eraseEmptyLevel(resting_side, level_it);
        }
        return incoming_qty;
    }

    void insertRestingOrder(Side side, OrderId order_id, Price price, Qty qty) {
        LevelIterator level_it = findOrInsertLevel(side, price);
        auto& level = level_it->level;
        level.total_qty += qty;
        level.orders.push_back(l3detail::Order{order_id, qty});
        order_refs_[order_id] = OrderRef{side, level_it, std::prev(level.orders.end())};
        LevelIterator& best = bestItFor(side);
        LevelList& levels = levelsFor(side);
        if (best == levels.end() || betterThan(side, price, best->price)) {
            best = level_it;
        }
    }

    void cancel(OrderId id) {
        auto it = order_refs_.find(id);
        if (it != order_refs_.end()) eraseByRef(it);
    }

    void modify(OrderId id, Price price, Qty qty) {
        auto it = order_refs_.find(id);
        if (it == order_refs_.end()) return;
        const Side side = it->second.side;
        if (qty <= 0) { eraseByRef(it); return; }
        eraseByRef(it);
        applyNew(side, id, price, qty, false);
    }

    void eraseByRef(std::unordered_map<OrderId, OrderRef>::iterator ref_it) {
        const auto ref = ref_it->second;
        auto& level = ref.level_it->level;
        level.total_qty -= ref.order_it->qty;
        level.orders.erase(ref.order_it);
        order_refs_.erase(ref_it);
        ++stats_.price_level_work_units;
        if (level.orders.empty()) eraseEmptyLevel(ref.side, ref.level_it);
    }

    void eraseEmptyLevel(Side side, LevelIterator level_it) {
        LevelList& levels = levelsFor(side);
        LevelIterator& best = bestItFor(side);
        const bool was_best = level_it == best;
        if (was_best) {
            side == Side::Bid ? ++stats_.best_bid_invalidations : ++stats_.best_ask_invalidations;
            best = std::next(level_it);
        }
        levels.erase(level_it);
        if (levels.empty()) {
            best = levels.end();
        }
    }

    LevelList bid_levels_; // sorted descending
    LevelList ask_levels_; // sorted ascending
    LevelIterator best_bid_ = bid_levels_.end();
    LevelIterator best_ask_ = ask_levels_.end();
    std::unordered_map<OrderId, OrderRef> order_refs_;
    L3BookStats stats_;
};

class L3ArrayScanOrderBook {
public:
    std::string name() const { return "L3ArrayBookScan"; }
    L3ArrayScanOrderBook() : bid_levels_(l3detail::priceRangeSize()), ask_levels_(l3detail::priceRangeSize()) {}

    void apply(const L3Message& msg) {
        ++stats_.messages;
        switch (msg.op_code) {
            case 'N': ++stats_.new_messages; applyNew(msg.side, msg.order_id, msg.price_tick, msg.quantity, true); break;
            case 'X': ++stats_.cancel_messages; cancel(msg.order_id); break;
            case 'M': ++stats_.modify_messages; modify(msg.order_id, msg.price_tick, msg.quantity); break;
            case 'T': ++stats_.trade_messages; ++stats_.ignored_trade_messages; break;
            default: break;
        }
    }
    std::optional<Level> bestBid() const { return best_bid_index_ < 0 ? std::nullopt : std::optional<Level>{Level{l3detail::indexToPrice(best_bid_index_), bid_levels_[best_bid_index_].total_qty}}; }
    std::optional<Level> bestAsk() const { return best_ask_index_ < 0 ? std::nullopt : std::optional<Level>{Level{l3detail::indexToPrice(best_ask_index_), ask_levels_[best_ask_index_].total_qty}}; }
    bool crossed() const { const auto b = bestBid(); const auto a = bestAsk(); return b && a && b->price >= a->price; }
    bool hasOrder(OrderId id) const { return order_refs_.find(id) != order_refs_.end(); }
    std::optional<L3OrderSnapshot> getOrder(OrderId id) const { return getOrderImpl(id); }
    L3BookStats stats() const { return statsWithActiveCounts(); }

private:
    using OrderIterator = std::list<l3detail::Order>::iterator;
    struct OrderRef { Side side{}; int index{}; OrderIterator order_it{}; };

    std::optional<L3OrderSnapshot> getOrderImpl(OrderId id) const {
        const auto it = order_refs_.find(id);
        if (it == order_refs_.end()) return std::nullopt;
        const auto& ref = it->second;
        return L3OrderSnapshot{ref.order_it->order_id, ref.side, l3detail::indexToPrice(ref.index), ref.order_it->qty};
    }

    L3BookStats statsWithActiveCounts() const {
        L3BookStats result = stats_;
        result.active_orders = order_refs_.size();
        for (const auto& x : bid_levels_) if (x.active()) ++result.active_bid_levels;
        for (const auto& x : ask_levels_) if (x.active()) ++result.active_ask_levels;
        return result;
    }

    std::vector<l3detail::PriceLevel>& levelsFor(Side side) { return side == Side::Bid ? bid_levels_ : ask_levels_; }
    const std::vector<l3detail::PriceLevel>& levelsFor(Side side) const { return side == Side::Bid ? bid_levels_ : ask_levels_; }
    int& bestIndexFor(Side side) { return side == Side::Bid ? best_bid_index_ : best_ask_index_; }
    const int& bestIndexFor(Side side) const { return side == Side::Bid ? best_bid_index_ : best_ask_index_; }

    bool wouldCross(Side side, Price price) const {
        const auto ask = bestAsk(); const auto bid = bestBid();
        return side == Side::Bid ? (ask && price >= ask->price) : (bid && price <= bid->price);
    }

    void applyNew(Side side, OrderId id, Price price, Qty qty, bool count_as_new) {
        if (qty <= 0 || id == 0) return;
        const int idx = l3detail::priceToIndex(price);
        if (!l3detail::validIndex(idx)) return;
        if (hasOrder(id)) cancel(id);
        const bool crossed_before = wouldCross(side, price);
        if (crossed_before) count_as_new ? ++stats_.crossing_new_orders : ++stats_.crossing_modify_orders;
        const Qty remaining = matchIncoming(side, price, qty);
        if (remaining > 0) insertResting(side, id, price, remaining);
    }

    Qty matchIncoming(Side side, Price limit, Qty qty) {
        Qty remaining = qty;
        const Side resting = side == Side::Bid ? Side::Ask : Side::Bid;
        while (remaining > 0 && bestIndexFor(resting) >= 0) {
            const Price best_price = l3detail::indexToPrice(bestIndexFor(resting));
            if (side == Side::Bid && best_price > limit) break;
            if (side == Side::Ask && best_price < limit) break;
            remaining = consumeLevel(resting, bestIndexFor(resting), remaining);
        }
        return remaining;
    }

    Qty consumeLevel(Side resting_side, int idx, Qty incoming_qty) {
        auto& level = levelsFor(resting_side)[idx];
        while (incoming_qty > 0 && !level.orders.empty()) {
            auto& resting = level.orders.front();
            const Qty traded = std::min(incoming_qty, resting.qty);
            incoming_qty -= traded; resting.qty -= traded; level.total_qty -= traded;
            ++stats_.match_events; stats_.traded_qty += static_cast<std::uint64_t>(traded);
            if (resting.qty == 0) { order_refs_.erase(resting.order_id); level.orders.pop_front(); }
        }
        if (!level.active()) clearLevel(resting_side, idx);
        return incoming_qty;
    }

    void insertResting(Side side, OrderId id, Price price, Qty qty) {
        const int idx = l3detail::priceToIndex(price);
        auto& level = levelsFor(side)[idx];
        level.total_qty += qty;
        level.orders.push_back(l3detail::Order{id, qty});
        order_refs_[id] = OrderRef{side, idx, std::prev(level.orders.end())};
        int& best = bestIndexFor(side);
        if (best < 0 || (side == Side::Bid ? idx > best : idx < best)) best = idx;
    }

    void cancel(OrderId id) { auto it = order_refs_.find(id); if (it != order_refs_.end()) eraseByRef(it); }
    void modify(OrderId id, Price price, Qty qty) { auto it = order_refs_.find(id); if (it == order_refs_.end()) return; const Side s = it->second.side; if (qty <= 0) { eraseByRef(it); return; } eraseByRef(it); applyNew(s, id, price, qty, false); }

    void eraseByRef(std::unordered_map<OrderId, OrderRef>::iterator it) {
        const auto ref = it->second;
        auto& level = levelsFor(ref.side)[ref.index];
        level.total_qty -= ref.order_it->qty;
        level.orders.erase(ref.order_it);
        order_refs_.erase(it);
        ++stats_.price_level_work_units;
        if (!level.active()) clearLevel(ref.side, ref.index);
    }

    void clearLevel(Side side, int idx) {
        auto& level = levelsFor(side)[idx];
        level.total_qty = 0;
        level.orders.clear();
        int& best = bestIndexFor(side);
        if (best == idx) {
            side == Side::Bid ? ++stats_.best_bid_invalidations : ++stats_.best_ask_invalidations;
            recoverBest(side);
        }
    }

    void recoverBest(Side side) {
        const auto& levels = levelsFor(side);
        if (side == Side::Bid) {
            for (int i = static_cast<int>(levels.size()) - 1; i >= 0; --i) { ++stats_.price_level_work_units; if (levels[i].active()) { best_bid_index_ = i; return; } }
            best_bid_index_ = -1;
        } else {
            for (int i = 0; i < static_cast<int>(levels.size()); ++i) { ++stats_.price_level_work_units; if (levels[i].active()) { best_ask_index_ = i; return; } }
            best_ask_index_ = -1;
        }
    }

    std::vector<l3detail::PriceLevel> bid_levels_;
    std::vector<l3detail::PriceLevel> ask_levels_;
    int best_bid_index_ = -1;
    int best_ask_index_ = -1;
    std::unordered_map<OrderId, OrderRef> order_refs_;
    L3BookStats stats_;
};

class L3ArrayFenwickBook {
public:
    std::string name() const { return "L3ArrayBookFenwick"; }
    L3ArrayFenwickBook()
        : bid_levels_(l3detail::priceRangeSize()), ask_levels_(l3detail::priceRangeSize()),
          bid_active_(l3detail::priceRangeSize()), ask_active_(l3detail::priceRangeSize()) {}

    void apply(const L3Message& msg) {
        ++stats_.messages;
        switch (msg.op_code) {
            case 'N': ++stats_.new_messages; applyNew(msg.side, msg.order_id, msg.price_tick, msg.quantity, true); break;
            case 'X': ++stats_.cancel_messages; cancel(msg.order_id); break;
            case 'M': ++stats_.modify_messages; modify(msg.order_id, msg.price_tick, msg.quantity); break;
            case 'T': ++stats_.trade_messages; ++stats_.ignored_trade_messages; break;
            default: break;
        }
    }
    std::optional<Level> bestBid() const { return best_bid_index_ < 0 ? std::nullopt : std::optional<Level>{Level{l3detail::indexToPrice(best_bid_index_), bid_levels_[best_bid_index_].total_qty}}; }
    std::optional<Level> bestAsk() const { return best_ask_index_ < 0 ? std::nullopt : std::optional<Level>{Level{l3detail::indexToPrice(best_ask_index_), ask_levels_[best_ask_index_].total_qty}}; }
    bool crossed() const { const auto b=bestBid(), a=bestAsk(); return b&&a&&b->price>=a->price; }
    bool hasOrder(OrderId id) const { return order_refs_.find(id)!=order_refs_.end(); }
    std::optional<L3OrderSnapshot> getOrder(OrderId id) const { const auto it=order_refs_.find(id); if(it==order_refs_.end()) return std::nullopt; const auto& r=it->second; return L3OrderSnapshot{r.order_it->order_id,r.side,l3detail::indexToPrice(r.index),r.order_it->qty}; }
    L3BookStats stats() const { L3BookStats r=stats_; r.active_orders=order_refs_.size(); r.active_bid_levels=bid_active_.sumAll(); r.active_ask_levels=ask_active_.sumAll(); return r; }

private:
    using OrderIterator = std::list<l3detail::Order>::iterator;
    struct OrderRef { Side side{}; int index{}; OrderIterator order_it{}; };
    std::vector<l3detail::PriceLevel>& levelsFor(Side s){return s==Side::Bid?bid_levels_:ask_levels_;}
    const std::vector<l3detail::PriceLevel>& levelsFor(Side s) const{return s==Side::Bid?bid_levels_:ask_levels_;}
    l3detail::FenwickTree& treeFor(Side s){return s==Side::Bid?bid_active_:ask_active_;}
    const l3detail::FenwickTree& treeFor(Side s) const{return s==Side::Bid?bid_active_:ask_active_;}
    int& bestIndexFor(Side s){return s==Side::Bid?best_bid_index_:best_ask_index_;}
    const int& bestIndexFor(Side s) const{return s==Side::Bid?best_bid_index_:best_ask_index_;}

    bool wouldCross(Side s, Price p) const{const auto a=bestAsk(), b=bestBid(); return s==Side::Bid?(a&&p>=a->price):(b&&p<=b->price);}    
    void applyNew(Side s, OrderId id, Price p, Qty q, bool count){ if(q<=0||id==0) return; int idx=l3detail::priceToIndex(p); if(!l3detail::validIndex(idx)) return; if(hasOrder(id)) cancel(id); bool cr=wouldCross(s,p); if(cr) count?++stats_.crossing_new_orders:++stats_.crossing_modify_orders; Qty rem=matchIncoming(s,p,q); if(rem>0) insertResting(s,id,p,rem); }
    Qty matchIncoming(Side s, Price limit, Qty q){ Qty rem=q; Side r=s==Side::Bid?Side::Ask:Side::Bid; while(rem>0&&bestIndexFor(r)>=0){ Price bp=l3detail::indexToPrice(bestIndexFor(r)); if(s==Side::Bid&&bp>limit) break; if(s==Side::Ask&&bp<limit) break; rem=consumeLevel(r,bestIndexFor(r),rem);} return rem; }
    Qty consumeLevel(Side s,int idx,Qty q){auto& level=levelsFor(s)[idx]; while(q>0&&!level.orders.empty()){auto& o=level.orders.front(); Qty t=std::min(q,o.qty); q-=t;o.qty-=t;level.total_qty-=t;++stats_.match_events;stats_.traded_qty+=static_cast<std::uint64_t>(t); if(o.qty==0){order_refs_.erase(o.order_id);level.orders.pop_front();}} if(!level.active()) clearLevel(s,idx); return q;}
    void insertResting(Side s,OrderId id,Price p,Qty q){int idx=l3detail::priceToIndex(p); auto& level=levelsFor(s)[idx]; bool was_active=level.active(); level.total_qty+=q; level.orders.push_back(l3detail::Order{id,q}); order_refs_[id]=OrderRef{s,idx,std::prev(level.orders.end())}; if(!was_active) treeFor(s).add(idx,1); int& best=bestIndexFor(s); if(best<0||(s==Side::Bid?idx>best:idx<best)) best=idx;}
    void cancel(OrderId id){auto it=order_refs_.find(id); if(it!=order_refs_.end()) eraseByRef(it);}    
    void modify(OrderId id,Price p,Qty q){auto it=order_refs_.find(id); if(it==order_refs_.end()) return; Side s=it->second.side; if(q<=0){eraseByRef(it);return;} eraseByRef(it); applyNew(s,id,p,q,false);}    
    void eraseByRef(std::unordered_map<OrderId,OrderRef>::iterator it){auto ref=it->second; auto& level=levelsFor(ref.side)[ref.index]; level.total_qty-=ref.order_it->qty; level.orders.erase(ref.order_it); order_refs_.erase(it); ++stats_.price_level_work_units; if(!level.active()) clearLevel(ref.side,ref.index);}    
    void clearLevel(Side s,int idx){auto& level=levelsFor(s)[idx]; level.total_qty=0; level.orders.clear(); treeFor(s).add(idx,-1); int& best=bestIndexFor(s); if(best==idx){s==Side::Bid?++stats_.best_bid_invalidations:++stats_.best_ask_invalidations; recoverBest(s);}}
    void recoverBest(Side s){ const int count=treeFor(s).sumAll(); if(count==0){bestIndexFor(s)=-1; return;} std::uint64_t work=0; const int order=s==Side::Bid?count:1; bestIndexFor(s)=treeFor(s).findByOrder(order,&work); stats_.price_level_work_units+=work;}

    std::vector<l3detail::PriceLevel> bid_levels_, ask_levels_;
    l3detail::FenwickTree bid_active_, ask_active_;
    int best_bid_index_=-1, best_ask_index_=-1;
    std::unordered_map<OrderId,OrderRef> order_refs_;
    L3BookStats stats_;
};

class L3ArrayBitsetBook {
public:
    std::string name() const { return "L3ArrayBookBitset"; }
    L3ArrayBitsetBook() : bid_levels_(l3detail::priceRangeSize()), ask_levels_(l3detail::priceRangeSize()),
                          bid_words_(wordCount(),0), ask_words_(wordCount(),0) {}
    void apply(const L3Message& msg){++stats_.messages; switch(msg.op_code){case 'N':++stats_.new_messages;applyNew(msg.side,msg.order_id,msg.price_tick,msg.quantity,true);break;case 'X':++stats_.cancel_messages;cancel(msg.order_id);break;case 'M':++stats_.modify_messages;modify(msg.order_id,msg.price_tick,msg.quantity);break;case 'T':++stats_.trade_messages;++stats_.ignored_trade_messages;break;default:break;}}
    std::optional<Level> bestBid() const{return best_bid_index_<0?std::nullopt:std::optional<Level>{Level{l3detail::indexToPrice(best_bid_index_),bid_levels_[best_bid_index_].total_qty}};}
    std::optional<Level> bestAsk() const{return best_ask_index_<0?std::nullopt:std::optional<Level>{Level{l3detail::indexToPrice(best_ask_index_),ask_levels_[best_ask_index_].total_qty}};}
    bool crossed() const{auto b=bestBid(),a=bestAsk();return b&&a&&b->price>=a->price;}
    bool hasOrder(OrderId id) const{return order_refs_.find(id)!=order_refs_.end();}
    std::optional<L3OrderSnapshot> getOrder(OrderId id) const{auto it=order_refs_.find(id); if(it==order_refs_.end())return std::nullopt; auto& r=it->second; return L3OrderSnapshot{r.order_it->order_id,r.side,l3detail::indexToPrice(r.index),r.order_it->qty};}
    L3BookStats stats() const{L3BookStats r=stats_; r.active_orders=order_refs_.size(); r.active_bid_levels=countBits(bid_words_); r.active_ask_levels=countBits(ask_words_); return r;}
private:
    using OrderIterator=std::list<l3detail::Order>::iterator; struct OrderRef{Side side{};int index{};OrderIterator order_it{};};
    static std::size_t wordCount(){return static_cast<std::size_t>((l3detail::priceRangeSize()+63)/64);}    
    static std::uint64_t maskFor(int idx){return 1ULL<<(idx%64);}    
    static int countBits(const std::vector<std::uint64_t>& words){int total=0; for(auto w:words) total+=std::popcount(w); return total;}    
    std::vector<l3detail::PriceLevel>& levelsFor(Side s){return s==Side::Bid?bid_levels_:ask_levels_;}
    const std::vector<l3detail::PriceLevel>& levelsFor(Side s) const{return s==Side::Bid?bid_levels_:ask_levels_;}
    std::vector<std::uint64_t>& wordsFor(Side s){return s==Side::Bid?bid_words_:ask_words_;}
    const std::vector<std::uint64_t>& wordsFor(Side s) const{return s==Side::Bid?bid_words_:ask_words_;}
    int& bestIndexFor(Side s){return s==Side::Bid?best_bid_index_:best_ask_index_;}
    const int& bestIndexFor(Side s) const{return s==Side::Bid?best_bid_index_:best_ask_index_;}
    void setActive(Side s,int idx,bool active){auto& words=wordsFor(s); auto& word=words[static_cast<std::size_t>(idx/64)]; if(active) word|=maskFor(idx); else word&=~maskFor(idx);}    
    bool wouldCross(Side s,Price p)const{auto a=bestAsk(),b=bestBid();return s==Side::Bid?(a&&p>=a->price):(b&&p<=b->price);}    
    void applyNew(Side s,OrderId id,Price p,Qty q,bool count){if(q<=0||id==0)return;int idx=l3detail::priceToIndex(p);if(!l3detail::validIndex(idx))return;if(hasOrder(id))cancel(id);bool cr=wouldCross(s,p);if(cr)count?++stats_.crossing_new_orders:++stats_.crossing_modify_orders;Qty rem=matchIncoming(s,p,q);if(rem>0)insertResting(s,id,p,rem);}    
    Qty matchIncoming(Side s,Price limit,Qty q){Qty rem=q;Side r=s==Side::Bid?Side::Ask:Side::Bid;while(rem>0&&bestIndexFor(r)>=0){Price bp=l3detail::indexToPrice(bestIndexFor(r));if(s==Side::Bid&&bp>limit)break;if(s==Side::Ask&&bp<limit)break;rem=consumeLevel(r,bestIndexFor(r),rem);}return rem;}    
    Qty consumeLevel(Side s,int idx,Qty q){auto& level=levelsFor(s)[idx];while(q>0&&!level.orders.empty()){auto& o=level.orders.front();Qty t=std::min(q,o.qty);q-=t;o.qty-=t;level.total_qty-=t;++stats_.match_events;stats_.traded_qty+=static_cast<std::uint64_t>(t);if(o.qty==0){order_refs_.erase(o.order_id);level.orders.pop_front();}}if(!level.active())clearLevel(s,idx);return q;}    
    void insertResting(Side s,OrderId id,Price p,Qty q){int idx=l3detail::priceToIndex(p);auto& level=levelsFor(s)[idx];bool was=level.active();level.total_qty+=q;level.orders.push_back(l3detail::Order{id,q});order_refs_[id]=OrderRef{s,idx,std::prev(level.orders.end())};if(!was)setActive(s,idx,true);int& best=bestIndexFor(s);if(best<0||(s==Side::Bid?idx>best:idx<best))best=idx;}    
    void cancel(OrderId id){auto it=order_refs_.find(id);if(it!=order_refs_.end())eraseByRef(it);}    
    void modify(OrderId id,Price p,Qty q){auto it=order_refs_.find(id);if(it==order_refs_.end())return;Side s=it->second.side;if(q<=0){eraseByRef(it);return;}eraseByRef(it);applyNew(s,id,p,q,false);}    
    void eraseByRef(std::unordered_map<OrderId,OrderRef>::iterator it){auto ref=it->second;auto& level=levelsFor(ref.side)[ref.index];level.total_qty-=ref.order_it->qty;level.orders.erase(ref.order_it);order_refs_.erase(it);++stats_.price_level_work_units;if(!level.active())clearLevel(ref.side,ref.index);}    
    void clearLevel(Side s,int idx){auto& level=levelsFor(s)[idx];level.total_qty=0;level.orders.clear();setActive(s,idx,false);int& best=bestIndexFor(s);if(best==idx){s==Side::Bid?++stats_.best_bid_invalidations:++stats_.best_ask_invalidations;recoverBest(s);}}    
    void recoverBest(Side s){const auto& words=wordsFor(s);if(s==Side::Bid){for(int w=static_cast<int>(words.size())-1;w>=0;--w){++stats_.price_level_work_units;std::uint64_t word=words[static_cast<std::size_t>(w)];if(word){int bit=63-std::countl_zero(word);int idx=w*64+bit;if(l3detail::validIndex(idx)){best_bid_index_=idx;return;}}}best_bid_index_=-1;}else{for(std::size_t w=0;w<words.size();++w){++stats_.price_level_work_units;std::uint64_t word=words[w];if(word){int bit=std::countr_zero(word);int idx=static_cast<int>(w*64+bit);if(l3detail::validIndex(idx)){best_ask_index_=idx;return;}}}best_ask_index_=-1;}}
    std::vector<l3detail::PriceLevel> bid_levels_,ask_levels_;std::vector<std::uint64_t> bid_words_,ask_words_;int best_bid_index_=-1,best_ask_index_=-1;std::unordered_map<OrderId,OrderRef> order_refs_;L3BookStats stats_;
};

using L3OrderBook = L3MapOrderBook;
using L3ArrayFenwickOrderBook = L3ArrayFenwickBook;
using L3ArrayBitsetOrderBook = L3ArrayBitsetBook;
