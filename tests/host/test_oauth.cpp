// Tests for choir OAuth code->token exchange (Task 5).
//
// exchange_code() turns an authorization `code` into an access token via the
// Streamkit hosted token endpoint. All HTTP goes through the injectable HttpPost
// interface, so these tests drive a FakeHttp that records the request and returns
// a canned HttpResponse. No real network, no Qt.

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
    std::vector<std::pair<std::string, std::string>> last_headers;
    std::string last_json_body;
    int call_count = 0;

    HttpResponse post_json(const std::string& url, const std::string& json_body,
                           const std::vector<std::pair<std::string, std::string>>& headers) override {
        ++call_count;
        last_url = url;
        last_json_body = json_body;
        last_headers = headers;
        return resp;
    }
};

// ---- Streamkit mode -------------------------------------------------------

void test_streamkit_success() {
    FakeHttp http;
    http.resp = {200, R"({"access_token":"abc","scope":"rpc rpc.voice.read","token_type":"Bearer"})"};

    TokenResult r = exchange_code(http, "the_code");

    assert(r.ok);
    assert(r.access_token == "abc");
    // Streamkit responses typically carry no usable refresh token.
    assert(r.refresh_token.empty());
    assert(r.error.empty());

    // It POSTed JSON to the Streamkit token URL.
    assert(http.call_count == 1);
    assert(http.last_url == kStreamkitTokenUrl);

    // The JSON body is exactly {"code": "<code>"} and nothing else (a form body
    // makes the Streamkit worker throw HTTP 500 "error code: 1101").
    auto body = nlohmann::json::parse(http.last_json_body);
    assert(body.is_object());
    assert(body.value("code", "") == "the_code");
    assert(!body.contains("client_secret"));
    assert(!body.contains("grant_type"));
    assert(!body.contains("client_id"));
}

void test_200_empty_object_is_error() {
    // The real Streamkit endpoint returns 200 {} for a bad/expired code. We must
    // treat that as a failure (no access_token), not a crash.
    FakeHttp http;
    http.resp = {200, "{}"};
    TokenResult r = exchange_code(http, "badcode");
    assert(!r.ok);
    assert(r.access_token.empty());
    assert(!r.error.empty());
}

void test_with_refresh_token() {
    // If a Streamkit-style response DID include a refresh token, we pass it through.
    FakeHttp http;
    http.resp = {200, R"({"access_token":"abc","refresh_token":"r2"})"};

    TokenResult r = exchange_code(http, "c");
    assert(r.ok);
    assert(r.access_token == "abc");
    assert(r.refresh_token == "r2");
}

// ---- Error handling -------------------------------------------------------

void test_401_yields_error() {
    FakeHttp http;
    http.resp = {401, R"({"error":"invalid_grant"})"};

    TokenResult r = exchange_code(http, "bad");
    assert(!r.ok);
    assert(r.access_token.empty());
    assert(!r.error.empty());
    // Useful error: should mention the status.
    assert(r.error.find("401") != std::string::npos);
}

void test_malformed_body_yields_error() {
    FakeHttp http;
    http.resp = {200, "this is not json {{{"};

    TokenResult r = exchange_code(http, "c");
    assert(!r.ok);
    assert(r.access_token.empty());
    assert(!r.error.empty());
}

void test_200_missing_access_token_yields_error() {
    // Valid JSON, 200, but no access_token field -> error, no crash.
    FakeHttp http;
    http.resp = {200, R"({"token_type":"Bearer","scope":"rpc"})"};

    TokenResult r = exchange_code(http, "c");
    assert(!r.ok);
    assert(r.access_token.empty());
    assert(!r.error.empty());
}

void test_empty_body_yields_error() {
    FakeHttp http;
    http.resp = {200, ""};

    TokenResult r = exchange_code(http, "c");
    assert(!r.ok);
    assert(!r.error.empty());
}

}  // namespace

int main() {
    test_streamkit_success();
    test_200_empty_object_is_error();
    test_with_refresh_token();
    test_401_yields_error();
    test_malformed_body_yields_error();
    test_200_missing_access_token_yields_error();
    test_empty_body_yields_error();
    return 0;
}
