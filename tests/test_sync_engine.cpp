#include "test_util.hpp"

#include <string>
#include <string_view>
#include <vector>

#include <simdjson.h>

#include "mdp/fixed_point.hpp"
#include "mdp/order_book.hpp"
#include "mdp/replay_source.hpp"
#include "mdp/sync_engine.hpp"

using mdp::DepthFrame;
using mdp::FixedPoint;
using mdp::OrderBook;
using mdp::ReplaySource;
using mdp::Snapshot;
using mdp::SyncEngine;

namespace {

const std::string kFixtures = MDP_FIXTURE_DIR;

std::vector<std::string> load_lines() {
    std::vector<std::string> lines;
    std::string err;
    CHECK(ReplaySource::load_frame_lines(kFixtures + "/depth_frames.jsonl", lines, err));
    CHECK_EQ(lines.size(), static_cast<std::size_t>(7));
    return lines;
}

// An empty fixture path simulates a failed snapshot fetch.
struct ScriptedSnapshots {
    std::vector<std::string> script;
    std::size_t calls = 0;

    SyncEngine::SnapshotFn fn() {
        return [this](Snapshot& out, std::string& err) {
            const std::string path = calls < script.size() ? script[calls] : script.back();
            ++calls;
            if (path.empty()) {
                err = "scripted failure";
                return false;
            }
            return ReplaySource::load_snapshot_file(path, out, err);
        };
    }
};

void feed(SyncEngine& engine, const std::vector<std::string>& lines, int skip_index = -1) {
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (static_cast<int>(i) == skip_index) continue;
        DepthFrame f;
        std::string err;
        CHECK(mdp::parse_depth_frame(lines[i], f, err));
        engine.on_frame(f);
    }
}

void check_golden(const OrderBook& book, const SyncEngine& engine) {
    std::string text, err;
    CHECK(ReplaySource::read_file(kFixtures + "/golden_top10.json", text, err));
    simdjson::dom::parser parser;
    simdjson::dom::element root;
    CHECK(!parser.parse(text).get(root));
    CHECK(root.is_object());
    std::int64_t final_update_id = 0;
    CHECK(!root["final_update_id"].get_int64().get(final_update_id));
    CHECK_EQ(engine.last_update_id(), final_update_id);
    struct SideSpec {
        const char* key;
        std::vector<mdp::Level> got;
    };
    const SideSpec sides[] = {{"bids", book.top_bids(10)}, {"asks", book.top_asks(10)}};
    for (const auto& side : sides) {
        simdjson::dom::array want;
        CHECK(!root[side.key].get_array().get(want));
        CHECK_EQ(side.got.size(), want.size());
        std::size_t i = 0;
        for (const simdjson::dom::element expected : want) {
            simdjson::dom::array pair;
            CHECK(!expected.get_array().get(pair));
            CHECK_EQ(pair.size(), static_cast<std::size_t>(2));
            auto item = pair.begin();
            std::string_view price;
            std::string_view qty;
            CHECK(!(*item).get_string().get(price));
            ++item;
            CHECK(!(*item).get_string().get(qty));
            CHECK_EQ(FixedPoint::format(side.got[i].price), std::string(price));
            CHECK_EQ(FixedPoint::format(side.got[i].qty), std::string(qty));
            ++i;
        }
    }
}

// Fail the first two fetches to exercise buffering before the snapshot arrives.
void test_startup_seam() {
    auto lines = load_lines();
    OrderBook book;
    ScriptedSnapshots snaps{{"", "", kFixtures + "/snapshot.json"}};
    SyncEngine engine(book, snaps.fn());
    feed(engine, lines);
    CHECK(engine.state() == SyncEngine::State::Live);
    CHECK_EQ(snaps.calls, static_cast<std::size_t>(3));
    CHECK_EQ(engine.sync_count(), 1ULL);
    CHECK_EQ(engine.gap_count(), 0ULL);
    check_golden(book, engine);
}

void test_stale_and_overlap() {
    auto lines = load_lines();
    OrderBook book;
    ScriptedSnapshots snaps{{kFixtures + "/snapshot.json"}};
    SyncEngine engine(book, snaps.fn());
    feed(engine, lines);
    CHECK(engine.state() == SyncEngine::State::Live);
    CHECK_EQ(engine.gap_count(), 0ULL);
    CHECK_EQ(engine.stale_count(), 1ULL);
    CHECK_EQ(engine.applied_frames(), 5ULL);
    CHECK_EQ(engine.applied_level_updates(), 7ULL);
    check_golden(book, engine);
}

void test_gap_resync() {
    auto lines = load_lines();
    OrderBook book;
    ScriptedSnapshots snaps{{kFixtures + "/snapshot.json", kFixtures + "/gap_snapshot.json"}};
    SyncEngine engine(book, snaps.fn());
    feed(engine, lines, /*skip_index=*/4);  // drop the [1004,1006] frame
    CHECK(engine.state() == SyncEngine::State::Live);
    CHECK_EQ(engine.gap_count(), 1ULL);
    CHECK_EQ(engine.sync_count(), 2ULL);
    CHECK_EQ(snaps.calls, static_cast<std::size_t>(2));
    check_golden(book, engine);
}

void test_deterministic_replay() {
    auto lines = load_lines();
    auto run = [&](std::int64_t& final_u, std::vector<mdp::Level>& bids,
                   std::vector<mdp::Level>& asks, std::uint64_t& applied) {
        OrderBook book;
        ScriptedSnapshots snaps{{kFixtures + "/snapshot.json"}};
        SyncEngine engine(book, snaps.fn());
        feed(engine, lines);
        final_u = engine.last_update_id();
        bids = book.top_bids(10);
        asks = book.top_asks(10);
        applied = engine.applied_level_updates();
        check_golden(book, engine);
    };
    std::int64_t u1, u2;
    std::vector<mdp::Level> b1, b2, a1, a2;
    std::uint64_t n1, n2;
    run(u1, b1, a1, n1);
    run(u2, b2, a2, n2);
    CHECK_EQ(u1, u2);
    CHECK_EQ(n1, n2);
    CHECK_EQ(b1.size(), b2.size());
    CHECK_EQ(a1.size(), a2.size());
    for (std::size_t i = 0; i < b1.size(); ++i) {
        CHECK_EQ(b1[i].price, b2[i].price);
        CHECK_EQ(b1[i].qty, b2[i].qty);
    }
    for (std::size_t i = 0; i < a1.size(); ++i) {
        CHECK_EQ(a1[i].price, a2[i].price);
        CHECK_EQ(a1[i].qty, a2[i].qty);
    }
}

void test_snapshot_too_old() {
    auto lines = load_lines();
    OrderBook book;
    // At L=990, none of the buffered frames cover the next update (991).
    const std::string old_snap = kFixtures + "/old_snapshot.json";
    ScriptedSnapshots snaps{{old_snap, kFixtures + "/snapshot.json"}};
    SyncEngine engine(book, snaps.fn());
    // [995,1001] cannot join a snapshot at L=990.
    feed(engine, {lines[1]});
    CHECK(engine.state() == SyncEngine::State::Syncing);
    feed(engine, {lines[2], lines[3], lines[4], lines[5], lines[6]});
    CHECK(engine.state() == SyncEngine::State::Live);
    CHECK_EQ(engine.gap_count(), 0ULL);
    check_golden(book, engine);
}

void test_crossed_book() {
    auto lines = load_lines();
    OrderBook book;
    ScriptedSnapshots snaps{{kFixtures + "/snapshot.json"}};
    SyncEngine engine(book, snaps.fn());
    feed(engine, lines);
    CHECK(engine.state() == SyncEngine::State::Live);
    DepthFrame bad;
    std::string err;
    CHECK(mdp::parse_depth_frame(
        R"({"e":"depthUpdate","U":1011,"u":1011,"b":[["100.15","1.0"]],"a":[]})", bad, err));
    engine.on_frame(bad);  // bid at the best ask price -> crossed
    CHECK(engine.state() == SyncEngine::State::Syncing);
    CHECK(engine.last_error().find("crossed") != std::string::npos);
}

void test_malformed_input() {
    DepthFrame f;
    std::string err;
    auto bad_frame = [&](std::string_view text, std::string_view reason) {
        CHECK(!mdp::parse_depth_frame(text, f, err));
        CHECK(err.find(reason) != std::string::npos);
    };

    bad_frame(R"({"e":"x"})", "U/u");
    bad_frame(R"({"u":2,"b":[],"a":[]})", "U/u");
    bad_frame(R"({"U":1,"b":[],"a":[]})", "U/u");
    bad_frame(R"({"U":"1","u":2,"b":[],"a":[]})", "U/u");
    bad_frame(R"({"U":1.0,"u":2,"b":[],"a":[]})", "U/u");
    bad_frame(R"({"U":true,"u":2,"b":[],"a":[]})", "U/u");
    bad_frame(R"({"U":0,"u":2,"b":[],"a":[]})", "sequence range");
    bad_frame(R"({"U":5,"u":4,"b":[],"a":[]})", "sequence range");

    bad_frame(R"({"U":1,"u":2,"b":{},"a":[]})", "not an array");
    bad_frame(R"({"U":1,"u":2,"b":[],"a":"no"})", "not an array");
    bad_frame(R"({"U":1,"u":2,"b":[1],"a":[]})", "string pair");
    bad_frame(R"({"U":1,"u":2,"b":[["1"]],"a":[]})", "string pair");
    bad_frame(R"({"U":1,"u":2,"b":[[1,"2"]],"a":[]})", "string pair");
    bad_frame(R"({"U":1,"u":2,"b":[["1",2]],"a":[]})", "string pair");
    bad_frame(R"({"U":1,"u":2,"b":[["abc","1"]],"a":[]})", "malformed price");
    bad_frame(R"({"U":1,"u":2,"b":[["1.0","-3"]],"a":[]})", "malformed quantity");
    bad_frame(R"({"U":1,"u":2,"b":[["1.0","xyz"]],"a":[]})", "malformed quantity");
    bad_frame(R"({"U":1,"u":2,"b":[["0","1"]],"a":[]})", "zero price");

    bad_frame("not json", "malformed json");
    bad_frame(R"({"U":1,"u":2,"b":[],"a":[]} trailing)", "malformed json");
    bad_frame(R"({"U":1,"u":2,"b":[],"a":[],"unused":tru})", "malformed json");
    bad_frame(R"([{"U":1,"u":2,"b":[],"a":[]}])", "not a JSON object");
    bad_frame(R"({"stream":"btcusdt@depth","data":[]})", "not a JSON object");

    Snapshot s;
    auto bad_snapshot = [&](std::string_view text, std::string_view reason) {
        CHECK(!mdp::parse_snapshot(text, s, err));
        CHECK(err.find(reason) != std::string::npos);
    };
    bad_snapshot(R"({"bids":[],"asks":[]})", "lastUpdateId");
    bad_snapshot(R"({"lastUpdateId":0,"bids":[],"asks":[]})", "lastUpdateId");
    bad_snapshot(R"({"lastUpdateId":"7","bids":[],"asks":[]})", "lastUpdateId");
    bad_snapshot(R"({"lastUpdateId":7,"bids":[]})", "bids/asks");
    bad_snapshot(R"({"lastUpdateId":7,"asks":[]})", "bids/asks");
    bad_snapshot(R"({"lastUpdateId":7,"bids":{},"asks":[]})", "not an array");
    bad_snapshot(R"({"lastUpdateId":7,"bids":[],"asks":false})", "not an array");
    bad_snapshot(R"({"lastUpdateId":7,"bids":[["x","1"]],"asks":[]})", "malformed price");
    bad_snapshot(R"({"lastUpdateId":7,"bids":[],"asks":[["1","x"]]})", "malformed quantity");
    bad_snapshot(R"({"lastUpdateId":7,"bids":[],"asks":[],"unused":[})", "malformed json");

    CHECK(mdp::parse_depth_frame(
        R"({"stream":"btcusdt@depth","data":{"U":10,"u":12,"b":[["1.5","2"]],"a":[]}})", f, err));
    CHECK_EQ(f.first_update_id, 10LL);
    CHECK_EQ(f.final_update_id, 12LL);
    CHECK_EQ(f.bids.size(), static_cast<std::size_t>(1));

    CHECK(mdp::parse_depth_frame(
        R"({"a":[],"b":[["\u0031.25","2.5",{"ignored":true}]],"u":21,"U":20})", f, err));
    CHECK_EQ(f.first_update_id, 20LL);
    CHECK_EQ(f.final_update_id, 21LL);
    CHECK_EQ(f.bids.size(), static_cast<std::size_t>(1));
    std::int64_t expected_price = 0;
    std::int64_t expected_qty = 0;
    CHECK(FixedPoint::parse("1.25", expected_price));
    CHECK(FixedPoint::parse("2.5", expected_qty));
    CHECK_EQ(f.bids[0].price, expected_price);
    CHECK_EQ(f.bids[0].qty, expected_qty);

    CHECK(mdp::parse_snapshot(
        R"({"asks":[],"bids":[["1.25","0"]],"lastUpdateId":21})", s, err));
    CHECK_EQ(s.last_update_id, 21LL);
    CHECK_EQ(s.bids.size(), static_cast<std::size_t>(1));
}

void test_disconnect_recovery() {
    auto lines = load_lines();
    OrderBook book;
    ScriptedSnapshots snaps{
        {kFixtures + "/snapshot.json", kFixtures + "/gap_snapshot.json"}};
    SyncEngine engine(book, snaps.fn());
    feed(engine, lines);
    CHECK(engine.state() == SyncEngine::State::Live);
    engine.on_disconnect();
    CHECK(engine.state() == SyncEngine::State::Syncing);
    CHECK_EQ(engine.disconnect_count(), 1ULL);
    CHECK_EQ(engine.last_update_id(), 0LL);
    feed(engine, {lines[5], lines[6]});
    CHECK(engine.state() == SyncEngine::State::Live);
    CHECK_EQ(engine.sync_count(), 2ULL);
    check_golden(book, engine);
}

}

int main() {
    test_startup_seam();
    test_stale_and_overlap();
    test_gap_resync();
    test_deterministic_replay();
    test_snapshot_too_old();
    test_crossed_book();
    test_malformed_input();
    test_disconnect_recovery();
    return test_summary("test_sync_engine");
}
