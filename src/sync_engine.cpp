#include "mdp/sync_engine.hpp"

#include <utility>

#include <simdjson.h>

#include "mdp/fixed_point.hpp"

namespace mdp {

namespace {

namespace dom = simdjson::dom;

// One parser is reused on the single-threaded event path.
dom::parser& json_parser() {
    static dom::parser parser;
    return parser;
}

bool parse_json(std::string_view json_text, dom::element& root, std::string& err) {
    const char* data = json_text.data() != nullptr ? json_text.data() : "";
    const simdjson::error_code error = json_parser().parse(data, json_text.size()).get(root);
    if (error) {
        err = "malformed json: " + std::string(simdjson::error_message(error));
        return false;
    }
    return true;
}

// Parse decimal strings directly to fixed-point values.
bool parse_levels(const dom::element& value, std::vector<Level>& out, std::string& err) {
    dom::array entries;
    if (value.get_array().get(entries)) {
        err = "levels field is not an array";
        return false;
    }
    out.reserve(entries.size());
    for (const dom::element entry : entries) {
        dom::array pair;
        if (entry.get_array().get(pair) || pair.size() < 2) {
            err = "level entry is not a [price, qty] string pair";
            return false;
        }
        auto item = pair.begin();
        const dom::element price = *item;
        ++item;
        const dom::element qty = *item;
        std::string_view price_s;
        std::string_view qty_s;
        if (price.get_string().get(price_s) || qty.get_string().get(qty_s)) {
            err = "level entry is not a [price, qty] string pair";
            return false;
        }
        Level lvl;
        if (!FixedPoint::parse(price_s, lvl.price)) {
            err = "malformed price '" + std::string(price_s) + "'";
            return false;
        }
        if (!FixedPoint::parse(qty_s, lvl.qty)) {
            err = "malformed quantity '" + std::string(qty_s) + "'";
            return false;
        }
        if (lvl.price == 0 && lvl.qty != 0) {
            err = "zero price with nonzero quantity";
            return false;
        }
        out.push_back(lvl);
    }
    return true;
}

bool get_seq_id(const dom::element& obj, const char* key, std::int64_t& out) {
    dom::element value;
    if (obj[key].get(value) || !value.is_int64()) return false;
    return !value.get_int64().get(out);
}

bool parse_optional_levels(const dom::element& obj, const char* key,
                           std::vector<Level>& out, std::string& err) {
    dom::element value;
    const simdjson::error_code field_error = obj[key].get(value);
    if (field_error == simdjson::NO_SUCH_FIELD) return true;
    if (field_error) {
        err = "malformed levels field";
        return false;
    }
    return parse_levels(value, out, err);
}

}

bool parse_depth_frame(std::string_view json_text, DepthFrame& out, std::string& err) {
    dom::element root;
    if (!parse_json(json_text, root, err)) return false;

    dom::element msg = root;
    if (root.is_object()) {  // combined-stream {"stream":..,"data":{..}} wrapper
        dom::element data;
        const simdjson::error_code data_error = root["data"].get(data);
        if (!data_error) {
            msg = data;
        } else if (data_error != simdjson::NO_SUCH_FIELD) {
            err = "malformed data field";
            return false;
        }
    }
    if (!msg.is_object()) {
        err = "frame is not a JSON object";
        return false;
    }

    DepthFrame parsed;
    if (!get_seq_id(msg, "U", parsed.first_update_id) ||
        !get_seq_id(msg, "u", parsed.final_update_id)) {
        err = "missing or malformed U/u sequence ids";
        return false;
    }
    if (parsed.first_update_id < 1 || parsed.final_update_id < parsed.first_update_id) {
        err = "invalid sequence range";
        return false;
    }
    if (!parse_optional_levels(msg, "b", parsed.bids, err)) return false;
    if (!parse_optional_levels(msg, "a", parsed.asks, err)) return false;
    out = std::move(parsed);
    return true;
}

bool parse_snapshot(std::string_view json_text, Snapshot& out, std::string& err) {
    dom::element root;
    if (!parse_json(json_text, root, err)) return false;
    if (!root.is_object()) {
        err = "snapshot is not a JSON object";
        return false;
    }

    Snapshot parsed;
    if (!get_seq_id(root, "lastUpdateId", parsed.last_update_id) || parsed.last_update_id < 1) {
        err = "missing or malformed lastUpdateId";
        return false;
    }
    dom::element bids;
    dom::element asks;
    if (root["bids"].get(bids) || root["asks"].get(asks)) {
        err = "missing bids/asks";
        return false;
    }
    if (!parse_levels(bids, parsed.bids, err)) return false;
    if (!parse_levels(asks, parsed.asks, err)) return false;
    out = std::move(parsed);
    return true;
}

SyncEngine::SyncEngine(OrderBook& book, SnapshotFn fetch_snapshot)
    : book_(book), fetch_snapshot_(std::move(fetch_snapshot)) {
    start_sync();
}

void SyncEngine::on_frame(const DepthFrame& frame) {
    if (state_ == State::Live) {
        if (frame.final_update_id <= last_update_id_) {
            ++stale_count_;  // duplicate or older than the book; safe to drop
            return;
        }
        if (frame.first_update_id > last_update_id_ + 1) {
            // Rebuild after a gap; keep this frame to join with the new snapshot.
            ++gap_count_;
            last_error_ = "sequence gap: expected <= " + std::to_string(last_update_id_ + 1) +
                          ", got U=" + std::to_string(frame.first_update_id);
            start_sync();
            buffer_.push_back(frame);
            try_sync();
            return;
        }
        // Overlapping ranges are OK: quantities replace levels, rather than add to them.
        apply_frame(frame);
        return;
    }
    buffer_.push_back(frame);
    try_sync();
}

void SyncEngine::on_disconnect() {
    ++disconnect_count_;
    start_sync();
}

void SyncEngine::start_sync() {
    book_.clear();
    buffer_.clear();
    last_update_id_ = 0;
    state_ = State::Syncing;
}

void SyncEngine::try_sync() {
    if (buffer_.empty()) return;

    Snapshot snap;
    std::string err;
    if (!fetch_snapshot_(snap, err)) {
        last_error_ = "snapshot unavailable: " + err;
        return;  // keep buffering; the next frame retries
    }

    while (!buffer_.empty() && buffer_.front().final_update_id <= snap.last_update_id) {
        buffer_.pop_front();
    }
    // The first retained frame must cover lastUpdateId + 1. Otherwise retry.
    if (!buffer_.empty() && buffer_.front().first_update_id > snap.last_update_id + 1) {
        last_error_ = "snapshot too old to bridge: lastUpdateId=" +
                      std::to_string(snap.last_update_id) + ", first buffered U=" +
                      std::to_string(buffer_.front().first_update_id);
        return;
    }

    book_.clear();
    for (const Level& l : snap.bids) book_.set_bid(l.price, l.qty);
    for (const Level& l : snap.asks) book_.set_ask(l.price, l.qty);
    if (book_.crossed()) {
        last_error_ = "snapshot produced a crossed book";
        book_.clear();
        return;  // discard it; stay SYNCING and retry on the next frame
    }

    last_update_id_ = snap.last_update_id;
    state_ = State::Live;
    ++sync_count_;

    // Use the live path so gaps in buffered frames trigger recovery too.
    std::deque<DepthFrame> pending;
    pending.swap(buffer_);
    for (const DepthFrame& f : pending) on_frame(f);
}

void SyncEngine::apply_frame(const DepthFrame& frame) {
    for (const Level& l : frame.bids) book_.set_bid(l.price, l.qty);
    for (const Level& l : frame.asks) book_.set_ask(l.price, l.qty);
    if (book_.crossed()) {
        last_error_ = "crossed book after applying frame u=" +
                      std::to_string(frame.final_update_id);
        ++gap_count_;
        start_sync();
        return;
    }
    last_update_id_ = frame.final_update_id;
    ++applied_frames_;
    applied_level_updates_ += frame.bids.size() + frame.asks.size();
}

}
