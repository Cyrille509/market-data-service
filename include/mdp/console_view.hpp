#pragma once

#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "mdp/fixed_point.hpp"
#include "mdp/order_book.hpp"
#include "mdp/sync_engine.hpp"

namespace mdp {

class ConsoleView {
public:
    explicit ConsoleView(bool ansi = true) : ansi_(ansi) {
#ifdef _WIN32
        // Enable ANSI cursor control on Windows.
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
            SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        } else {
            ansi_ = false;  // redirected output: plain text, no cursor moves
        }
#endif
    }

    void render(const OrderBook& book, const SyncEngine& engine, const std::string& symbol) {
        const std::size_t depth = 10;
        std::vector<Level> bids = book.top_bids(depth);
        std::vector<Level> asks = book.top_asks(depth);

        std::string out;
        out.reserve(2048);
        if (ansi_) out += "\x1b[H\x1b[J";  // home + clear to end of screen

        out += symbol + "  [" +
               (engine.state() == SyncEngine::State::Live ? "LIVE" : "SYNCING") +
               "]  last_u=" + std::to_string(engine.last_update_id()) +
               "  gaps=" + std::to_string(engine.gap_count()) +
               "  syncs=" + std::to_string(engine.sync_count()) +
               "  stale=" + std::to_string(engine.stale_count()) + "\n";
        if (!engine.last_error().empty()) {
            out += "last event: " + engine.last_error() + "\n";
        }
        out += "\n            ASKS (qty @ price)\n";
        // Put the best ask next to the best bid.
        for (std::size_t i = asks.size(); i-- > 0;) {
            out += row(asks[i]);
        }
        out += "  ----------------------------------------\n";
        for (const Level& l : bids) {
            out += row(l);
        }
        out += "\n            BIDS (qty @ price)\n";

        std::fwrite(out.data(), 1, out.size(), stdout);
        std::fflush(stdout);
    }

private:
    static std::string row(const Level& l) {
        std::string qty = FixedPoint::format(l.qty);
        std::string price = FixedPoint::format(l.price);
        std::string line = "  ";
        if (qty.size() < 14) line += std::string(14 - qty.size(), ' ');
        line += qty + " @ " + price + "\n";
        return line;
    }

    bool ansi_;
};

}
