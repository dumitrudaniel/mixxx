#include "mixer/gridcorrector.h"

#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QtDebug>
#include <cmath>

#include "mixer/basetrackplayer.h"
#include "moc_gridcorrector.cpp"
#include "track/track.h"

namespace {
const QString kConfigGroup = QStringLiteral("[DJApp]");
const QString kConnectionName = QStringLiteral("djapp_brain_gridcorrector");

// The correction only applies to the exact grid it was computed on.
constexpr double kBpmTolerance = 1e-4;
constexpr double kAnchorToleranceFrames = 1.0;
constexpr double kSampleRateTolerance = 0.5;
} // namespace

GridCorrector::GridCorrector(UserSettingsPointer pConfig, QObject* pParent)
        : QObject(pParent),
          m_pConfig(pConfig) {
}

void GridCorrector::watchPlayer(BaseTrackPlayer* pPlayer) {
    connect(pPlayer,
            &BaseTrackPlayer::newTrackLoaded,
            this,
            &GridCorrector::slotTrackLoaded);
}

// static
std::optional<double> GridCorrector::offsetFrames(double currentBpm,
        double currentAnchorFrame,
        double currentSampleRate,
        const Correction& correction) {
    if (correction.correctionBeats == 0.0 || correction.originalBpm <= 0.0 ||
            correction.sampleRate <= 0.0) {
        return std::nullopt;
    }
    if (std::abs(currentBpm - correction.originalBpm) > kBpmTolerance ||
            std::abs(currentAnchorFrame - correction.originalAnchorFrame) >
                    kAnchorToleranceFrames ||
            std::abs(currentSampleRate - correction.sampleRate) > kSampleRateTolerance) {
        return std::nullopt;
    }
    return correction.correctionBeats * 60.0 / correction.originalBpm * correction.sampleRate;
}

// static
QString GridCorrector::normalizedLocation(const QString& location) {
    QString normalized = location;
    normalized.replace(QChar('\\'), QChar('/'));
    return normalized.toLower();
}

std::optional<GridCorrector::Correction> GridCorrector::lookup(const QString& location) const {
    const QString dbPath = m_pConfig->getValueString(
            ConfigKey(kConfigGroup, QStringLiteral("BrainDb")));
    if (dbPath.isEmpty() || !QFileInfo::exists(dbPath)) {
        return std::nullopt;
    }
    std::optional<Correction> result;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), kConnectionName);
        db.setDatabaseName(dbPath);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (!db.open()) {
            qWarning() << "GridCorrector: cannot open" << dbPath << db.lastError().text();
        } else {
            // The path match happens here, not in SQL: SQLite's lower() only
            // folds ASCII, and Dan's file names have diacritics. Only rows to
            // apply are read, so the scan stays small.
            QSqlQuery query(db);
            const QString wanted = normalizedLocation(location);
            if (!query.exec(QStringLiteral(
                        "SELECT location, original_bpm, original_first_beat_frame, "
                        "sample_rate, correction_beats, reason FROM grid_corrections "
                        "WHERE apply = 1"))) {
                // Older brain.db without the table: nothing to apply.
                qDebug() << "GridCorrector: lookup failed" << query.lastError().text();
            } else {
                while (query.next()) {
                    if (normalizedLocation(query.value(0).toString()) != wanted) {
                        continue;
                    }
                    Correction correction;
                    correction.originalBpm = query.value(1).toDouble();
                    correction.originalAnchorFrame = query.value(2).toDouble();
                    correction.sampleRate = query.value(3).toDouble();
                    correction.correctionBeats = query.value(4).toDouble();
                    correction.reason = query.value(5).toString();
                    result = correction;
                    break;
                }
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(kConnectionName);
    return result;
}

void GridCorrector::slotTrackLoaded(TrackPointer pTrack) {
    if (!pTrack ||
            !m_pConfig->getValue(ConfigKey(kConfigGroup, QStringLiteral("GridCorrection")),
                    true)) {
        return;
    }
    const mixxx::BeatsPointer pBeats = pTrack->getBeats();
    if (!pBeats) {
        return;
    }
    // Constant grids only (no tempo markers): that is what brain analyzed.
    if (pBeats->cfirstmarker() != pBeats->clastmarker()) {
        return;
    }
    const QString location = pTrack->getLocation();
    const std::optional<Correction> correction = lookup(location);
    if (!correction) {
        return;
    }
    const std::optional<double> offset = offsetFrames(pBeats->getLastMarkerBpm().value(),
            pBeats->getLastMarkerPosition().value(),
            pBeats->getSampleRate().toDouble(),
            *correction);
    if (!offset) {
        qInfo() << "GridCorrector: grid of" << location
                << "differs from the analyzed one (already corrected or edited), left as is";
        return;
    }
    if (pTrack->isBpmLocked()) {
        qInfo() << "GridCorrector: BPM of" << location << "is locked, not correcting";
        return;
    }
    const std::optional<mixxx::BeatsPointer> translated = pBeats->tryTranslate(*offset);
    if (!translated || !*translated) {
        return;
    }
    if (pTrack->trySetBeats(*translated)) {
        qInfo() << "GridCorrector: corrected grid of" << location << "by"
                << correction->correctionBeats << "beats (" << correction->reason << ")";
    }
}
