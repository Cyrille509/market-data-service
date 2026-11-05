#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "mdp/order_book.hpp"

namespace mdp {

struct DepthFrame {
    std::int64_t first_update_id = 0;  // U
    std::int64_t final_update_id = 0;  // u
    std::vector<Level> bids;
    std::vector<Level> asks;
};

struct Snapshot {
    std::int64_t last_update_id = 0;
    std::vector<Level> bids;
    std::vector<Level> asks;
};

bool parse_depth_frame(std::string_view json_text, DepthFrame& out, std::string& err);

bool parse_snapshot(std::string_view json_text, Snapshot& out, std::string& err);

class SyncEngine {
public:
    enum class State { Syncing, Live };

    // Return false if a snapshot is unavailable; the next frame retries.
    using SnapshotFn = std::function<bool(Snapshot& out, std::string& err)>;

    SyncEngine(OrderBook& book, SnapshotFn fetch_snapshot);

    void on_frame(const DepthFrame& frame);

    void on_disconnect();

    State state() const { return state_; }
    std::int64_t last_update_id() const { return last_update_id_; }

    std::uint64_t gap_count() const { return gap_count_; }
    std::uint64_t stale_count() const { return stale_count_; }
    std::uint64_t sync_count() const { return sync_count_; }  // times LIVE was entered
    std::uint64_t disconnect_count() const { return disconnect_count_; }
    std::uint64_t applied_frames() const { return applied_frames_; }
    std::uint64_t applied_level_updates() const { return applied_level_updates_; }
    const std::string& last_error() const { return last_error_; }

private:
    void start_sync();               // clear book + buffer, enter SYNCING
    void try_sync();                 // fetch snapshot, join it to the buffer
    void apply_frame(const DepthFrame& frame);

    OrderBook& book_;
    SnapshotFn fetch_snapshot_;
    State state_ = State::Syncing;
    std::int64_t last_update_id_ = 0;
    std::deque<DepthFrame> buffer_;

    std::uint64_t gap_count_ = 0;
    std::uint64_t stale_count_ = 0;
    std::uint64_t sync_count_ = 0;
    std::uint64_t disconnect_count_ = 0;
    std::uint64_t applied_frames_ = 0;
    std::uint64_t applied_level_updates_ = 0;
    std::string last_error_;
};

}
