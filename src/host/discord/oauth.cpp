#include "discord/oauth.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <string>

namespace choir {

namespace {

using nlohmann::json;

// Read a string field defensively: returns "" if the key is missing or the
// value is not a string (a JSON null also yields "").
std::string str_or(const json& obj, const char* key) {
    if (!obj.is_object()) return "";
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return "";
    return it->get<std::string>();
}

// Truncate a body for inclusion in an error message so we never paste a huge
// (or secret-laden) response into a log line.
std::string snippet(const std::string& body, size_t max = 200) {
    if (body.size() <= max) return body;
    return body.substr(0, max) + "...";
}

// The Content-Type header common to both modes.
std::vector<std::pair<std::string, std::string>> form_headers() {
    return {{"Content-Type", "application/x-www-form-urlencoded"}};
}

}  // namespace

std::string url_encode(const std::string& value) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (unsigned char c : value) {
        // RFC 3986 unreserved characters pass through untouched.
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0xF]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

TokenResult exchange_code(HttpPost& http, AuthMode mode, const std::string& code,
                          const std::string& client_id, const std::string& client_secret) {
    TokenResult result;

    std::string url;
    std::vector<std::pair<std::string, std::string>> form;

    if (mode == AuthMode::Streamkit) {
        // Streamkit holds the client secret server-side; we only send the code.
        url = kStreamkitTokenUrl;
        form = {{"code", code}};
    } else {
        // Standard OAuth2 authorization_code grant with our own credentials.
        url = kDiscordTokenUrl;
        form = {
            {"grant_type", "authorization_code"},
            {"code", code},
            {"client_id", client_id},
            {"client_secret", client_secret},
        };
    }

    HttpResponse resp = http.post(url, form, form_headers());

    if (resp.status != 200) {
        result.error = "token exchange failed: HTTP " + std::to_string(resp.status) +
                       " body=" + snippet(resp.body);
        return result;
    }

    // Defensive parse: never throw out of exchange_code.
    json parsed = json::parse(resp.body, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        result.error = "token exchange returned unparseable JSON (HTTP " +
                       std::to_string(resp.status) + "): " + snippet(resp.body);
        return result;
    }

    std::string access = str_or(parsed, "access_token");
    if (access.empty()) {
        result.error = "token exchange response missing access_token (HTTP " +
                       std::to_string(resp.status) + "): " + snippet(resp.body);
        return result;
    }

    result.ok = true;
    result.access_token = access;
    // Streamkit responses typically omit a usable refresh_token; pass it through
    // when present, leave it empty otherwise.
    result.refresh_token = str_or(parsed, "refresh_token");
    return result;
}

}  // namespace choir
