#include "network/controlapiserver.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QTcpServer>
#include <QTcpSocket>

#include "control/controlobject.h"
#include "network/controlapibridge.h"
#include "network/controlapiparsing.h"
#include "util/logger.h"
#include "util/versionstore.h"

using djapp::controlapi::ControlGetQuery;
using djapp::controlapi::ControlSetBody;
using djapp::controlapi::HttpRequest;
using djapp::controlapi::LoadBody;

namespace {
const mixxx::Logger kLogger("ControlApiServer");

constexpr int kAcceptWaitMs = 200;
constexpr int kReadWaitMs = 2000;
constexpr int kWriteWaitMs = 2000;
constexpr int kMaxRequestBytes = 1 << 20; // 1 MiB is more than enough for this API.

QByteArray jsonResponse(int statusCode, const QString& statusText, const QJsonObject& body) {
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    QByteArray response;
    response += "HTTP/1.1 " + QByteArray::number(statusCode) + " " + statusText.toUtf8() +
            "\r\n";
    response += "Content-Type: application/json\r\n";
    response += "Content-Length: " + QByteArray::number(payload.size()) + "\r\n";
    response += "Connection: close\r\n";
    response += "\r\n";
    response += payload;
    return response;
}

QByteArray okJson(const QJsonObject& body) {
    return jsonResponse(200, QStringLiteral("OK"), body);
}

QByteArray errorJson(int statusCode, const QString& statusText, const QString& message) {
    QJsonObject obj;
    obj.insert(QStringLiteral("error"), message);
    return jsonResponse(statusCode, statusText, obj);
}

QByteArray badRequest(const QString& message) {
    return errorJson(400, QStringLiteral("Bad Request"), message);
}

QByteArray notFound(const QString& message) {
    return errorJson(404, QStringLiteral("Not Found"), message);
}

QByteArray internalError(const QString& message) {
    return errorJson(500, QStringLiteral("Internal Server Error"), message);
}

// Reads one deck's handful of control values for GET /state.
QJsonObject deckState(const QString& group, ControlApiBridge* pBridge) {
    QJsonObject obj;
    QString location;
    if (pBridge) {
        QMetaObject::invokeMethod(pBridge,
                "getDeckTrackLocation",
                Qt::BlockingQueuedConnection,
                Q_RETURN_ARG(QString, location),
                Q_ARG(QString, group));
    }
    obj.insert(QStringLiteral("location"), location);
    obj.insert(QStringLiteral("play"),
            ControlObject::exists(ConfigKey(group, QStringLiteral("play")))
                    ? ControlObject::get(ConfigKey(group, QStringLiteral("play")))
                    : 0.0);
    obj.insert(QStringLiteral("position"),
            ControlObject::exists(ConfigKey(group, QStringLiteral("playposition")))
                    ? ControlObject::get(ConfigKey(group, QStringLiteral("playposition")))
                    : 0.0);
    obj.insert(QStringLiteral("duration"),
            ControlObject::exists(ConfigKey(group, QStringLiteral("duration")))
                    ? ControlObject::get(ConfigKey(group, QStringLiteral("duration")))
                    : 0.0);
    obj.insert(QStringLiteral("bpm"),
            ControlObject::exists(ConfigKey(group, QStringLiteral("bpm")))
                    ? ControlObject::get(ConfigKey(group, QStringLiteral("bpm")))
                    : 0.0);
    obj.insert(QStringLiteral("key"),
            ControlObject::exists(ConfigKey(group, QStringLiteral("visual_key")))
                    ? ControlObject::get(ConfigKey(group, QStringLiteral("visual_key")))
                    : 0.0);
    return obj;
}

QByteArray handleHealth() {
    QJsonObject obj;
    obj.insert(QStringLiteral("ok"), true);
    obj.insert(QStringLiteral("version"), VersionStore::version());
    return okJson(obj);
}

QByteArray handleControlGet(const HttpRequest& request) {
    QString error;
    const auto parsed = djapp::controlapi::parseControlGetQuery(request.query, &error);
    if (!parsed) {
        return badRequest(error);
    }
    const ConfigKey key(parsed->group, parsed->key);
    if (!ControlObject::exists(key)) {
        return notFound(QStringLiteral("no control named %1,%2").arg(parsed->group, parsed->key));
    }
    QJsonObject obj;
    obj.insert(QStringLiteral("group"), parsed->group);
    obj.insert(QStringLiteral("key"), parsed->key);
    obj.insert(QStringLiteral("value"), ControlObject::get(key));
    return okJson(obj);
}

QByteArray handleControlPost(const HttpRequest& request) {
    QString error;
    const auto parsed = djapp::controlapi::parseControlSetBody(request.body, &error);
    if (!parsed) {
        return badRequest(error);
    }
    const ConfigKey key(parsed->group, parsed->key);
    if (!ControlObject::exists(key)) {
        return notFound(QStringLiteral("no control named %1,%2").arg(parsed->group, parsed->key));
    }
    // The real set path (same as a MIDI mapping or a skin button): every
    // normal effect of this control firing still happens (automix refusal
    // rules included), nothing here bypasses it.
    ControlObject::set(key, parsed->value);
    QJsonObject obj;
    obj.insert(QStringLiteral("ok"), true);
    obj.insert(QStringLiteral("group"), parsed->group);
    obj.insert(QStringLiteral("key"), parsed->key);
    obj.insert(QStringLiteral("value"), ControlObject::get(key));
    return okJson(obj);
}

QByteArray handleState(ControlApiBridge* pBridge) {
    QJsonObject obj;
    obj.insert(QStringLiteral("deck1"), deckState(QStringLiteral("[Channel1]"), pBridge));
    obj.insert(QStringLiteral("deck2"), deckState(QStringLiteral("[Channel2]"), pBridge));

    QJsonObject automix;
    const ConfigKey stateKey(QStringLiteral("[AutomixTransition]"), QStringLiteral("state"));
    const ConfigKey countdownKey(
            QStringLiteral("[AutomixTransition]"), QStringLiteral("countdown"));
    automix.insert(QStringLiteral("state"),
            ControlObject::exists(stateKey) ? ControlObject::get(stateKey) : 0.0);
    automix.insert(QStringLiteral("countdown"),
            ControlObject::exists(countdownKey) ? ControlObject::get(countdownKey) : 0.0);
    obj.insert(QStringLiteral("automix"), automix);

    QJsonObject autopilot;
    const ConfigKey enabledKey(QStringLiteral("[DJAppAutomix]"), QStringLiteral("enabled"));
    autopilot.insert(QStringLiteral("enabled"),
            ControlObject::exists(enabledKey) ? (ControlObject::get(enabledKey) > 0.0) : false);
    obj.insert(QStringLiteral("autopilot"), autopilot);

    return okJson(obj);
}

QByteArray handleLoad(const HttpRequest& request, ControlApiBridge* pBridge) {
    QString error;
    const auto parsed = djapp::controlapi::parseLoadBody(request.body, &error);
    if (!parsed) {
        return badRequest(error);
    }
    if (!pBridge) {
        return internalError(QStringLiteral("player manager not available"));
    }
    // Fire-and-forget on the main thread, exactly PlayerManager's own load
    // path -- not a reimplementation.
    QMetaObject::invokeMethod(pBridge,
            "loadLocation",
            Qt::QueuedConnection,
            Q_ARG(QString, parsed->group),
            Q_ARG(QString, parsed->location),
            Q_ARG(bool, false));
    QJsonObject obj;
    obj.insert(QStringLiteral("ok"), true);
    obj.insert(QStringLiteral("queued"), true);
    obj.insert(QStringLiteral("group"), parsed->group);
    obj.insert(QStringLiteral("location"), parsed->location);
    return okJson(obj);
}

QByteArray routeRequest(const HttpRequest& request, ControlApiBridge* pBridge) {
    const QString hostHeader = request.headers.value(QStringLiteral("host"));
    if (!djapp::controlapi::isLoopbackHost(hostHeader)) {
        return badRequest(QStringLiteral("Host header must be loopback"));
    }

    if (request.method == QStringLiteral("GET") && request.path == QStringLiteral("/health")) {
        return handleHealth();
    }
    if (request.method == QStringLiteral("GET") && request.path == QStringLiteral("/control")) {
        return handleControlGet(request);
    }
    if (request.method == QStringLiteral("POST") && request.path == QStringLiteral("/control")) {
        return handleControlPost(request);
    }
    if (request.method == QStringLiteral("GET") && request.path == QStringLiteral("/state")) {
        return handleState(pBridge);
    }
    if (request.method == QStringLiteral("POST") && request.path == QStringLiteral("/load")) {
        return handleLoad(request, pBridge);
    }
    return notFound(QStringLiteral("no such endpoint"));
}

} // namespace

ControlApiServer::ControlApiServer(quint16 port, ControlApiBridge* pBridge)
        : m_port(port),
          m_pBridge(pBridge) {
}

ControlApiServer::~ControlApiServer() {
    stop();
}

bool ControlApiServer::start() {
    if (m_pThread) {
        return m_listening.load();
    }
    m_stopRequested.store(false);

    // QTcpServer must be created inside the thread that will use it, so
    // the actual listen() call happens in run(); below we just wait for it
    // to flip m_listening (or give up and report failure).
    m_pThread = std::make_unique<std::thread>([this]() { run(); });

    // Wait briefly for the listen result; run() flips m_listening (and
    // m_boundPort) before entering its accept loop.
    QElapsedTimer waited;
    waited.start();
    while (!m_listening.load() && waited.elapsed() < 2000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (m_stopRequested.load()) {
            break;
        }
    }
    return m_listening.load();
}

void ControlApiServer::stop() {
    m_stopRequested.store(true);
    if (m_pThread) {
        m_pThread->join();
        m_pThread.reset();
    }
    m_listening.store(false);
}

void ControlApiServer::run() {
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, m_port)) {
        kLogger.warning() << "DJ App control API: failed to listen on 127.0.0.1:" << m_port
                           << "--" << server.errorString() << "(control API disabled)";
        m_listening.store(false);
        m_stopRequested.store(true);
        return;
    }
    m_boundPort.store(server.serverPort());
    m_listening.store(true);
    kLogger.info() << "DJ App control API: listening on 127.0.0.1:" << server.serverPort();

    while (!m_stopRequested.load()) {
        if (!server.waitForNewConnection(kAcceptWaitMs)) {
            continue;
        }
        QTcpSocket* pSocket = server.nextPendingConnection();
        if (!pSocket) {
            continue;
        }
        if (pSocket->peerAddress() != QHostAddress(QHostAddress::LocalHost) &&
                pSocket->peerAddress() != QHostAddress(QHostAddress::LocalHostIPv6)) {
            // Belt-and-suspenders: the listen socket itself is already
            // loopback-only, so this should be unreachable.
            pSocket->close();
            delete pSocket;
            continue;
        }
        handleConnection(pSocket);
        delete pSocket;
    }
    kLogger.info() << "DJ App control API: stopped";
}

void ControlApiServer::handleConnection(QTcpSocket* pSocket) {
    try {
        QByteArray buffer;
        QElapsedTimer timer;
        timer.start();

        int headerEnd = -1;
        qint64 wantTotal = -1; // -1 until known (headers not fully read yet)

        while (timer.elapsed() < kReadWaitMs) {
            if (pSocket->bytesAvailable() > 0 || pSocket->waitForReadyRead(50)) {
                buffer += pSocket->readAll();
            }
            if (buffer.size() > kMaxRequestBytes) {
                pSocket->write(badRequest(QStringLiteral("request too large")));
                pSocket->waitForBytesWritten(kWriteWaitMs);
                return;
            }
            if (headerEnd < 0) {
                headerEnd = buffer.indexOf("\r\n\r\n");
            }
            if (headerEnd >= 0 && wantTotal < 0) {
                const auto parsedForLength = djapp::controlapi::parseRequest(buffer);
                if (!parsedForLength) {
                    pSocket->write(badRequest(QStringLiteral("malformed request")));
                    pSocket->waitForBytesWritten(kWriteWaitMs);
                    return;
                }
                const auto len = djapp::controlapi::contentLength(*parsedForLength);
                if (!len) {
                    pSocket->write(badRequest(QStringLiteral("invalid Content-Length")));
                    pSocket->waitForBytesWritten(kWriteWaitMs);
                    return;
                }
                const qint64 bodyLen = *len < 0 ? 0 : *len;
                wantTotal = headerEnd + 4 + bodyLen;
            }
            if (wantTotal >= 0 && buffer.size() >= wantTotal) {
                break;
            }
            if (pSocket->state() != QAbstractSocket::ConnectedState &&
                    buffer.size() >= (headerEnd < 0 ? 0 : headerEnd + 4)) {
                break;
            }
        }

        const auto requestOpt = djapp::controlapi::parseRequest(buffer);
        if (!requestOpt) {
            pSocket->write(badRequest(QStringLiteral("malformed or incomplete request")));
            pSocket->waitForBytesWritten(kWriteWaitMs);
            return;
        }

        const QByteArray response = routeRequest(*requestOpt, m_pBridge);
        pSocket->write(response);
        pSocket->waitForBytesWritten(kWriteWaitMs);
    } catch (...) {
        kLogger.warning() << "DJ App control API: request handler threw, dropping connection";
        const QByteArray response = internalError(QStringLiteral("internal error"));
        pSocket->write(response);
        pSocket->waitForBytesWritten(kWriteWaitMs);
    }
}
