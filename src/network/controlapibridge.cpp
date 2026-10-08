#include "network/controlapibridge.h"

#include "mixer/basetrackplayer.h"
#include "mixer/playermanager.h"
#include "moc_controlapibridge.cpp"
#include "track/track.h"

ControlApiBridge::ControlApiBridge(PlayerManager* pPlayerManager, QObject* pParent)
        : QObject(pParent),
          m_pPlayerManager(pPlayerManager) {
}

QString ControlApiBridge::getDeckTrackLocation(const QString& group) {
    if (!m_pPlayerManager) {
        return QString();
    }
    BaseTrackPlayer* pPlayer = m_pPlayerManager->getPlayer(group);
    if (!pPlayer) {
        return QString();
    }
    TrackPointer pTrack = pPlayer->getLoadedTrack();
    return pTrack ? pTrack->getLocation() : QString();
}

void ControlApiBridge::loadLocation(const QString& group, const QString& location, bool play) {
    if (!m_pPlayerManager) {
        return;
    }
    m_pPlayerManager->slotLoadLocationToPlayer(location, group, play);
}
