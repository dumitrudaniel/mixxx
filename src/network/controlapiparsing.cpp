#include "network/controlapiparsing.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <QUrl>

namespace djapp {
namespace controlapi {

QMap<QString, QString> parseQueryString(const QString& query) {
    QMap<QString, QString> result;
    if (query.isEmpty()) {
        return result;
    }
    const QStringList pairs = query.split(QChar('&'), Qt::SkipEmptyParts);
    for (const QString& pair : pairs) {
        const int eq = pair.indexOf(QChar('='));
        QString rawKey = eq >= 0 ? pair.left(eq) : pair;
        QString rawValue = eq >= 0 ? pair.mid(eq + 1) : QString();
        const QString key = QUrl::fromPercentEncoding(rawKey.toUtf8());
        // Mixxx group names contain '[' ']' and are routinely passed
        // unescaped in a quick curl call; QUrl::fromPercentEncoding leaves
        // them untouched (they are not '%XX' sequences), so this is safe
        // for both the escaped and unescaped forms.
        const QString value = QUrl::fromPercentEncoding(rawValue.toUtf8());
        if (!key.isEmpty()) {
            result.insert(key, value);
        }
    }
    return result;
}

std::optional<HttpRequest> parseRequest(const QByteArray& raw) {
    const int headerEnd = raw.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        return std::nullopt;
    }
    const QByteArray headerPart = raw.left(headerEnd);
    const QByteArray bodyPart = raw.mid(headerEnd + 4);

    const QList<QByteArray> lines = headerPart.split('\n');
    if (lines.isEmpty()) {
        return std::nullopt;
    }

    QByteArray requestLine = lines.first();
    if (requestLine.endsWith('\r')) {
        requestLine.chop(1);
    }
    const QList<QByteArray> requestParts = requestLine.split(' ');
    if (requestParts.size() < 2) {
        return std::nullopt;
    }

    HttpRequest request;
    request.method = QString::fromLatin1(requestParts.at(0)).toUpper();

    const QString target = QString::fromUtf8(requestParts.at(1));
    const int queryStart = target.indexOf(QChar('?'));
    if (queryStart >= 0) {
        request.path = target.left(queryStart);
        request.query = parseQueryString(target.mid(queryStart + 1));
    } else {
        request.path = target;
    }

    for (int i = 1; i < lines.size(); ++i) {
        QByteArray line = lines.at(i);
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        if (line.isEmpty()) {
            continue;
        }
        const int colon = line.indexOf(':');
        if (colon < 0) {
            continue; // Ignore malformed header lines rather than failing the request.
        }
        const QString name = QString::fromLatin1(line.left(colon)).trimmed().toLower();
        const QString value = QString::fromUtf8(line.mid(colon + 1)).trimmed();
        if (!name.isEmpty()) {
            request.headers.insert(name, value);
        }
    }

    request.body = bodyPart;
    return request;
}

bool isLoopbackHost(const QString& hostHeaderValue) {
    QString host = hostHeaderValue.trimmed();
    if (host.isEmpty()) {
        // No Host header at all: be permissive (some minimal HTTP/1.0
        // clients omit it); the socket itself only ever accepts loopback
        // connections (see ControlApiServer), which is the real guard.
        return true;
    }
    // Strip a trailing ":<port>", but keep IPv6 literals ("[::1]:17000") intact.
    if (host.startsWith(QChar('['))) {
        const int close = host.indexOf(QChar(']'));
        if (close >= 0) {
            host = host.left(close + 1);
        }
    } else {
        const int colon = host.lastIndexOf(QChar(':'));
        if (colon >= 0) {
            host = host.left(colon);
        }
    }
    host = host.toLower();
    return host == QLatin1String("127.0.0.1") || host == QLatin1String("localhost") ||
            host == QLatin1String("[::1]") || host == QLatin1String("::1");
}

std::optional<qint64> contentLength(const HttpRequest& request) {
    const auto it = request.headers.constFind(QStringLiteral("content-length"));
    if (it == request.headers.constEnd()) {
        return -1;
    }
    bool ok = false;
    const qint64 value = it.value().toLongLong(&ok);
    if (!ok || value < 0) {
        return std::nullopt;
    }
    return value;
}

std::optional<ControlGetQuery> parseControlGetQuery(
        const QMap<QString, QString>& query, QString* errorOut) {
    const QString group = query.value(QStringLiteral("group"));
    const QString key = query.value(QStringLiteral("key"));
    if (group.isEmpty() || key.isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral("'group' and 'key' query parameters are required");
        }
        return std::nullopt;
    }
    ControlGetQuery result;
    result.group = group;
    result.key = key;
    return result;
}

namespace {
std::optional<QJsonObject> parseJsonObject(const QByteArray& json, QString* errorOut) {
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (errorOut) {
            *errorOut = QStringLiteral("invalid JSON: %1").arg(parseError.errorString());
        }
        return std::nullopt;
    }
    if (!doc.isObject()) {
        if (errorOut) {
            *errorOut = QStringLiteral("JSON body must be an object");
        }
        return std::nullopt;
    }
    return doc.object();
}
} // namespace

std::optional<ControlSetBody> parseControlSetBody(const QByteArray& json, QString* errorOut) {
    const auto objOpt = parseJsonObject(json, errorOut);
    if (!objOpt) {
        return std::nullopt;
    }
    const QJsonObject obj = *objOpt;
    const QJsonValue groupValue = obj.value(QStringLiteral("group"));
    const QJsonValue keyValue = obj.value(QStringLiteral("key"));
    const QJsonValue valueValue = obj.value(QStringLiteral("value"));
    if (!groupValue.isString() || groupValue.toString().isEmpty() || !keyValue.isString() ||
            keyValue.toString().isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral(
                    "body must have non-empty string fields 'group' and 'key'");
        }
        return std::nullopt;
    }
    if (!valueValue.isDouble()) {
        if (errorOut) {
            *errorOut = QStringLiteral("body field 'value' must be a number");
        }
        return std::nullopt;
    }
    ControlSetBody result;
    result.group = groupValue.toString();
    result.key = keyValue.toString();
    result.value = valueValue.toDouble();
    return result;
}

std::optional<LoadBody> parseLoadBody(const QByteArray& json, QString* errorOut) {
    const auto objOpt = parseJsonObject(json, errorOut);
    if (!objOpt) {
        return std::nullopt;
    }
    const QJsonObject obj = *objOpt;
    const QJsonValue groupValue = obj.value(QStringLiteral("group"));
    const QJsonValue locationValue = obj.value(QStringLiteral("location"));
    if (!groupValue.isString() || groupValue.toString().isEmpty() ||
            !locationValue.isString() || locationValue.toString().isEmpty()) {
        if (errorOut) {
            *errorOut = QStringLiteral(
                    "body must have non-empty string fields 'group' and 'location'");
        }
        return std::nullopt;
    }
    LoadBody result;
    result.group = groupValue.toString();
    result.location = locationValue.toString();
    return result;
}

} // namespace controlapi
} // namespace djapp
