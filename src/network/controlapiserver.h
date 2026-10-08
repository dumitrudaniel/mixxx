#pragma once

#include <QString>
#include <atomic>
#include <memory>
#include <thread>

#include "preferences/usersettings.h"

class ControlApiBridge;
class QTcpSocket;

// Minimal local control/testing HTTP server for DJ App (docs/decisions/0031):
// lets any Claude Code session read/set Mixxx's ControlObjects and load
// tracks over a plain loopback-only HTTP API, instead of a human relaying
// screenshots. See CoreServices::initialize() for where this is created and
// torn down, and src/test/controlapi_test.cpp for the request-parsing unit
// tests and one real end-to-end round trip.
//
// Binds 127.0.0.1 only. Runs its own worker thread (synchronous/blocking
// Qt sockets, no event loop needed) so it can never block the UI/audio
// thread; the one place it reaches into the running app beyond
// ControlObject's own thread-safe statics (PlayerManager, for track
// location/loading) goes through ControlApiBridge via
// QMetaObject::invokeMethod, which runs on the main thread.
//
// Config ([DJApp] in mixxx.cfg):
//   ControlApiEnabled = 0   -- turns the whole thing off (default: on).
//   ControlApiPort = <int>  -- default 17000.
class ControlApiServer {
  public:
    // pBridge may be null (the server still answers /health and
    // /control, just not /load and the track-location fields of /state).
    // Does not take ownership of pBridge.
    ControlApiServer(quint16 port, ControlApiBridge* pBridge);
    ~ControlApiServer();

    // Starts the worker thread. Logs one line stating whether it is
    // listening and on which port (or why it failed to bind). Returns
    // true if the socket is listening.
    bool start();

    // Stops the worker thread and joins it. Safe to call even if start()
    // was never called or failed.
    void stop();

    quint16 requestedPort() const {
        return m_port;
    }

    // The actual bound port once listening (same as requestedPort() unless
    // 0 was requested for an ephemeral port, used by the round-trip test).
    quint16 boundPort() const {
        return m_boundPort.load();
    }

    bool isListening() const {
        return m_listening.load();
    }

  private:
    void run();
    // Reads one full HTTP request off `pSocket` (headers + body, per
    // Content-Length) with a short per-read timeout, then writes the
    // response and closes the connection. Never throws: any failure
    // becomes a 4xx/5xx JSON response or a closed connection.
    void handleConnection(QTcpSocket* pSocket);

    quint16 m_port;
    ControlApiBridge* m_pBridge;
    std::atomic<quint16> m_boundPort{0};
    std::atomic<bool> m_listening{false};
    std::atomic<bool> m_stopRequested{false};
    std::unique_ptr<std::thread> m_pThread;
};
