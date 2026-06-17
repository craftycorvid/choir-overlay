#include "config/config.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>

#include "config/denylist.hpp"

namespace choir {

namespace {

using nlohmann::json;

const char* auth_mode_to_str(AuthMode m) {
    return (m == AuthMode::OwnApp) ? "own-app" : "streamkit";
}

AuthMode auth_mode_from_str(const std::string& s) {
    if (s == "own-app") return AuthMode::OwnApp;
    return AuthMode::Streamkit;  // default on "streamkit" or anything unknown
}

// Read whole file into `out`. Returns false if it can't be opened/read.
bool read_file(const std::string& path, std::string& out) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    std::string data;
    char buf[4096];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof buf)) > 0) {
        data.append(buf, static_cast<size_t>(n));
    }
    const bool ok = (n >= 0);  // n==0 is EOF; n<0 is error
    ::close(fd);
    if (!ok) return false;
    out = std::move(data);
    return true;
}

// Recursively create the parent directory of `path` (mkdir -p of dirname).
void make_parent_dirs(const std::string& path) {
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return;  // no/root parent
    const std::string dir = path.substr(0, slash);
    // Walk each prefix component, creating as we go.
    for (std::size_t i = 1; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == '/') {
            const std::string sub = dir.substr(0, i);
            if (!sub.empty()) {
                ::mkdir(sub.c_str(), 0700);  // ignore EEXIST
            }
        }
    }
}

}  // namespace

Config Config::load(const std::string& path) {
    Config c;  // member defaults already in place

    std::string text;
    if (!read_file(path, text)) {
        // Absent or unreadable -> defaults + default denylist.
        c.denylist = Denylist::defaults();
        return c;
    }

    // Defensive parse: never throw on malformed input.
    json j = json::parse(text, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        c.denylist = Denylist::defaults();
        return c;
    }

    // Each field is optional; fall back to the default already in `c`.
    try {
        if (j.contains("appearance") && j["appearance"].is_object()) {
            c.appearance = j["appearance"].get<AppearanceConfig>();
        }
    } catch (...) {
        // keep default appearance
    }

    if (j.contains("auth_mode") && j["auth_mode"].is_string()) {
        c.auth_mode = auth_mode_from_str(j["auth_mode"].get<std::string>());
    }
    if (j.contains("client_id") && j["client_id"].is_string()) {
        c.client_id = j["client_id"].get<std::string>();
    }
    if (j.contains("client_secret") && j["client_secret"].is_string()) {
        c.client_secret = j["client_secret"].get<std::string>();
    }
    if (j.contains("access_token") && j["access_token"].is_string()) {
        c.access_token = j["access_token"].get<std::string>();
    }
    if (j.contains("refresh_token") && j["refresh_token"].is_string()) {
        c.refresh_token = j["refresh_token"].get<std::string>();
    }

    if (j.contains("denylist") && j["denylist"].is_array()) {
        std::vector<std::string> dl;
        for (const auto& el : j["denylist"]) {
            if (el.is_string()) dl.push_back(el.get<std::string>());
        }
        c.denylist = std::move(dl);
    }

    // An absent or emptied denylist gets the defaults (so a fresh/partial config
    // is still protected).
    if (c.denylist.empty()) {
        c.denylist = Denylist::defaults();
    }

    return c;
}

bool Config::save(const std::string& path) const {
    json j;
    j["appearance"] = appearance;
    j["auth_mode"] = auth_mode_to_str(auth_mode);
    j["client_id"] = client_id;
    j["client_secret"] = client_secret;
    j["access_token"] = access_token;
    j["refresh_token"] = refresh_token;
    j["denylist"] = denylist;

    const std::string text = j.dump(2);

    make_parent_dirs(path);

    // 0600 from creation; the file holds an access token.
    const int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0) return false;

    // Belt-and-suspenders: if the file pre-existed with looser perms, O_CREAT's
    // mode is ignored, so force 0600 explicitly.
    ::fchmod(fd, 0600);

    const char* p = text.data();
    size_t left = text.size();
    bool ok = true;
    while (left > 0) {
        const ssize_t n = ::write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        p += n;
        left -= static_cast<size_t>(n);
    }

    if (::close(fd) != 0) ok = false;
    return ok;
}

}  // namespace choir
