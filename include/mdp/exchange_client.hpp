#pragma once

#include <functional>
#include <string>

#include "mdp/sync_engine.hpp"

namespace mdp {

class ExchangeClient {
public:
    struct Config {
        std::string rest_host = "api.binance.com";
        std::string ws_host = "stream.binance.com";
        unsigned short rest_port = 443;
        unsigned short ws_port = 9443;
        std::string symbol = "BTCUSDT";
        std::string stream_suffix = "@depth@100ms";
    };

    explicit ExchangeClient(Config cfg);
    ~ExchangeClient();
    ExchangeClient(const ExchangeClient&) = delete;
    ExchangeClient& operator=(const ExchangeClient&) = delete;

    bool fetch_snapshot(Snapshot& out, std::string& err);

    // Blocks until the callback returns false or the connection fails.
    // Returns false on connection failure, with details in err.
    bool run_stream(const std::function<bool(const std::string&)>& on_message, std::string& err);

private:
    Config cfg_;
    void* session_ = nullptr;  // HINTERNET, kept as void* to keep windows.h out of this header
};

}
