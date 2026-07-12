#include "discord/oauth.hpp"

#include <nlohmann/json.hpp>

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

}  // namespace

TokenResult exchange_code(HttpPost& http, const std::string& code) {
    TokenResult result;

    // Streamkit's Cloudflare worker holds the client secret and expects a JSON
    // body {"code": ...} (a form body makes it throw -> HTTP 500 "error code:
    // 1101"). We only send the code.
    const std::string json_body = json{{"code", code}}.dump();
    HttpResponse resp = http.post_json(kStreamkitTokenUrl, json_body, /*headers=*/{});

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
