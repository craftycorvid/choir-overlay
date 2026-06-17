// Tests for choir OAuth code->token exchange (Task 5).
//
// exchange_code() turns an authorization `code` into an access token, via the
// Streamkit hosted token endpoint (default) or the standard Discord token
// endpoint (own-app). All HTTP goes through the injectable HttpPost interface,
// so these tests drive a FakeHttp that records the request and returns a canned
// HttpResponse. No real network, no Qt.

#include "discord/oauth.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <string>
#include <utility>
#include <vector>

using namespace choir;

namespace {

// Records the last request and returns a programmed response.
struct FakeHttp : HttpPost {
    // Programmed response.
    HttpResponse resp{200, "{}"};

    // Captured request.
    std::string last_url;
    std::vector<std::pair<std::string, std::string>> last_form;
    std::vector<std::pair<std::string, std::string>> last_headers;
    std::string last_json_body;   // set when post_json() was used
    bool last_was_json = false;   // which method the last call used
    int call_count = 0;

    HttpResponse post(const std::string& url,
                      const std::vector<std::pair<std::string, std::string>>& form,
                      const std::vector<std::pair<std::string, std::string>>& headers) override {
        ++call_count;
        last_url = url;
        last_form = form;
        last_headers = headers;
        last_was_json = false;
        return resp;
    }

    HttpResponse post_json(const std::string& url, const std::string& json_body,
                           const std::vector<std::pair<std::string, std::string>>& headers) override {
        ++call_count;
        last_url = url;
        last_json_body = json_body;
        last_headers = headers;
        last_was_json = true;
        return resp;
    }

    // Helper: find a form field's value; returns nullptr if absent.
    const std::string* form_value(const std::string& key) const {
        for (const auto& kv : last_form) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }

    bool has_header_containing(const std::string& key, const std::string& needle) const {
        for (const auto& kv : last_headers) {
            if (kv.first == key && kv.second.find(needle) != std::string::npos) return true;
        }
        return false;
    }
};

// ---- Streamkit mode -------------------------------------------------------

void test_streamkit_success() {
    FakeHttp http;
    http.resp = {200, R"({"access_token":"abc","scope":"rpc rpc.voice.read","token_type":"Bearer"})"};

    TokenResult r = exchange_code(http, AuthMode::Streamkit, "the_code", "client123", "secret456");

    assert(r.ok);
    assert(r.access_token == "abc");
    // Streamkit responses typically carry no usable refresh token.
    assert(r.refresh_token.empty());
    assert(r.error.empty());

    // It POSTed JSON to the Streamkit token URL (NOT form-urlencoded — a form
    // body makes the Streamkit worker throw HTTP 500 "error code: 1101").
    assert(http.call_count == 1);
    assert(http.last_was_json);
    assert(http.last_url == kStreamkitTokenUrl);

    // The JSON body is exactly {"code": "<code>"} and nothing else.
    auto body = nlohmann::json::parse(http.last_json_body);
    assert(body.is_object());
    assert(body.value("code", "") == "the_code");
    // Streamkit must NOT leak client_secret / grant_type / client_id.
    assert(!body.contains("client_secret"));
    assert(!body.contains("grant_type"));
    assert(!body.contains("client_id"));
}

void test_streamkit_sends_json_not_form() {
    // Regression for the HTTP 500 / 1101 bug: Streamkit mode must use the JSON
    // POST path, never the form path.
    FakeHttp http;
    http.resp = {200, R"({"access_token":"x"})"};
    exchange_code(http, AuthMode::Streamkit, "c", "id", "");
    assert(http.last_was_json);
    assert(http.last_form.empty());
    // Body is valid JSON.
    assert(!nlohmann::json::parse(http.last_json_body).is_discarded());
}

void test_streamkit_200_empty_object_is_error() {
    // The real Streamkit endpoint returns 200 {} for a bad/expired code. We must
    // treat that as a failure (no access_token), not a crash.
    FakeHttp http;
    http.resp = {200, "{}"};
    TokenResult r = exchange_code(http, AuthMode::Streamkit, "badcode", "id", "");
    assert(!r.ok);
    assert(r.access_token.empty());
    assert(!r.error.empty());
}

void test_streamkit_with_refresh_token() {
    // If a Streamkit-style response DID include a refresh token, we pass it through.
    FakeHttp http;
    http.resp = {200, R"({"access_token":"abc","refresh_token":"r2"})"};

    TokenResult r = exchange_code(http, AuthMode::Streamkit, "c", "id", "");
    assert(r.ok);
    assert(r.access_token == "abc");
    assert(r.refresh_token == "r2");
}

// ---- OwnApp mode ----------------------------------------------------------

void test_ownapp_success() {
    FakeHttp http;
    http.resp = {200, R"({"access_token":"acc_tok","refresh_token":"ref_tok","token_type":"Bearer","expires_in":604800})"};

    TokenResult r = exchange_code(http, AuthMode::OwnApp, "auth_code_xyz",
                                  "my_client_id", "my_client_secret");

    assert(r.ok);
    assert(r.access_token == "acc_tok");
    assert(r.refresh_token == "ref_tok");
    assert(r.error.empty());

    // Hits Discord's standard token endpoint.
    assert(http.call_count == 1);
    assert(http.last_url == "https://discord.com/api/oauth2/token");

    // The form carries the standard authorization_code grant fields.
    const std::string* grant = http.form_value("grant_type");
    assert(grant != nullptr && *grant == "authorization_code");

    const std::string* code = http.form_value("code");
    assert(code != nullptr && *code == "auth_code_xyz");

    const std::string* cid = http.form_value("client_id");
    assert(cid != nullptr && *cid == "my_client_id");

    const std::string* secret = http.form_value("client_secret");
    assert(secret != nullptr && *secret == "my_client_secret");

    assert(http.has_header_containing("Content-Type", "application/x-www-form-urlencoded"));
}

// ---- Error handling -------------------------------------------------------

void test_401_yields_error() {
    FakeHttp http;
    http.resp = {401, R"({"error":"invalid_grant"})"};

    TokenResult r = exchange_code(http, AuthMode::OwnApp, "bad", "id", "secret");
    assert(!r.ok);
    assert(r.access_token.empty());
    assert(!r.error.empty());
    // Useful error: should mention the status.
    assert(r.error.find("401") != std::string::npos);
}

void test_malformed_body_yields_error() {
    FakeHttp http;
    http.resp = {200, "this is not json {{{"};

    TokenResult r = exchange_code(http, AuthMode::Streamkit, "c", "id", "secret");
    assert(!r.ok);
    assert(r.access_token.empty());
    assert(!r.error.empty());
}

void test_200_missing_access_token_yields_error() {
    // Valid JSON, 200, but no access_token field -> error, no crash.
    FakeHttp http;
    http.resp = {200, R"({"token_type":"Bearer","scope":"rpc"})"};

    TokenResult r = exchange_code(http, AuthMode::OwnApp, "c", "id", "secret");
    assert(!r.ok);
    assert(r.access_token.empty());
    assert(!r.error.empty());
}

void test_empty_body_yields_error() {
    FakeHttp http;
    http.resp = {200, ""};

    TokenResult r = exchange_code(http, AuthMode::Streamkit, "c", "id", "secret");
    assert(!r.ok);
    assert(!r.error.empty());
}

}  // namespace

int main() {
    test_streamkit_success();
    test_streamkit_sends_json_not_form();
    test_streamkit_200_empty_object_is_error();
    test_streamkit_with_refresh_token();
    test_ownapp_success();
    test_401_yields_error();
    test_malformed_body_yields_error();
    test_200_missing_access_token_yields_error();
    test_empty_body_yields_error();
    return 0;
}
