#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace mdp {

struct Level {
    std::int64_t price = 0;
    std::int64_t qty = 0;
};

class OrderBook {
public:
    // Zero quantity removes a level; otherwise replace its quantity.
    void set_bid(std::int64_t price, std::int64_t qty);
    void set_ask(std::int64_t price, std::int64_t qty);

    std::vector<Level> top_bids(std::size_t n) const;  // best (highest) first
    std::vector<Level> top_asks(std::size_t n) const;  // best (lowest) first

    std::size_t bid_depth() const { return bids_.size(); }
    std::size_t ask_depth() const { return asks_.size(); }
    bool empty() const { return bids_.empty() && asks_.empty(); }

    bool crossed() const;

    void clear();

private:
    std::map<std::int64_t, std::int64_t, std::greater<std::int64_t>> bids_;
    std::map<std::int64_t, std::int64_t> asks_;
};

}
