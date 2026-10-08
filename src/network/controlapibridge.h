#pragma once

#include <QObject>
#include <QString>

class PlayerManager;

// The only thing in the DJ App control API (docs/decisions/0031) that
// touches PlayerManager directly. Lives on the main/UI thread (constructed
// by CoreServices alongside PlayerManager); ControlApiServer, which runs on
// its own worker thread, only ever reaches it through
// QMetaObject::invokeMethod so these calls execute on the main thread like
// any other PlayerManager access (track loading, TrackPointer refcounting).
//
// Everything else the control API needs (reading/writing a named control,
// [AutomixTransition]/[DJAppAutomix] state) goes through
// ControlObject::get()/set(), which are already thread-safe statics used by
// controller-mapping threads -- no bridge needed for those.
class ControlApiBridge : public QObject {
    Q_OBJECT
  public:
    explicit ControlApiBridge(PlayerManager* pPlayerManager, QObject* pParent = nullptr);

  public slots:
    // Empty string if `group` has no player or no track loaded.
    QString getDeckTrackLocation(const QString& group);

    // Fire-and-forget, exactly like DlgDJAppSuggestions/DJAppAutopilot's own
    // loadTrackToPlayer: calls PlayerManager's existing load path, never a
    // bypass. Does not wait for the load (which can involve analysis) to
    // finish.
    void loadLocation(const QString& group, const QString& location, bool play);

  private:
    PlayerManager* m_pPlayerManager;
};
