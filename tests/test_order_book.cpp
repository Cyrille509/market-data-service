#include "test_util.hpp"

#include "mdp/fixed_point.hpp"
#include "mdp/order_book.hpp"

using mdp::FixedPoint;
using mdp::Level;
using mdp::OrderBook;

namespace {

std::int64_t fp(const char* s) {
    std::int64_t v = 0;
    CHECK(FixedPoint::parse(s, v));
    return v;
}

void test_fixed_point_parse() {
    std::int64_t v = 0;
    CHECK(FixedPoint::parse("0", v));
    CHECK_EQ(v, 0LL);
    CHECK(FixedPoint::parse("104521.10", v));
    CHECK_EQ(v, 10452110000000LL);
    CHECK(FixedPoint::parse("0.00000001", v));
    CHECK_EQ(v, 1LL);
    CHECK(FixedPoint::parse("1.5", v));
    CHECK_EQ(v, 150000000LL);
    // Extra decimal places are allowed only if they are zero.
    CHECK(FixedPoint::parse("1.2300000000", v));
    CHECK_EQ(v, 123000000LL);
    CHECK(!FixedPoint::parse("1.230000001", v));

    CHECK(!FixedPoint::parse("", v));
    CHECK(!FixedPoint::parse(".", v));
    CHECK(!FixedPoint::parse("1.", v));
    CHECK(!FixedPoint::parse("abc", v));
    CHECK(!FixedPoint::parse("1.2.3", v));
    CHECK(!FixedPoint::parse("-1", v));
    CHECK(!FixedPoint::parse("1e5", v));
    CHECK(!FixedPoint::parse("1 ", v));
    CHECK(!FixedPoint::parse("92233720368547758.08", v));  // overflow
}

void test_fixed_point_format() {
    CHECK_EQ(FixedPoint::format(fp("104500")), "104500");
    CHECK_EQ(FixedPoint::format(fp("100.00")), "100");
    CHECK_EQ(FixedPoint::format(fp("99.90")), "99.9");
    CHECK_EQ(FixedPoint::format(fp("0.7")), "0.7");
    CHECK_EQ(FixedPoint::format(fp("0.00000001")), "0.00000001");
    CHECK_EQ(FixedPoint::format(0), "0");
}

void test_order_book_levels() {
    OrderBook book;
    CHECK(book.empty());

    book.set_bid(fp("99.5"), fp("1"));
    book.set_bid(fp("100.0"), fp("2"));
    book.set_bid(fp("99.8"), fp("3"));
    book.set_ask(fp("100.3"), fp("4"));
    book.set_ask(fp("100.1"), fp("5"));
    book.set_ask(fp("100.2"), fp("6"));

    auto bids = book.top_bids(10);
    CHECK_EQ(bids.size(), static_cast<std::size_t>(3));
    CHECK_EQ(bids[0].price, fp("100.0"));
    CHECK_EQ(bids[1].price, fp("99.8"));
    CHECK_EQ(bids[2].price, fp("99.5"));

    auto asks = book.top_asks(10);
    CHECK_EQ(asks.size(), static_cast<std::size_t>(3));
    CHECK_EQ(asks[0].price, fp("100.1"));
    CHECK_EQ(asks[1].price, fp("100.2"));
    CHECK_EQ(asks[2].price, fp("100.3"));

    book.set_bid(fp("100.0"), fp("9"));
    CHECK_EQ(book.top_bids(1)[0].qty, fp("9"));

    book.set_bid(fp("99.8"), 0);
    CHECK_EQ(book.bid_depth(), static_cast<std::size_t>(2));
    book.set_bid(fp("50.0"), 0);
    CHECK_EQ(book.bid_depth(), static_cast<std::size_t>(2));

    CHECK_EQ(book.top_asks(2).size(), static_cast<std::size_t>(2));

    CHECK(!book.crossed());
    book.set_bid(fp("100.1"), fp("1"));  // equals best ask -> crossed
    CHECK(book.crossed());
    book.set_bid(fp("100.1"), 0);
    CHECK(!book.crossed());

    book.clear();
    CHECK(book.empty());
    CHECK_EQ(book.top_bids(10).size(), static_cast<std::size_t>(0));
}

}

int main() {
    test_fixed_point_parse();
    test_fixed_point_format();
    test_order_book_levels();
    return test_summary("test_order_book");
}
