#pragma once

// Pure parsing/validation helpers for the DJ App local control API
// (docs/decisions/0031). Kept free of QTcpSocket/QTcpServer so the request
// parsing and JSON validation logic can be unit tested without opening any
// real network socket (see src/test/controlapi_test.cpp).
//
// This is intentionally tiny and hand-rolled: it understands exactly the
// one-line-request / headers / optional-body shape the control API needs,
// not general HTTP/1.1.

#include <QByteArray>
#include <QMap>
#include <QString>
#include <optional>

namespace djapp {
namespace controlapi {

struct HttpRequest {
    QString method; // "GET" / "POST"
    QString path; // e.g. "/control" (no query string)
    QMap<QString, QString> query; // decoded query parameters
    QMap<QString, QString> headers; // header name (lowercased) -> value (trimmed)
    QByteArray body;
};

// Parses a decoded query string ("a=1&b=2") into a name->value map. Values
// are percent-decoded; literal '[' ']' (as used by Mixxx group names, e.g.
// "group=[Channel1]") are accepted unescaped since they are not ambiguous
// in this context.
QMap<QString, QString> parseQueryString(const QString& query);

// Parses one full HTTP request already buffered in `raw` (the request
// line, headers, a blank line, and then exactly as many body bytes as the
// caller has read according to Content-Length -- the socket-reading loop
// in controlapiserver.cpp is responsible for reading that many bytes
// before calling this). Returns std::nullopt if the request line or
// headers are malformed.
std::optional<HttpRequest> parseRequest(const QByteArray& raw);

// Host header value (e.g. "127.0.0.1:17000" or "localhost") is loopback-only.
bool isLoopbackHost(const QString& hostHeaderValue);

// Reads the Content-Length header, if present and valid. -1 if absent, or
// nullopt if present but not a valid non-negative integer.
std::optional<qint64> contentLength(const HttpRequest& request);

struct ControlGetQuery {
    QString group;
    QString key;
};

// GET /control?group=..&key=.. -- both are required and non-empty.
std::optional<ControlGetQuery> parseControlGetQuery(
        const QMap<QString, QString>& query, QString* errorOut);

struct ControlSetBody {
    QString group;
    QString key;
    double value = 0.0;
};

// POST /control body: {"group": "...", "key": "...", "value": 1.0}
std::optional<ControlSetBody> parseControlSetBody(const QByteArray& json, QString* errorOut);

struct LoadBody {
    QString group;
    QString location;
};

// POST /load body: {"group": "...", "location": "..."}
std::optional<LoadBody> parseLoadBody(const QByteArray& json, QString* errorOut);

} // namespace controlapi
} // namespace djapp
