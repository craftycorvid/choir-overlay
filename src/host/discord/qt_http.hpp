#pragma once

// Production implementations of the injected host interfaces (Task 11).
//
// These are the ONLY Qt-backed implementations of the otherwise Qt-free seams
// defined in libchoir_host_core:
//   - QtHttpPost   implements choir::HttpPost  (OAuth token exchange, Task 5)
//   - QtAvatarSource implements choir::AvatarSource (avatar fetch+decode, Task 9)
//
// Both run synchronously (a local QEventLoop blocks until the network reply
// finishes) with a hard timeout so neither can hang the host. They live in the
// `choir` executable target, never in libchoir_host_core.

#include "discord/oauth.hpp"        // choir::HttpPost
#include "model/avatar_cache.hpp"   // choir::AvatarSource

#include <QtGlobal>

#include <optional>
#include <string>

QT_BEGIN_NAMESPACE
class QNetworkAccessManager;
QT_END_NAMESPACE

namespace choir {

// QNetworkAccessManager-backed synchronous HTTP POST.
class QtHttpPost : public HttpPost {
public:
    QtHttpPost();
    ~QtHttpPost() override;

    HttpResponse post(const std::string& url,
                      const std::vector<std::pair<std::string, std::string>>& form,
                      const std::vector<std::pair<std::string, std::string>>& headers) override;

    HttpResponse post_json(const std::string& url, const std::string& json_body,
                           const std::vector<std::pair<std::string, std::string>>& headers) override;

    // Network timeout in milliseconds (default 10s).
    void set_timeout_ms(int ms) { timeout_ms_ = ms; }

private:
    QNetworkAccessManager* nam_ = nullptr;
    int timeout_ms_ = 10000;
};

// QNetworkAccessManager + QImage avatar fetcher. Fetches the URL, decodes the
// image, scales/crops to exactly 64x64 RGBA8, returns nullopt on any failure.
class QtAvatarSource : public AvatarSource {
public:
    QtAvatarSource();
    ~QtAvatarSource() override;

    std::optional<DecodedAvatar> fetch(const std::string& url) override;

    void set_timeout_ms(int ms) { timeout_ms_ = ms; }

private:
    QNetworkAccessManager* nam_ = nullptr;
    int timeout_ms_ = 10000;
};

}  // namespace choir
