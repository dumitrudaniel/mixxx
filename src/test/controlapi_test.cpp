#include "network/controlapiserver.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>

#include "network/controlapiparsing.h"
#include "test/mixxxtest.h"

using namespace djapp::controlapi;

namespace {

// ---- Pure parsing / validation tests (no socket involved) ----------------

TEST(ControlApiParsingTest, ParseQueryStringBasic) {
    const auto query = parseQueryString(QStringLiteral("group=%5BChannel1%5D&key=play"));
    EXPECT_EQ(query.value(QStringLiteral("group")), QStringLiteral("[Channel1]"));
    EXPECT_EQ(query.value(QStringLiteral("key")), QStringLiteral("play"));
}

TEST(ControlApiParsingTest, ParseQueryStringUnescapedBrackets) {
    // Dan's own quick curl calls routinely leave '[' ']' unescaped.
    const auto query = parseQueryString(QStringLiteral("group=[Channel1]&key=play"));
    EXPECT_EQ(query.value(QStringLiteral("group")), QStringLiteral("[Channel1]"));
    EXPECT_EQ(query.value(QStringLiteral("key")), QStringLiteral("play"));
}

TEST(ControlApiParsingTest, ParseQueryStringEmpty) {
    EXPECT_TRUE(parseQueryString(QString()).isEmpty());
}

TEST(ControlApiParsingTest, ParseRequestGetWithQuery) {
    const QByteArray raw =
            "GET /control?group=[Channel1]&key=play HTTP/1.1\r\n"
            "Host: 127.0.0.1:17000\r\n"
            "\r\n";
    const auto request = parseRequest(raw);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->method, QStringLiteral("GET"));
    EXPECT_EQ(request->path, QStringLiteral("/control"));
    EXPECT_EQ(request->query.value(QStringLiteral("group")), QStringLiteral("[Channel1]"));
    EXPECT_EQ(request->headers.value(QStringLiteral("host")),
            QStringLiteral("127.0.0.1:17000"));
    EXPECT_TRUE(request->body.isEmpty());
}

TEST(ControlApiParsingTest, ParseRequestPostWithBody) {
    const QByteArray body = "{\"group\":\"[Channel1]\",\"key\":\"play\",\"value\":1.0}";
    QByteArray raw = "POST /control HTTP/1.1\r\n";
    raw += "Host: 127.0.0.1:17000\r\n";
    raw += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    raw += "\r\n";
    raw += body;

    const auto request = parseRequest(raw);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->method, QStringLiteral("POST"));
    EXPECT_EQ(request->path, QStringLiteral("/control"));
    EXPECT_EQ(request->body, body);

    const auto length = contentLength(*request);
    ASSERT_TRUE(length.has_value());
    EXPECT_EQ(*length, body.size());
}

TEST(ControlApiParsingTest, ParseRequestMissingHeaderBlankLineFails) {
    const QByteArray raw = "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    EXPECT_FALSE(parseRequest(raw).has_value());
}

TEST(ControlApiParsingTest, ParseRequestMalformedRequestLineFails) {
    const QByteArray raw = "NOTHTTP\r\n\r\n";
    EXPECT_FALSE(parseRequest(raw).has_value());
}

TEST(ControlApiParsingTest, ContentLengthDefaultsToMinusOneWhenAbsent) {
    const QByteArray raw = "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    const auto request = parseRequest(raw);
    ASSERT_TRUE(request.has_value());
    const auto length = contentLength(*request);
    ASSERT_TRUE(length.has_value());
    EXPECT_EQ(*length, -1);
}

TEST(ControlApiParsingTest, ContentLengthInvalidFails) {
    const QByteArray raw = "GET /health HTTP/1.1\r\nContent-Length: notanumber\r\n\r\n";
    const auto request = parseRequest(raw);
    ASSERT_TRUE(request.has_value());
    EXPECT_FALSE(contentLength(*request).has_value());
}

TEST(ControlApiParsingTest, IsLoopbackHostAccepts) {
    EXPECT_TRUE(isLoopbackHost(QStringLiteral("127.0.0.1")));
    EXPECT_TRUE(isLoopbackHost(QStringLiteral("127.0.0.1:17000")));
    EXPECT_TRUE(isLoopbackHost(QStringLiteral("localhost")));
    EXPECT_TRUE(isLoopbackHost(QStringLiteral("localhost:17000")));
    EXPECT_TRUE(isLoopbackHost(QStringLiteral("[::1]:17000")));
    EXPECT_TRUE(isLoopbackHost(QString())); // absent Host header: permissive
}

TEST(ControlApiParsingTest, IsLoopbackHostRejectsRemote) {
    EXPECT_FALSE(isLoopbackHost(QStringLiteral("example.com")));
    EXPECT_FALSE(isLoopbackHost(QStringLiteral("192.168.1.5:17000")));
}

TEST(ControlApiParsingTest, ParseControlGetQueryRequiresBoth) {
    QString error;
    QMap<QString, QString> query;
    query.insert(QStringLiteral("group"), QStringLiteral("[Channel1]"));
    EXPECT_FALSE(parseControlGetQuery(query, &error).has_value());
    EXPECT_FALSE(error.isEmpty());

    query.insert(QStringLiteral("key"), QStringLiteral("play"));
    const auto parsed = parseControlGetQuery(query, &error);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->group, QStringLiteral("[Channel1]"));
    EXPECT_EQ(parsed->key, QStringLiteral("play"));
}

TEST(ControlApiParsingTest, ParseControlSetBodyValid) {
    QString error;
    const QByteArray json = "{\"group\":\"[Channel1]\",\"key\":\"pfl\",\"value\":1}";
    const auto parsed = parseControlSetBody(json, &error);
    ASSERT_TRUE(parsed.has_value()) << error.toStdString();
    EXPECT_EQ(parsed->group, QStringLiteral("[Channel1]"));
    EXPECT_EQ(parsed->key, QStringLiteral("pfl"));
    EXPECT_DOUBLE_EQ(parsed->value, 1.0);
}

TEST(ControlApiParsingTest, ParseControlSetBodyRejectsMalformedJson) {
    QString error;
    EXPECT_FALSE(parseControlSetBody("not json", &error).has_value());
    EXPECT_FALSE(error.isEmpty());
}

TEST(ControlApiParsingTest, ParseControlSetBodyRejectsMissingFields) {
    QString error;
    EXPECT_FALSE(parseControlSetBody("{\"group\":\"[Channel1]\"}", &error).has_value());
    EXPECT_FALSE(error.isEmpty());
}

TEST(ControlApiParsingTest, ParseControlSetBodyRejectsNonNumericValue) {
    QString error;
    const QByteArray json = "{\"group\":\"[Channel1]\",\"key\":\"play\",\"value\":\"on\"}";
    EXPECT_FALSE(parseControlSetBody(json, &error).has_value());
}

TEST(ControlApiParsingTest, ParseLoadBodyValid) {
    QString error;
    const QByteArray json =
            "{\"group\":\"[Channel1]\",\"location\":\"C:/music/track.mp3\"}";
    const auto parsed = parseLoadBody(json, &error);
    ASSERT_TRUE(parsed.has_value()) << error.toStdString();
    EXPECT_EQ(parsed->group, QStringLiteral("[Channel1]"));
    EXPECT_EQ(parsed->location, QStringLiteral("C:/music/track.mp3"));
}

TEST(ControlApiParsingTest, ParseLoadBodyRejectsMissingLocation) {
    QString error;
    EXPECT_FALSE(parseLoadBody("{\"group\":\"[Channel1]\"}", &error).has_value());
}

// ---- Real end-to-end round trip: start the server, do an actual GET ------

class ControlApiServerTest : public MixxxTest {};

TEST_F(ControlApiServerTest, HealthEndToEnd) {
    // Port 0: let the OS pick an ephemeral free port, so the test never
    // collides with a real running Mixxx instance (default 17000) or with
    // another test run.
    ControlApiServer server(0, nullptr);
    ASSERT_TRUE(server.start());
    ASSERT_TRUE(server.isListening());
    const quint16 port = server.boundPort();
    ASSERT_NE(port, 0);

    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, port);
    ASSERT_TRUE(socket.waitForConnected(2000));

    const QByteArray requestLine = "GET /health HTTP/1.1\r\n"
                                    "Host: 127.0.0.1\r\n"
                                    "Connection: close\r\n"
                                    "\r\n";
    socket.write(requestLine);
    ASSERT_TRUE(socket.waitForBytesWritten(2000));

    QByteArray response;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 3000) {
        if (socket.waitForReadyRead(200)) {
            response += socket.readAll();
        }
        if (socket.state() == QAbstractSocket::UnconnectedState && !response.isEmpty()) {
            break;
        }
    }

    ASSERT_FALSE(response.isEmpty());
    EXPECT_TRUE(response.startsWith("HTTP/1.1 200"));

    const int bodyStart = response.indexOf("\r\n\r\n");
    ASSERT_GE(bodyStart, 0);
    const QByteArray body = response.mid(bodyStart + 4);
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    ASSERT_TRUE(doc.isObject());
    EXPECT_TRUE(doc.object().value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(doc.object().value(QStringLiteral("version")).toString().isEmpty());

    server.stop();
}

TEST_F(ControlApiServerTest, UnknownEndpointReturns404) {
    ControlApiServer server(0, nullptr);
    ASSERT_TRUE(server.start());
    const quint16 port = server.boundPort();

    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, port);
    ASSERT_TRUE(socket.waitForConnected(2000));
    socket.write("GET /nope HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    ASSERT_TRUE(socket.waitForBytesWritten(2000));

    QByteArray response;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 3000) {
        if (socket.waitForReadyRead(200)) {
            response += socket.readAll();
        }
        if (socket.state() == QAbstractSocket::UnconnectedState && !response.isEmpty()) {
            break;
        }
    }

    ASSERT_FALSE(response.isEmpty());
    EXPECT_TRUE(response.startsWith("HTTP/1.1 404"));

    server.stop();
}

} // namespace
