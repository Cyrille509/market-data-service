// Latency and throughput use separate passes; see docs/benchmarks.md.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "mdp/order_book.hpp"
#include "mdp/sync_engine.hpp"

using mdp::DepthFrame;
using mdp::Level;
using mdp::OrderBook;
using mdp::Snapshot;
using mdp::SyncEngine;

namespace {

using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;

struct Rng {
    std::uint64_t s;
    std::uint64_t next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 0x2545F4914F6CDD1DULL;
    }
    std::uint64_t below(std::uint64_t n) { return next() % n; }
};

struct Stats {
    std::int64_t p50 = 0;
    std::int64_t p95 = 0;
    std::int64_t p99 = 0;
    std::int64_t p999 = 0;
    double mean = 0.0;
    std::int64_t max = 0;
    std::size_t count = 0;
};

struct Result {
    Stats latency;
    double seconds = 0.0;
    std::uint64_t frames = 0;
    std::uint64_t updates = 0;
    std::uint64_t fingerprint = 0;
};

struct MutationMix {
    std::uint64_t adds = 0;
    std::uint64_t modifies = 0;
    std::uint64_t deletes = 0;
    std::uint64_t absent_delete_noops = 0;
};

std::string price_str(std::int64_t cents) {
    return std::to_string(cents / 100) + "." + (cents % 100 < 10 ? "0" : "") +
           std::to_string(cents % 100);
}

std::string make_snapshot_json(int depth, std::int64_t last_update_id) {
    std::string bids;
    std::string asks;
    bids.reserve(static_cast<std::size_t>(depth) * 32);
    asks.reserve(static_cast<std::size_t>(depth) * 32);
    for (int i = 0; i < depth; ++i) {
        if (i) {
            bids += ',';
            asks += ',';
        }
        bids += "[\"" + price_str(10450000 - 100 - i) + "\",\"1.00\"]";
        asks += "[\"" + price_str(10450000 + 100 + i) + "\",\"1.00\"]";
    }
    return "{\"lastUpdateId\":" + std::to_string(last_update_id) + ",\"bids\":[" + bids +
           "],\"asks\":[" + asks + "]}";
}

std::vector<std::string> make_frames(std::uint64_t count, int levels_per_side, int depth,
                                     std::int64_t start_u) {
    Rng rng{0x9E3779B97F4A7C15ULL};
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(count));
    std::int64_t u = start_u;
    for (std::uint64_t n = 0; n < count; ++n) {
        const std::int64_t first = u + 1;
        const std::int64_t last = first + static_cast<std::int64_t>(2 * levels_per_side) - 1;
        std::string bids;
        std::string asks;
        bids.reserve(static_cast<std::size_t>(levels_per_side) * 32);
        asks.reserve(static_cast<std::size_t>(levels_per_side) * 32);
        for (int i = 0; i < levels_per_side; ++i) {
            if (i) {
                bids += ',';
                asks += ',';
            }
            const auto bid_offset = static_cast<std::int64_t>(rng.below(depth));
            const auto ask_offset = static_cast<std::int64_t>(rng.below(depth));
            const bool delete_bid = rng.below(10) == 0;
            const bool delete_ask = rng.below(10) == 0;
            const std::string bid_qty =
                delete_bid ? "0" : "0." + std::to_string(10 + rng.below(89)) + "0";
            const std::string ask_qty =
                delete_ask ? "0" : "0." + std::to_string(10 + rng.below(89)) + "0";
            bids += "[\"" + price_str(10450000 - 100 - bid_offset) + "\",\"" + bid_qty +
                    "\"]";
            asks += "[\"" + price_str(10450000 + 100 + ask_offset) + "\",\"" + ask_qty +
                    "\"]";
        }
        out.push_back("{\"e\":\"depthUpdate\",\"E\":" + std::to_string(n) +
                      ",\"s\":\"BTCUSDT\",\"U\":" + std::to_string(first) +
                      ",\"u\":" + std::to_string(last) + ",\"b\":[" + bids +
                      "],\"a\":[" + asks + "]}");
        u = last;
    }
    return out;
}

void hash_value(std::uint64_t value, std::uint64_t& hash) {
    constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
    for (int shift = 0; shift < 64; shift += 8) {
        hash ^= (value >> shift) & 0xffULL;
        hash *= kFnvPrime;
    }
}

std::uint64_t book_fingerprint(const OrderBook& book) {
    std::uint64_t hash = 14695981039346656037ULL;
    hash_value(0x42494453ULL, hash);
    for (const Level& level : book.top_bids(book.bid_depth())) {
        hash_value(static_cast<std::uint64_t>(level.price), hash);
        hash_value(static_cast<std::uint64_t>(level.qty), hash);
    }
    hash_value(0x41534B53ULL, hash);
    for (const Level& level : book.top_asks(book.ask_depth())) {
        hash_value(static_cast<std::uint64_t>(level.price), hash);
        hash_value(static_cast<std::uint64_t>(level.qty), hash);
    }
    return hash;
}

std::int64_t ns_between(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration_cast<Nanoseconds>(end - begin).count();
}

Stats summarize(const std::vector<std::int64_t>& samples) {
    Stats out;
    out.count = samples.size();
    if (samples.empty()) return out;
    std::vector<std::int64_t> sorted(samples);
    std::sort(sorted.begin(), sorted.end());
    auto percentile = [&](double q) {
        const auto rank = static_cast<std::size_t>(
            std::ceil(q * static_cast<double>(sorted.size())));
        return sorted[std::max<std::size_t>(1, rank) - 1];
    };
    long double sum = 0.0;
    for (std::int64_t sample : samples) sum += sample;
    out.p50 = percentile(0.50);
    out.p95 = percentile(0.95);
    out.p99 = percentile(0.99);
    out.p999 = percentile(0.999);
    out.mean = static_cast<double>(sum / samples.size());
    out.max = sorted.back();
    return out;
}

void print_ns(const char* name, double ns) {
    std::printf("  %-8s %12.1f ns  (%9.3f us)\n", name, ns, ns / 1000.0);
}

void print_stats(const Stats& stats) {
    print_ns("p50", static_cast<double>(stats.p50));
    print_ns("p95", static_cast<double>(stats.p95));
    print_ns("p99", static_cast<double>(stats.p99));
    if (stats.count >= 10000) print_ns("p99.9", static_cast<double>(stats.p999));
    print_ns("mean", stats.mean);
    print_ns("max", static_cast<double>(stats.max));
    std::printf("  %-8s %12llu\n", "samples",
                static_cast<unsigned long long>(stats.count));
}

void print_rates(const Result& result) {
    std::printf("  elapsed:              %.6f s\n", result.seconds);
    std::printf("  frames/sec:           %.0f\n",
                static_cast<double>(result.frames) / result.seconds);
    std::printf("  level updates/sec:    %.0f\n",
                static_cast<double>(result.updates) / result.seconds);
}

bool parse_all(const std::vector<std::string>& lines, std::vector<DepthFrame>& frames,
               std::string& err) {
    frames.reserve(lines.size());
    for (const std::string& line : lines) {
        DepthFrame frame;
        if (!mdp::parse_depth_frame(line, frame, err)) return false;
        frames.push_back(std::move(frame));
    }
    return true;
}

void load_snapshot(const Snapshot& snapshot, OrderBook& book) {
    for (const Level& level : snapshot.bids) book.set_bid(level.price, level.qty);
    for (const Level& level : snapshot.asks) book.set_ask(level.price, level.qty);
}

void apply_direct(OrderBook& book, const DepthFrame& frame) {
    for (const Level& level : frame.bids) book.set_bid(level.price, level.qty);
    for (const Level& level : frame.asks) book.set_ask(level.price, level.qty);
}

MutationMix classify_mix(const Snapshot& snapshot, const std::vector<DepthFrame>& frames,
                         std::size_t measured_begin) {
    std::unordered_set<std::int64_t> bids;
    std::unordered_set<std::int64_t> asks;
    for (const Level& level : snapshot.bids) bids.insert(level.price);
    for (const Level& level : snapshot.asks) asks.insert(level.price);
    MutationMix mix;
    auto visit = [&](const Level& level, std::unordered_set<std::int64_t>& side,
                     bool measured) {
        const bool present = side.find(level.price) != side.end();
        if (level.qty == 0) {
            if (measured) {
                present ? ++mix.deletes : ++mix.absent_delete_noops;
            }
            side.erase(level.price);
        } else {
            if (measured) present ? ++mix.modifies : ++mix.adds;
            side.insert(level.price);
        }
    };
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const bool measured = i >= measured_begin;
        for (const Level& level : frames[i].bids) visit(level, bids, measured);
        for (const Level& level : frames[i].asks) visit(level, asks, measured);
    }
    return mix;
}

Result run_book_micro(const Snapshot& snapshot, const std::vector<DepthFrame>& frames,
                      std::size_t measured_begin, std::size_t measured_updates) {
    OrderBook book;
    load_snapshot(snapshot, book);
    for (std::size_t i = 0; i < measured_begin; ++i) apply_direct(book, frames[i]);
    std::vector<std::int64_t> samples(measured_updates);
    std::size_t sample = 0;
    const auto pass_begin = Clock::now();
    for (std::size_t i = measured_begin; i < frames.size(); ++i) {
        for (const Level& level : frames[i].bids) {
            const auto begin = Clock::now();
            book.set_bid(level.price, level.qty);
            const auto end = Clock::now();
            samples[sample++] = ns_between(begin, end);
        }
        for (const Level& level : frames[i].asks) {
            const auto begin = Clock::now();
            book.set_ask(level.price, level.qty);
            const auto end = Clock::now();
            samples[sample++] = ns_between(begin, end);
        }
    }
    const auto pass_end = Clock::now();
    Result out;
    out.seconds = std::chrono::duration<double>(pass_end - pass_begin).count();
    out.frames = frames.size() - measured_begin;
    out.updates = sample;
    out.fingerprint = book_fingerprint(book);
    out.latency = summarize(samples);
    return out;
}

bool warm_engine(SyncEngine& engine, const std::vector<std::string>& lines,
                 std::size_t warmup, std::string& err) {
    for (std::size_t i = 0; i < warmup; ++i) {
        DepthFrame frame;
        if (!mdp::parse_depth_frame(lines[i], frame, err)) return false;
        engine.on_frame(frame);
    }
    return true;
}

bool validate(const SyncEngine& engine, const OrderBook& book, std::uint64_t total_frames,
              std::uint64_t total_updates, std::int64_t final_u, std::string& err) {
    if (engine.state() != SyncEngine::State::Live || engine.gap_count() != 0 ||
        engine.applied_frames() != total_frames ||
        engine.applied_level_updates() != total_updates ||
        engine.last_update_id() != final_u || book.crossed()) {
        err = "engine counters or final book state did not match the generated stream";
        return false;
    }
    return true;
}

bool run_e2e_latency(const std::string& snapshot_json,
                     const std::vector<std::string>& lines, std::size_t warmup,
                     std::uint64_t updates_per_frame, std::int64_t final_u, Result& out,
                     std::string& err) {
    OrderBook book;
    SyncEngine engine(book, [&](Snapshot& snapshot, std::string& snapshot_err) {
        return mdp::parse_snapshot(snapshot_json, snapshot, snapshot_err);
    });
    if (!warm_engine(engine, lines, warmup, err)) return false;
    const auto frames_before = engine.applied_frames();
    const auto updates_before = engine.applied_level_updates();
    std::vector<std::int64_t> samples(lines.size() - warmup);
    const auto pass_begin = Clock::now();
    for (std::size_t i = warmup; i < lines.size(); ++i) {
        DepthFrame frame;
        const auto begin = Clock::now();
        if (!mdp::parse_depth_frame(lines[i], frame, err)) return false;
        engine.on_frame(frame);
        const auto end = Clock::now();
        samples[i - warmup] = ns_between(begin, end);
    }
    const auto pass_end = Clock::now();
    const auto total_frames = static_cast<std::uint64_t>(lines.size());
    if (!validate(engine, book, total_frames, total_frames * updates_per_frame, final_u, err))
        return false;
    out.seconds = std::chrono::duration<double>(pass_end - pass_begin).count();
    out.frames = engine.applied_frames() - frames_before;
    out.updates = engine.applied_level_updates() - updates_before;
    out.fingerprint = book_fingerprint(book);
    out.latency = summarize(samples);
    return true;
}

bool run_throughput(const std::string& snapshot_json, const std::vector<std::string>& lines,
                    std::size_t warmup, std::uint64_t updates_per_frame,
                    std::int64_t final_u, Result& out, std::string& err) {
    OrderBook book;
    SyncEngine engine(book, [&](Snapshot& snapshot, std::string& snapshot_err) {
        return mdp::parse_snapshot(snapshot_json, snapshot, snapshot_err);
    });
    if (!warm_engine(engine, lines, warmup, err)) return false;
    const auto frames_before = engine.applied_frames();
    const auto updates_before = engine.applied_level_updates();
    const auto begin = Clock::now();
    for (std::size_t i = warmup; i < lines.size(); ++i) {
        DepthFrame frame;
        if (!mdp::parse_depth_frame(lines[i], frame, err)) return false;
        engine.on_frame(frame);
    }
    const auto end = Clock::now();
    const auto total_frames = static_cast<std::uint64_t>(lines.size());
    if (!validate(engine, book, total_frames, total_frames * updates_per_frame, final_u, err))
        return false;
    out.seconds = std::chrono::duration<double>(end - begin).count();
    out.frames = engine.applied_frames() - frames_before;
    out.updates = engine.applied_level_updates() - updates_before;
    out.fingerprint = book_fingerprint(book);
    return true;
}

Stats clock_pair_floor(std::size_t count) {
    std::vector<std::int64_t> samples(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto begin = Clock::now();
        const auto end = Clock::now();
        samples[i] = ns_between(begin, end);
    }
    return summarize(samples);
}

}

int main(int argc, char** argv) {
    const std::uint64_t measured_frames =
        argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 200000;
    const int levels_per_side = argc > 2 ? std::atoi(argv[2]) : 5;
    const std::uint64_t warmup_frames =
        argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 20000;
    constexpr int kDepth = 500;
    constexpr std::int64_t kSnapshotId = 1000;
    if (measured_frames == 0 || warmup_frames == 0 || levels_per_side <= 0 ||
        levels_per_side > 100 ||
        measured_frames > std::numeric_limits<std::size_t>::max() - warmup_frames) {
        std::fprintf(stderr, "usage: replay_bench [frames>0] [levels/side=1..100] [warmup>0]\n");
        return 2;
    }

    const std::uint64_t total_frames = warmup_frames + measured_frames;
    const std::uint64_t updates_per_frame = static_cast<std::uint64_t>(2 * levels_per_side);
    const std::uint64_t measured_updates = measured_frames * updates_per_frame;
    const std::int64_t final_u =
        kSnapshotId + static_cast<std::int64_t>(total_frames * updates_per_frame);

    std::printf("synthetic deterministic replay workload\n");
    std::printf("  measured frames:      %llu\n",
                static_cast<unsigned long long>(measured_frames));
    std::printf("  warm-up frames:       %llu (excluded)\n",
                static_cast<unsigned long long>(warmup_frames));
    std::printf("  updates/frame:        %llu (%d bids + %d asks)\n",
                static_cast<unsigned long long>(updates_per_frame), levels_per_side,
                levels_per_side);
    std::printf("  initial depth:        %d bids / %d asks\n", kDepth, kDepth);
    std::printf("  trades:               unsupported by this L2 implementation\n");
    std::printf("generating and pre-decoding untimed inputs...\n");

    const std::string snapshot_json = make_snapshot_json(kDepth, kSnapshotId);
    const std::vector<std::string> lines =
        make_frames(total_frames, levels_per_side, kDepth, kSnapshotId);
    Snapshot snapshot;
    std::string err;
    if (!mdp::parse_snapshot(snapshot_json, snapshot, err)) {
        std::fprintf(stderr, "generated snapshot failed: %s\n", err.c_str());
        return 1;
    }
    std::vector<DepthFrame> decoded;
    if (!parse_all(lines, decoded, err)) {
        std::fprintf(stderr, "generated frame failed: %s\n", err.c_str());
        return 1;
    }
    const MutationMix mix =
        classify_mix(snapshot, decoded, static_cast<std::size_t>(warmup_frames));
    std::printf("  operation mix:        adds=%llu modifies=%llu deletes=%llu no-op deletes=%llu\n",
                static_cast<unsigned long long>(mix.adds),
                static_cast<unsigned long long>(mix.modifies),
                static_cast<unsigned long long>(mix.deletes),
                static_cast<unsigned long long>(mix.absent_delete_noops));

    std::printf("\nsteady_clock back-to-back call-pair floor (not subtracted)\n");
    print_stats(clock_pair_floor(static_cast<std::size_t>(
        std::min<std::uint64_t>(measured_frames, 200000))));

    std::printf("\ndirect OrderBook mutation latency -- COMPONENT MICROBENCHMARK\n");
    std::printf("  boundary: decoded Level -> one std::map set/erase returns\n");
    const Result book = run_book_micro(snapshot, decoded,
                                       static_cast<std::size_t>(warmup_frames),
                                       static_cast<std::size_t>(measured_updates));
    print_stats(book.latency);
    std::printf("  instrumented pass rates (include per-update clock calls):\n");
    print_rates(book);

    std::printf("\nPRIMARY: end-to-end processing service latency\n");
    std::printf("  boundary: raw JSON -> parse -> sequence validation -> all L2 mutations complete\n");
    std::printf("  sample unit: one frame with %llu levels; not divided by batch size\n",
                static_cast<unsigned long long>(updates_per_frame));
    Result latency;
    if (!run_e2e_latency(snapshot_json, lines, static_cast<std::size_t>(warmup_frames),
                         updates_per_frame, final_u, latency, err)) {
        std::fprintf(stderr, "latency pass failed: %s\n", err.c_str());
        return 1;
    }
    print_stats(latency.latency);
    std::printf("  instrumented pass rates (include per-frame clock calls):\n");
    print_rates(latency);

    std::printf("\nuninstrumented end-to-end throughput -- SEPARATE PASS\n");
    Result throughput;
    if (!run_throughput(snapshot_json, lines, static_cast<std::size_t>(warmup_frames),
                        updates_per_frame, final_u, throughput, err)) {
        std::fprintf(stderr, "throughput pass failed: %s\n", err.c_str());
        return 1;
    }
    print_rates(throughput);

    if (book.updates != measured_updates || latency.frames != measured_frames ||
        latency.updates != measured_updates || throughput.frames != measured_frames ||
        throughput.updates != measured_updates || book.fingerprint != latency.fingerprint ||
        latency.fingerprint != throughput.fingerprint) {
        std::fprintf(stderr, "benchmark passes produced inconsistent results\n");
        return 1;
    }
    constexpr std::uint64_t kDefaultFingerprint = 0x5E2ED2B33C4AA494ULL;
    if (measured_frames == 200000 && levels_per_side == 5 && warmup_frames == 20000 &&
        throughput.fingerprint != kDefaultFingerprint) {
        std::fprintf(stderr, "benchmark default fingerprint did not match the golden value\n");
        return 1;
    }
    std::printf("\nverification\n");
    std::printf("  final last_u:         %lld\n", static_cast<long long>(final_u));
    std::printf("  book fingerprint:     %016llx (identical across all passes)\n",
                static_cast<unsigned long long>(throughput.fingerprint));
    std::printf("  sequence gaps:        0\n");
    return 0;
}
