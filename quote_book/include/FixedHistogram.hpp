#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

class FixedHistogram {
public:
    static constexpr std::uint64_t kInfinity = std::numeric_limits<std::uint64_t>::max();

    explicit FixedHistogram(std::vector<std::uint64_t> bucket_upper_bounds_ns)
        : bucket_upper_bounds_ns_(std::move(bucket_upper_bounds_ns)),
          buckets_(bucket_upper_bounds_ns_.size(), 0) {
        validateBucketBounds(bucket_upper_bounds_ns_);
    }

    // Backward-compatible constructor for callers that still want fixed-width buckets.
    FixedHistogram(std::uint64_t bucket_width_ns, std::uint64_t max_latency_ns)
        : FixedHistogram(makeFixedWidthBounds(bucket_width_ns, max_latency_ns)) {}

    void record(std::uint64_t latency_ns) {
        const std::size_t idx = bucketIndex(latency_ns);
        ++buckets_[idx];
        ++total_count_;
    }

    void clear() {
        std::fill(buckets_.begin(), buckets_.end(), 0);
        total_count_ = 0;
    }

    void addBucketCount(std::size_t bucket_index, std::uint64_t count) {
        if (bucket_index >= buckets_.size() || count == 0) {
            return;
        }
        buckets_[bucket_index] += count;
        total_count_ += count;
    }

    std::uint64_t percentile(double percentile_value) const {
        if (total_count_ == 0) {
            return 0;
        }
        return bucketUpperBoundNs(percentileBucketIndex(percentile_value));
    }

    std::size_t percentileBucketIndex(double percentile_value) const {
        if (total_count_ == 0) {
            return 0;
        }

        const auto threshold = static_cast<std::uint64_t>(
            (percentile_value / 100.0) * static_cast<double>(total_count_) + 0.999999);

        std::uint64_t running = 0;
        for (std::size_t i = 0; i < buckets_.size(); ++i) {
            running += buckets_[i];
            if (running >= threshold) {
                return i;
            }
        }

        return buckets_.empty() ? 0 : buckets_.size() - 1;
    }

    std::uint64_t count() const {
        return total_count_;
    }

    const std::vector<std::uint64_t>& buckets() const {
        return buckets_;
    }

    const std::vector<std::uint64_t>& bucketUpperBoundsNs() const {
        return bucket_upper_bounds_ns_;
    }

    std::uint64_t bucketUpperBoundNs(std::size_t bucket_index) const {
        return bucket_upper_bounds_ns_.at(bucket_index);
    }

    std::string bucketLabel(std::size_t bucket_index) const {
        if (bucket_upper_bounds_ns_.empty()) {
            return "NA";
        }

        const std::uint64_t lower = bucket_index == 0 ? 0 : bucket_upper_bounds_ns_.at(bucket_index - 1);
        const std::uint64_t upper = bucket_upper_bounds_ns_.at(bucket_index);

        std::ostringstream out;
        if (upper == kInfinity) {
            out << '>' << lower << "ns";
        } else if (bucket_index == 0) {
            out << "0-" << upper << "ns";
        } else {
            out << lower << '-' << upper << "ns";
        }
        return out.str();
    }

    std::string percentileLabel(double percentile_value) const {
        if (total_count_ == 0) {
            return "NA";
        }
        return bucketLabel(percentileBucketIndex(percentile_value));
    }

    static std::vector<std::uint64_t> fineLatencyBounds() {
        // Fine-grained buckets for parse/book-update/best-call measurements.
        // The first bucket starts at 0-20ns, but values in the very first buckets
        // should still be interpreted as "near the measurement floor", not as
        // exact per-operation timing.
        return {20, 30, 40, 80, 120, 160, 200, 240, 280, 320,
                400, 500, 1'000, 2'000, kInfinity};
    }

    static std::vector<std::uint64_t> wireLatencyBounds() {
        // Wire latency includes loopback/kernel/scheduling effects, so it needs wider buckets.
        return {1'000, 2'500, 5'000, 10'000, 25'000, 50'000,
                100'000, 250'000, 1'000'000, kInfinity};
    }

    static std::string describeBounds(const std::vector<std::uint64_t>& bounds) {
        FixedHistogram tmp(bounds);
        std::ostringstream out;
        for (std::size_t i = 0; i < bounds.size(); ++i) {
            if (i != 0) {
                out << ", ";
            }
            out << tmp.bucketLabel(i);
        }
        return out.str();
    }

private:
    static void validateBucketBounds(const std::vector<std::uint64_t>& bounds) {
        if (bounds.empty()) {
            throw std::invalid_argument("histogram must have at least one bucket");
        }
        for (std::size_t i = 1; i < bounds.size(); ++i) {
            if (bounds[i] <= bounds[i - 1]) {
                throw std::invalid_argument("histogram bucket upper bounds must be strictly increasing");
            }
        }
        if (bounds.back() != kInfinity) {
            throw std::invalid_argument("last histogram bucket must be an overflow bucket with kInfinity upper bound");
        }
    }

    static std::vector<std::uint64_t> makeFixedWidthBounds(std::uint64_t bucket_width_ns,
                                                          std::uint64_t max_latency_ns) {
        if (bucket_width_ns == 0 || max_latency_ns == 0 || bucket_width_ns > max_latency_ns) {
            throw std::invalid_argument("invalid fixed-width histogram configuration");
        }

        std::vector<std::uint64_t> bounds;
        for (std::uint64_t bound = bucket_width_ns; bound <= max_latency_ns; bound += bucket_width_ns) {
            bounds.push_back(bound);
            if (max_latency_ns - bound < bucket_width_ns) {
                break;
            }
        }
        bounds.push_back(kInfinity);
        return bounds;
    }

    std::size_t bucketIndex(std::uint64_t latency_ns) const {
        const auto it = std::lower_bound(bucket_upper_bounds_ns_.begin(),
                                         bucket_upper_bounds_ns_.end(),
                                         latency_ns);
        if (it == bucket_upper_bounds_ns_.end()) {
            return bucket_upper_bounds_ns_.size() - 1;
        }
        return static_cast<std::size_t>(it - bucket_upper_bounds_ns_.begin());
    }

    std::vector<std::uint64_t> bucket_upper_bounds_ns_;
    std::vector<std::uint64_t> buckets_;
    std::uint64_t total_count_{};
};
