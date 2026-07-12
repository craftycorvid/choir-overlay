#pragma once

// OAuth2 code -> access-token exchange (Task 5).
//
// After the RPC AUTHORIZE command returns an authorization `code`, the host
// trades it for an access token by POSTing the code to Discord's hosted
// Streamkit token endpoint, which holds the client secret. This is how the
// "Discover" overlay works; the user never registers an app.
//
// All HTTP goes through the injectable HttpPost interface so this stays Qt-free
// and unit-testable. The production HttpPost (QNetworkAccessManager-backed) is
// implemented in Task 11. exchange_code() never throws.

#include <string>
#include <utility>
#include <vector>

namespace choir {

// Streamkit's hosted token-exchange endpoint (a Cloudflare Worker that holds
// Discord's client_secret for app 207646673902501888). CONFIRMED by the M0 auth
// spike: it expects a JSON body {"code":"..."} with Content-Type application/json
// (a form-urlencoded body makes the worker throw -> HTTP 500 "error code: 1101").
// On a bad/expired code it returns 200 with {} (no access_token). This is the
// same call the "Discover" overlay makes.
constexpr const char* kStreamkitTokenUrl = "https://streamkit.discord.com/overlay/token";

// Result of a single HTTP POST.
struct HttpResponse {
    int status = 0;
    std::string body;
};

// Injectable HTTP POST seam.
struct HttpPost {
    virtual ~HttpPost() = default;
    // Send a raw `json_body` with Content-Type application/json plus any extra
    // `headers` (used by the Streamkit token endpoint, which wants JSON).
    virtual HttpResponse post_json(const std::string& url, const std::string& json_body,
                                   const std::vector<std::pair<std::string, std::string>>& headers) = 0;
};

// Outcome of a token exchange. On failure, `ok` is false and `error` holds a
// human-useful message (including the HTTP status and a body snippet).
struct TokenResult {
    bool ok = false;
    std::string access_token;
    std::string refresh_token;  // may be empty (Streamkit often omits it)
    std::string error;
};

// Exchange an authorization `code` for an access token: POST JSON {"code":...}
// to kStreamkitTokenUrl.
//
// Returns ok=false (never throws) on a non-200 status, an unparseable body, or
// a response missing `access_token`.
TokenResult exchange_code(HttpPost& http, const std::string& code);

}  // namespace choir
