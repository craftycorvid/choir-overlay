#pragma once

// OAuth2 code -> access-token exchange (Task 5).
//
// After the RPC AUTHORIZE command returns an authorization `code`, the host
// trades it for an access token. Two modes:
//   - Streamkit (default): POST the code to Discord's hosted Streamkit token
//     endpoint, which holds the client secret. This is how the "Discover"
//     overlay works; the user never registers an app.
//   - OwnApp: the user registered their own Discord application, so we do the
//     standard authorization_code exchange with their client_id/client_secret.
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

// The standard Discord OAuth2 token endpoint (used in OwnApp mode).
constexpr const char* kDiscordTokenUrl = "https://discord.com/api/oauth2/token";

// Result of a single HTTP POST.
struct HttpResponse {
    int status = 0;
    std::string body;
};

// Injectable HTTP POST seam.
struct HttpPost {
    virtual ~HttpPost() = default;
    // Send `form` as application/x-www-form-urlencoded with the given extra
    // `headers` (used by the standard Discord token endpoint, OwnApp mode).
    virtual HttpResponse post(const std::string& url,
                              const std::vector<std::pair<std::string, std::string>>& form,
                              const std::vector<std::pair<std::string, std::string>>& headers) = 0;
    // Send a raw `json_body` with Content-Type application/json plus any extra
    // `headers` (used by the Streamkit token endpoint, which wants JSON).
    virtual HttpResponse post_json(const std::string& url, const std::string& json_body,
                                   const std::vector<std::pair<std::string, std::string>>& headers) = 0;
};

enum class AuthMode { Streamkit, OwnApp };

// Outcome of a token exchange. On failure, `ok` is false and `error` holds a
// human-useful message (including the HTTP status and a body snippet).
struct TokenResult {
    bool ok = false;
    std::string access_token;
    std::string refresh_token;  // may be empty (Streamkit often omits it)
    std::string error;
};

// Exchange an authorization `code` for an access token.
//
// Streamkit: POST JSON {"code":...} to kStreamkitTokenUrl.
// OwnApp:    POST form {grant_type=authorization_code, code, client_id, client_secret}
//            to kDiscordTokenUrl.
//
// Returns ok=false (never throws) on a non-200 status, an unparseable body, or
// a response missing `access_token`.
TokenResult exchange_code(HttpPost& http, AuthMode mode, const std::string& code,
                          const std::string& client_id, const std::string& client_secret);

// Percent-encode a value for application/x-www-form-urlencoded bodies. Exposed
// so the production HttpPost (Task 11) can build the body identically. Encodes
// every byte outside the RFC 3986 unreserved set (ALPHA / DIGIT / - _ . ~) as
// %XX, including spaces (%20), which Discord's token endpoint accepts.
std::string url_encode(const std::string& value);

}  // namespace choir
