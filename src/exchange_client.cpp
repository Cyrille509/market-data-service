#ifdef _WIN32

#include "mdp/exchange_client.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <vector>

namespace mdp {

namespace {

std::wstring widen(const std::string& s) {
    return std::wstring(s.begin(), s.end());  // hosts/paths here are ASCII
}

std::string win_err(const char* what) {
    return std::string(what) + " (WinHTTP error " + std::to_string(GetLastError()) + ")";
}

struct HandleGuard {
    HINTERNET h = nullptr;
    ~HandleGuard() {
        if (h) WinHttpCloseHandle(h);
    }
};

}

ExchangeClient::ExchangeClient(Config cfg) : cfg_(std::move(cfg)) {
    session_ = WinHttpOpen(L"mdp/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session_) {
        // Resolve, connect, send, and receive timeouts in milliseconds.
        WinHttpSetTimeouts(static_cast<HINTERNET>(session_), 10000, 10000, 15000, 60000);
    }
}

ExchangeClient::~ExchangeClient() {
    if (session_) WinHttpCloseHandle(static_cast<HINTERNET>(session_));
}

bool ExchangeClient::fetch_snapshot(Snapshot& out, std::string& err) {
    if (!session_) {
        err = "WinHttpOpen failed";
        return false;
    }
    HandleGuard conn;
    conn.h = WinHttpConnect(static_cast<HINTERNET>(session_), widen(cfg_.rest_host).c_str(),
                            cfg_.rest_port, 0);
    if (!conn.h) {
        err = win_err("WinHttpConnect(rest) failed");
        return false;
    }
    const std::wstring path = widen("/api/v3/depth?symbol=" + cfg_.symbol + "&limit=1000");
    HandleGuard req;
    req.h = WinHttpOpenRequest(conn.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                               WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req.h) {
        err = win_err("WinHttpOpenRequest(rest) failed");
        return false;
    }
    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
                            0, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr)) {
        err = win_err("snapshot request failed");
        return false;
    }
    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    std::string body;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail)) {
            err = win_err("snapshot read failed");
            return false;
        }
        if (avail == 0) break;
        std::size_t off = body.size();
        body.resize(off + avail);
        DWORD read = 0;
        if (!WinHttpReadData(req.h, &body[off], avail, &read)) {
            err = win_err("snapshot read failed");
            return false;
        }
        body.resize(off + read);
    }
    if (status != 200) {
        err = "snapshot HTTP status " + std::to_string(status) + ": " + body.substr(0, 200);
        return false;
    }
    return parse_snapshot(body, out, err);
}

bool ExchangeClient::run_stream(const std::function<bool(const std::string&)>& on_message,
                                std::string& err) {
    if (!session_) {
        err = "WinHttpOpen failed";
        return false;
    }
    HandleGuard conn;
    conn.h = WinHttpConnect(static_cast<HINTERNET>(session_), widen(cfg_.ws_host).c_str(),
                            cfg_.ws_port, 0);
    if (!conn.h) {
        err = win_err("WinHttpConnect(ws) failed");
        return false;
    }
    std::string lower = cfg_.symbol;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::wstring path = widen("/ws/" + lower + cfg_.stream_suffix);
    HandleGuard req;
    req.h = WinHttpOpenRequest(conn.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                               WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req.h) {
        err = win_err("WinHttpOpenRequest(ws) failed");
        return false;
    }
    if (!WinHttpSetOption(req.h, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
        err = win_err("websocket upgrade option failed");
        return false;
    }
    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
                            0, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr)) {
        err = win_err("websocket handshake failed");
        return false;
    }
    HandleGuard ws;
    ws.h = WinHttpWebSocketCompleteUpgrade(req.h, 0);
    if (!ws.h) {
        err = win_err("websocket upgrade failed");
        return false;
    }

    std::vector<char> buf(64 * 1024);
    std::string message;
    for (;;) {
        DWORD read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        DWORD rc = WinHttpWebSocketReceive(ws.h, buf.data(), static_cast<DWORD>(buf.size()),
                                           &read, &type);
        if (rc != NO_ERROR) {
            err = "websocket receive failed (error " + std::to_string(rc) + ")";
            return false;
        }
        switch (type) {
            case WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE:
            case WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE:
                message.append(buf.data(), read);
                break;
            case WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE:
            case WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE: {
                message.append(buf.data(), read);
                bool keep_going = on_message(message);
                message.clear();
                if (!keep_going) {
                    WinHttpWebSocketClose(ws.h, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
                                          nullptr, 0);
                    return true;
                }
                break;
            }
            case WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE:
                err = "websocket closed by server";
                return false;
        }
    }
}

}

#endif  // _WIN32
