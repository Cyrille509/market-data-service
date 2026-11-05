#pragma once

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "mdp/sync_engine.hpp"

namespace mdp {

struct ReplaySource {
    static bool read_file(const std::string& path, std::string& out, std::string& err) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            err = "cannot open " + path;
            return false;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        out = ss.str();
        return true;
    }

    static bool load_snapshot_file(const std::string& path, Snapshot& out, std::string& err) {
        std::string text;
        if (!read_file(path, text, err)) return false;
        if (!parse_snapshot(text, out, err)) {
            err = path + ": " + err;
            return false;
        }
        return true;
    }

    static bool load_frame_lines(const std::string& path, std::vector<std::string>& out,
                                 std::string& err) {
        std::string text;
        if (!read_file(path, text, err)) return false;
        std::istringstream ss(text);
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) out.push_back(line);
        }
        if (out.empty()) {
            err = path + ": no frames";
            return false;
        }
        return true;
    }
};

}
