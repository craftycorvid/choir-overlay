#include "discord/qt_http.hpp"

#include <QByteArray>
#include <QEventLoop>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QString>
#include <QTimer>
#include <QUrl>

namespace choir {

namespace {

// Run `reply` to completion under a local event loop with a hard timeout.
// Returns true if the reply finished on its own, false if the timeout fired
// (in which case the reply is aborted by the caller's deleteLater path).
bool wait_for_reply(QNetworkReply* reply, int timeout_ms) {
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    bool timed_out = false;

    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, [&]() {
        timed_out = true;
        reply->abort();
        loop.quit();
    });

    timer.start(timeout_ms);
    loop.exec();
    return !timed_out;
}

// Apply extra raw headers, POST the body, wait under a timeout, and read the
// status + body. Shared by the form and JSON POST paths.
HttpResponse run_post(QNetworkAccessManager* nam, QNetworkRequest& req,
                      const std::vector<std::pair<std::string, std::string>>& headers,
                      const std::string& body, int timeout_ms) {
    HttpResponse out;
    for (const auto& h : headers) {
        req.setRawHeader(QByteArray(h.first.c_str(), static_cast<int>(h.first.size())),
                         QByteArray(h.second.c_str(), static_cast<int>(h.second.size())));
    }
    QNetworkReply* reply =
        nam->post(req, QByteArray(body.data(), static_cast<int>(body.size())));

    if (!wait_for_reply(reply, timeout_ms)) {
        out.status = 0;
        out.body = "request timed out";
        reply->deleteLater();
        return out;
    }

    const QVariant code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    out.status = code.isValid() ? code.toInt() : 0;
    const QByteArray data = reply->readAll();
    out.body.assign(data.constData(), static_cast<size_t>(data.size()));
    reply->deleteLater();
    return out;
}

}  // namespace

// ---- QtHttpPost ------------------------------------------------------------

QtHttpPost::QtHttpPost() : nam_(new QNetworkAccessManager()) {}
QtHttpPost::~QtHttpPost() { delete nam_; }

HttpResponse QtHttpPost::post(
    const std::string& url,
    const std::vector<std::pair<std::string, std::string>>& form,
    const std::vector<std::pair<std::string, std::string>>& headers) {
    // Build the application/x-www-form-urlencoded body using the same encoder
    // the OAuth layer expects (choir::url_encode).
    std::string body;
    for (size_t i = 0; i < form.size(); ++i) {
        if (i) body += '&';
        body += url_encode(form[i].first);
        body += '=';
        body += url_encode(form[i].second);
    }

    QNetworkRequest req((QUrl(QString::fromStdString(url))));
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/x-www-form-urlencoded"));
    return run_post(nam_, req, headers, body, timeout_ms_);
}

HttpResponse QtHttpPost::post_json(
    const std::string& url, const std::string& json_body,
    const std::vector<std::pair<std::string, std::string>>& headers) {
    QNetworkRequest req((QUrl(QString::fromStdString(url))));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    return run_post(nam_, req, headers, json_body, timeout_ms_);
}

// ---- QtAvatarSource --------------------------------------------------------

QtAvatarSource::QtAvatarSource() : nam_(new QNetworkAccessManager()) {}
QtAvatarSource::~QtAvatarSource() { delete nam_; }

std::optional<DecodedAvatar> QtAvatarSource::fetch(const std::string& url) {
    QNetworkRequest req((QUrl(QString::fromStdString(url))));
    QNetworkReply* reply = nam_->get(req);

    if (!wait_for_reply(reply, timeout_ms_)) {
        reply->deleteLater();
        return std::nullopt;
    }
    if (reply->error() != QNetworkReply::NoError) {
        reply->deleteLater();
        return std::nullopt;
    }

    const QByteArray data = reply->readAll();
    reply->deleteLater();

    QImage img;
    if (!img.loadFromData(data)) return std::nullopt;

    // Scale to cover 64x64 then crop the centre, and force RGBA8888 so the
    // bytes match the on-disk .rgba format the layer reads.
    QImage scaled =
        img.scaled(64, 64, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const int x = (scaled.width() - 64) / 2;
    const int y = (scaled.height() - 64) / 2;
    QImage cropped = scaled.copy(x, y, 64, 64).convertToFormat(QImage::Format_RGBA8888);
    if (cropped.width() != 64 || cropped.height() != 64) return std::nullopt;

    DecodedAvatar dec;
    dec.w = 64;
    dec.h = 64;
    dec.rgba.resize(64 * 64 * 4);
    for (int row = 0; row < 64; ++row) {
        const uchar* src = cropped.constScanLine(row);
        std::copy(src, src + 64 * 4, dec.rgba.begin() + static_cast<size_t>(row) * 64 * 4);
    }
    return dec;
}

}  // namespace choir
