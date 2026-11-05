#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <simdjson.h>

#include "mdp/console_view.hpp"
#include "mdp/fixed_point.hpp"
#include "mdp/order_book.hpp"
#include "mdp/replay_source.hpp"
#include "mdp/sync_engine.hpp"

#ifdef _WIN32
#include "mdp/exchange_client.hpp"
#endif

namespace {

using mdp::ConsoleView;
using mdp::DepthFrame;
using mdp::FixedPoint;
using mdp::Level;
using mdp::OrderBook;
using mdp::ReplaySource;
using mdp::Snapshot;
using mdp::SyncEngine;

int usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  mdp replay <snapshot.json> <frames.jsonl> [--golden <golden_top10.json>] [--quiet]\n"
                 "  mdp live [--symbol BTCUSDT] [--rest-host api.binance.com]\n"
                 "           [--ws-host stream.binance.com] [--ws-port 9443]\n"
                 "           [--drop-frame N] [--max-frames N] [--render-ms 250]\n");
    return 2;
}

void print_summary(const OrderBook& book, const SyncEngine& engine, const std::string& symbol) {
    ConsoleView view(/*ansi=*/false);
    view.render(book, engine, symbol);
    std::printf("frames applied: %llu, level updates applied: %llu\n",
                static_cast<unsigned long long>(engine.applied_frames()),
                static_cast<unsigned long long>(engine.applied_level_updates()));
}

bool check_golden(const std::string& path, const OrderBook& book, const SyncEngine& engine) {
    std::string text, err;
    if (!ReplaySource::read_file(path, text, err)) {
        std::fprintf(stderr, "golden: %s\n", err.c_str());
        return false;
    }
    simdjson::dom::parser parser;
    simdjson::dom::element root;
    if (parser.parse(text).get(root) || !root.is_object()) {
        std::fprintf(stderr, "golden: malformed json\n");
        return false;
    }
    simdjson::dom::element final_update_id;
    std::int64_t want_u = 0;
    if (root["final_update_id"].get(final_update_id) || !final_update_id.is_int64() ||
        final_update_id.get_int64().get(want_u)) {
        std::fprintf(stderr, "golden: missing final_update_id\n");
        return false;
    }
    bool ok = true;
    if (engine.last_update_id() != want_u) {
        std::fprintf(stderr, "golden: final_update_id mismatch: got %lld want %lld\n",
                     static_cast<long long>(engine.last_update_id()),
                     static_cast<long long>(want_u));
        ok = false;
    }
    auto check_side = [&](const char* key, const std::vector<Level>& got) {
        simdjson::dom::array want;
        if (root[key].get_array().get(want)) {
            std::fprintf(stderr, "golden: missing %s\n", key);
            ok = false;
            return;
        }
        if (want.size() != got.size()) {
            std::fprintf(stderr, "golden: %s size mismatch: got %zu want %zu\n", key, got.size(),
                         want.size());
            ok = false;
            return;
        }
        std::size_t i = 0;
        for (const simdjson::dom::element expected : want) {
            simdjson::dom::array pair;
            if (expected.get_array().get(pair) || pair.size() != 2) {
                std::fprintf(stderr, "golden: %s[%zu] malformed\n", key, i);
                ok = false;
                return;
            }
            auto item = pair.begin();
            std::string_view wp;
            std::string_view wq;
            if ((*item).get_string().get(wp) || (*(++item)).get_string().get(wq)) {
                std::fprintf(stderr, "golden: %s[%zu] malformed\n", key, i);
                ok = false;
                return;
            }
            std::string gp = FixedPoint::format(got[i].price);
            std::string gq = FixedPoint::format(got[i].qty);
            if (std::string_view(gp) != wp || std::string_view(gq) != wq) {
                std::fprintf(stderr, "golden: %s[%zu] mismatch: got [%s, %s] want [%s, %s]\n",
                             key, i, gp.c_str(), gq.c_str(), std::string(wp).c_str(),
                             std::string(wq).c_str());
                ok = false;
            }
            ++i;
        }
    };
    check_side("bids", book.top_bids(10));
    check_side("asks", book.top_asks(10));
    return ok;
}

int run_replay(const std::vector<std::string>& args) {
    if (args.size() < 2) return usage();
    const std::string snapshot_path = args[0];
    const std::string frames_path = args[1];
    std::string golden_path;
    bool quiet = false;
    for (std::size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--golden" && i + 1 < args.size()) {
            golden_path = args[++i];
        } else if (args[i] == "--quiet") {
            quiet = true;
        } else {
            return usage();
        }
    }

    std::vector<std::string> lines;
    std::string err;
    if (!ReplaySource::load_frame_lines(frames_path, lines, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 2;
    }

    OrderBook book;
    SyncEngine engine(book, [&](Snapshot& out, std::string& serr) {
        return ReplaySource::load_snapshot_file(snapshot_path, out, serr);
    });

    for (std::size_t i = 0; i < lines.size(); ++i) {
        DepthFrame frame;
        if (!mdp::parse_depth_frame(lines[i], frame, err)) {
            std::fprintf(stderr, "error: frame line %zu: %s\n", i + 1, err.c_str());
            return 2;
        }
        engine.on_frame(frame);
    }

    if (!quiet) print_summary(book, engine, "REPLAY");
    if (!golden_path.empty()) {
        if (!check_golden(golden_path, book, engine)) {
            std::printf("golden check: FAIL\n");
            return 1;
        }
        std::printf("golden check: PASS (final_update_id=%lld)\n",
                    static_cast<long long>(engine.last_update_id()));
    }
    return 0;
}

#ifdef _WIN32
int run_live(const std::vector<std::string>& args) {
    mdp::ExchangeClient::Config cfg;
    std::uint64_t drop_frame = 0;   // 1-based index of one frame to drop (forces a gap)
    std::uint64_t max_frames = 0;   // 0 = run until the connection is closed externally
    int render_ms = 250;
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto next = [&](std::string& dst) {
            if (i + 1 >= args.size()) return false;
            dst = args[++i];
            return true;
        };
        std::string v;
        if (args[i] == "--symbol" && next(v)) cfg.symbol = v;
        else if (args[i] == "--rest-host" && next(v)) cfg.rest_host = v;
        else if (args[i] == "--ws-host" && next(v)) cfg.ws_host = v;
        else if (args[i] == "--ws-port" && next(v)) cfg.ws_port = static_cast<unsigned short>(std::atoi(v.c_str()));
        else if (args[i] == "--drop-frame" && next(v)) drop_frame = std::strtoull(v.c_str(), nullptr, 10);
        else if (args[i] == "--max-frames" && next(v)) max_frames = std::strtoull(v.c_str(), nullptr, 10);
        else if (args[i] == "--render-ms" && next(v)) render_ms = std::atoi(v.c_str());
        else return usage();
    }

    mdp::ExchangeClient client(cfg);
    OrderBook book;
    SyncEngine engine(book, [&](Snapshot& out, std::string& serr) {
        return client.fetch_snapshot(out, serr);
    });
    ConsoleView view;

    std::uint64_t frames_seen = 0;
    std::uint64_t parse_errors = 0;
    auto last_render = std::chrono::steady_clock::now() - std::chrono::hours(1);

    auto on_message = [&](const std::string& msg) {
        DepthFrame frame;
        std::string perr;
        if (!mdp::parse_depth_frame(msg, frame, perr)) {
            ++parse_errors;
            std::fprintf(stderr, "frame parse error: %s\n", perr.c_str());
            return true;
        }
        ++frames_seen;
        if (drop_frame != 0 && frames_seen == drop_frame) {
            std::fprintf(stderr, "[forced] dropping frame %llu (U=%lld u=%lld)\n",
                         static_cast<unsigned long long>(frames_seen),
                         static_cast<long long>(frame.first_update_id),
                         static_cast<long long>(frame.final_update_id));
        } else {
            engine.on_frame(frame);
        }
        auto now = std::chrono::steady_clock::now();
        if (now - last_render >= std::chrono::milliseconds(render_ms)) {
            view.render(book, engine, cfg.symbol);
            last_render = now;
        }
        return max_frames == 0 || frames_seen < max_frames;
    };

    for (;;) {
        std::string err;
        bool clean = client.run_stream(on_message, err);
        if (clean) break;  // on_message asked to stop (--max-frames)
        std::fprintf(stderr, "stream ended: %s; reconnecting...\n", err.c_str());
        engine.on_disconnect();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::printf("\n");
    print_summary(book, engine, cfg.symbol);
    std::printf("frames seen: %llu, parse errors: %llu, disconnects: %llu\n",
                static_cast<unsigned long long>(frames_seen),
                static_cast<unsigned long long>(parse_errors),
                static_cast<unsigned long long>(engine.disconnect_count()));
    return 0;
}
#endif

}

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) return usage();
    const std::string mode = args[0];
    args.erase(args.begin());
    if (mode == "replay") return run_replay(args);
    if (mode == "live") {
#ifdef _WIN32
        return run_live(args);
#else
        std::fprintf(stderr, "live mode is Windows-only in this build; replay/tests are portable\n");
        return 2;
#endif
    }
    return usage();
}
