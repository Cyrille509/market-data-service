#include "mdp/order_book.hpp"

namespace mdp {

void OrderBook::set_bid(std::int64_t price, std::int64_t qty) {
    if (qty == 0) {
        bids_.erase(price);
    } else {
        bids_[price] = qty;
    }
}

void OrderBook::set_ask(std::int64_t price, std::int64_t qty) {
    if (qty == 0) {
        asks_.erase(price);
    } else {
        asks_[price] = qty;
    }
}

std::vector<Level> OrderBook::top_bids(std::size_t n) const {
    std::vector<Level> out;
    out.reserve(n < bids_.size() ? n : bids_.size());
    for (auto it = bids_.begin(); it != bids_.end() && out.size() < n; ++it) {
        out.push_back({it->first, it->second});
    }
    return out;
}

std::vector<Level> OrderBook::top_asks(std::size_t n) const {
    std::vector<Level> out;
    out.reserve(n < asks_.size() ? n : asks_.size());
    for (auto it = asks_.begin(); it != asks_.end() && out.size() < n; ++it) {
        out.push_back({it->first, it->second});
    }
    return out;
}

bool OrderBook::crossed() const {
    if (bids_.empty() || asks_.empty()) return false;
    return bids_.begin()->first >= asks_.begin()->first;
}

void OrderBook::clear() {
    bids_.clear();
    asks_.clear();
}

}
