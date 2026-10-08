#include "mixer/stemtwin.h"

#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QtDebug>
#include <cmath>

#include "library/trackcollectionmanager.h"
#include "mixer/basetrackplayer.h"
#include "mixer/gridcorrector.h"
#include "moc_stemtwin.cpp"
#include "track/cue.h"
#include "track/track.h"
#include "track/trackref.h"

namespace {
const QString kConfigGroup = QStringLiteral("[DJApp]");
const QString kConnectionName = QStringLiteral("djapp_brain_stemtwin");

// New cue objects with the same content, so copying never aliases the same
// Cue between the original and the twin (each Track connects to its own
// cues' updated() signal and mutates its own list independently).
QList<CuePointer> cloneCues(const QList<CuePointer>& source) {
    QList<CuePointer> cloned;
    cloned.reserve(source.size());
    for (const CuePointer& pCue : source) {
        CuePointer pClone(new Cue(pCue->getType(),
                pCue->getHotCue(),
                pCue->getPosition(),
                pCue->getEndPosition(),
                pCue->getColor()));
        pClone->setLabel(pCue->getLabel());
        cloned.push_back(pClone);
    }
    return cloned;
}
} // namespace

StemTwinController::StemTwinController(UserSettingsPointer pConfig, QObject* pParent)
        : QObject(pParent),
          m_pConfig(pConfig) {
}

void StemTwinController::setTrackCollectionManager(TrackCollectionManager* pTrackCollectionManager) {
    m_pTrackCollectionManager = pTrackCollectionManager;
}

void StemTwinController::watchPlayer(BaseTrackPlayer* pPlayer) {
    const QString group = pPlayer->getGroup();
    connect(pPlayer,
            &BaseTrackPlayer::trackUnloaded,
            this,
            [this, group](TrackPointer pUnloadedTrack) {
                // During a replace-load, substituteForLoad() already
                // released the old mapping and inserted the new one before
                // the engine actually ejects the old track, so by the time
                // this fires `group` already points at the NEW twin: only
                // release here when the unloaded track is still the one
                // mapped (a plain eject onto an empty deck).
                const auto it = m_byGroup.constFind(group);
                if (it != m_byGroup.constEnd() && it.value().pTwin == pUnloadedTrack) {
                    releaseGroup(group);
                }
            });
}

// static
bool StemTwinController::isStemFileLocation(const QString& location) {
    return location.endsWith(QStringLiteral(".stem.mp4"), Qt::CaseInsensitive) ||
            location.endsWith(QStringLiteral(".stem.m4a"), Qt::CaseInsensitive);
}

// static
bool StemTwinController::exportIsCurrent(double originalSampleRate,
        double originalDurationSeconds,
        double twinSampleRate,
        double twinFrames) {
    if (originalSampleRate <= 0.0 || originalSampleRate != twinSampleRate) {
        return false;
    }
    if (originalDurationSeconds <= 0.0) {
        return false;
    }
    const double expectedFrames = originalDurationSeconds * twinSampleRate;
    return std::abs(expectedFrames - twinFrames) <= kFrameTolerance;
}

// static
void StemTwinController::copyOntoTwin(const Track& original, Track* pTwin) {
    // Constant or not, the same Beats object is valid on the twin: its
    // frames are 1:1 with the original's (re-export, docs/decisions/0028),
    // and Beats are immutable (edits make a new BeatsPointer, never mutate
    // in place), so sharing the pointer is as safe as GridCorrector sharing
    // one across tryTranslate() calls.
    pTwin->trySetBeats(original.getBeats());
    pTwin->setKeys(original.getKeys());
    pTwin->setReplayGain(original.getReplayGain());
    pTwin->setCuePoints(cloneCues(original.getCuePoints()));
}

void StemTwinController::releaseGroup(const QString& group) {
    const auto it = m_byGroup.find(group);
    if (it == m_byGroup.end()) {
        return;
    }
    const TwinState state = it.value();
    m_byGroup.erase(it);
    if (!state.pOriginal || !state.pTwin) {
        return;
    }
    // The only two-way part of the sync: hotcues/cues Dan set while the twin
    // played go back onto the track that keeps playlists, history and
    // library cue edits (Dan's decision #2, 2026-10-08).
    state.pOriginal->setCuePoints(cloneCues(state.pTwin->getCuePoints()));
    if (m_pTrackCollectionManager) {
        m_pTrackCollectionManager->saveTrack(state.pOriginal);
    }
    qInfo() << "StemTwin: copied cues back to" << state.pOriginal->getLocation();
}

TrackPointer StemTwinController::substituteForLoad(
        const TrackPointer& pOriginal, const QString& group) {
    // Whatever twin was on this deck before must hand its cues back to its
    // own original before we decide what the NEW load does.
    releaseGroup(group);

    if (!pOriginal ||
            !m_pConfig->getValue(ConfigKey(kConfigGroup, QStringLiteral("StemTwins")), true)) {
        return pOriginal;
    }
    if (isStemFileLocation(pOriginal->getLocation())) {
        // Dan loaded a stem file directly (or we are seeing our own twin
        // re-enter somehow): never look for ITS twin.
        return pOriginal;
    }
    const QString dbPath = m_pConfig->getValueString(ConfigKey(kConfigGroup, QStringLiteral("BrainDb")));
    const std::optional<StemTwinInfo> twinInfo = StemTwinStore::lookup(dbPath, pOriginal->getLocation());
    if (!twinInfo) {
        return pOriginal;
    }
    if (!exportIsCurrent(pOriginal->getSampleRate().toDouble(),
                pOriginal->getDuration(),
                twinInfo->sampleRate,
                twinInfo->frames)) {
        qInfo() << "StemTwin: export for" << pOriginal->getLocation()
                << "is stale (sample rate or length changed) - playing the original";
        return pOriginal;
    }
    if (!m_pTrackCollectionManager) {
        qWarning() << "StemTwin: no library yet, playing the original";
        return pOriginal;
    }
    bool alreadyInLibrary = false;
    const TrackPointer pTwin = m_pTrackCollectionManager->getOrAddTrack(
            TrackRef::fromFilePath(twinInfo->stemPath), &alreadyInLibrary);
    if (!pTwin) {
        qWarning() << "StemTwin: cannot open twin file" << twinInfo->stemPath
                   << "- playing the original";
        return pOriginal;
    }
    if (!alreadyInLibrary) {
        // Hidden once, like a soft delete: stays hidden across sessions, so
        // Dan never has to see it browsing the library.
        m_pTrackCollectionManager->hideTracks({pTwin->getId()});
    }
    copyOntoTwin(*pOriginal, pTwin.get());
    m_pTrackCollectionManager->saveTrack(pTwin);
    m_byGroup[group] = TwinState{pOriginal, pTwin};
    qInfo() << "StemTwin: playing" << twinInfo->stemPath << "for" << pOriginal->getLocation();
    return pTwin;
}

// static
std::optional<StemTwinInfo> StemTwinStore::lookup(const QString& dbPath, const QString& originalLocation) {
    if (dbPath.isEmpty() || originalLocation.isEmpty() || !QFileInfo::exists(dbPath)) {
        return std::nullopt;
    }
    std::optional<StemTwinInfo> result;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), kConnectionName);
        db.setDatabaseName(dbPath);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (!db.open()) {
            qWarning() << "StemTwin: cannot open" << dbPath << db.lastError().text();
        } else {
            // Path comparison happens in C++ (GridCorrector::normalizedLocation),
            // not SQL: SQLite's lower() only folds ASCII, and Dan's locations
            // have diacritics.
            QSqlQuery query(db);
            const QString wanted = GridCorrector::normalizedLocation(originalLocation);
            if (query.exec(QStringLiteral(
                        "SELECT source_location, stem_path, sample_rate, frames "
                        "FROM stem_exports"))) {
                while (query.next()) {
                    if (GridCorrector::normalizedLocation(query.value(0).toString()) != wanted) {
                        continue;
                    }
                    StemTwinInfo info;
                    info.stemPath = query.value(1).toString();
                    info.sampleRate = query.value(2).toDouble();
                    info.frames = query.value(3).toDouble();
                    result = info;
                    break;
                }
            } else {
                qDebug() << "StemTwin: lookup failed" << query.lastError().text();
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(kConnectionName);
    return result;
}
