#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace mdp {

// Prices and quantities are integers scaled by 10^8.
struct FixedPoint {
    static constexpr int kFracDigits = 8;
    static constexpr std::int64_t kOne = 100000000LL;  // 10^8

    // Reject negatives, overflow, and values not representable at this scale.
    static bool parse(std::string_view s, std::int64_t& out) {
        if (s.empty()) return false;
        std::int64_t int_part = 0;
        std::size_t i = 0;
        bool any_digit = false;
        for (; i < s.size() && s[i] != '.'; ++i) {
            const char c = s[i];
            if (c < '0' || c > '9') return false;
            if (int_part > (std::numeric_limits<std::int64_t>::max() - 9) / 10) return false;
            int_part = int_part * 10 + (c - '0');
            any_digit = true;
        }
        std::int64_t frac_part = 0;
        int frac_digits = 0;
        if (i < s.size()) {  // s[i] == '.'
            ++i;
            if (i >= s.size()) return false;  // "123."
            for (; i < s.size(); ++i) {
                const char c = s[i];
                if (c < '0' || c > '9') return false;
                if (frac_digits < kFracDigits) {
                    frac_part = frac_part * 10 + (c - '0');
                    ++frac_digits;
                } else if (c != '0') {
                    return false;  // precision the scale cannot represent
                }
            }
            any_digit = true;
        }
        if (!any_digit) return false;
        while (frac_digits < kFracDigits) {
            frac_part *= 10;
            ++frac_digits;
        }
        if (int_part > (std::numeric_limits<std::int64_t>::max() - frac_part) / kOne) return false;
        out = int_part * kOne + frac_part;
        return true;
    }

    static std::string format(std::int64_t v) {
        std::string sign;
        std::uint64_t a;
        if (v < 0) {
            sign = "-";
            a = static_cast<std::uint64_t>(-(v + 1)) + 1;
        } else {
            a = static_cast<std::uint64_t>(v);
        }
        const std::uint64_t one = static_cast<std::uint64_t>(kOne);
        std::string r = sign + std::to_string(a / one);
        std::uint64_t frac = a % one;
        if (frac != 0) {
            std::string f = std::to_string(frac);
            f.insert(0, static_cast<std::size_t>(kFracDigits) - f.size(), '0');
            while (!f.empty() && f.back() == '0') f.pop_back();
            r += '.';
            r += f;
        }
        return r;
    }
};

}
