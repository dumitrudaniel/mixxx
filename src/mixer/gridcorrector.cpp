#include "mixer/gridcorrector.h"

#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QtDebug>
#include <cmath>
#include <limits>

#include "mixer/basetrackplayer.h"
#include "moc_gridcorrector.cpp"
#include "track/track.h"

namespace {
const QString kConfigGroup = QStringLiteral("[DJApp]");
const QString kConnectionName = QStringLiteral("djapp_brain_gridcorrector");
const QString kKindTempo = QStringLiteral("tempo");
const QString kKindTempoPhase = QStringLiteral("tempo+phase");

// The correction only applies to the exact grid it was computed on.
constexpr double kBpmTolerance = 1e-4;
constexpr double kAnchorToleranceFrames = 1.0;
constexpr double kSampleRateTolerance = 0.5;

// ADR 0025 columns first; a brain.db written before ADR 0025 has only the
// phase columns (and every row there is a phase row).
const QString kQuery = QStringLiteral(
        "SELECT location, original_bpm, original_first_beat_frame, sample_rate, "
        "correction_beats, reason, kind, corrected_bpm, corrected_first_beat_frame "
        "FROM grid_corrections WHERE apply = 1");
const QString kQueryBeforeTempo = QStringLiteral(
        "SELECT location, original_bpm, original_first_beat_frame, sample_rate, "
        "correction_beats, reason FROM grid_corrections WHERE apply = 1");

bool isTempoKind(const QString& kind) {
    return kind == kKindTempo || kind == kKindTempoPhase;
}
} // namespace

bool GridCorrector::Correction::correctsTempo() const {
    return isTempoKind(kind) && std::isfinite(correctedBpm) && correctedBpm > 0.0 &&
            std::isfinite(correctedAnchorFrame);
}

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
bool GridCorrector::matchesAnalyzedGrid(double currentBpm,
        double currentAnchorFrame,
        double currentSampleRate,
        const Correction& correction) {
    if (correction.originalBpm <= 0.0 || correction.sampleRate <= 0.0) {
        return false;
    }
    return std::abs(currentBpm - correction.originalBpm) <= kBpmTolerance &&
            std::abs(currentAnchorFrame - correction.originalAnchorFrame) <=
            kAnchorToleranceFrames &&
            std::abs(currentSampleRate - correction.sampleRate) <= kSampleRateTolerance;
}

// static
std::optional<double> GridCorrector::offsetFrames(double currentBpm,
        double currentAnchorFrame,
        double currentSampleRate,
        const Correction& correction) {
    if (correction.correctionBeats == 0.0 ||
            !matchesAnalyzedGrid(
                    currentBpm, currentAnchorFrame, currentSampleRate, correction)) {
        return std::nullopt;
    }
    return correction.correctionBeats * 60.0 / correction.originalBpm * correction.sampleRate;
}

// static
mixxx::BeatsPointer GridCorrector::correctedBeats(
        const mixxx::BeatsPointer& pCurrent, const Correction& correction) {
    // Constant grids only (no tempo markers): that is what brain analyzed.
    if (!pCurrent || pCurrent->cfirstmarker() != pCurrent->clastmarker()) {
        return nullptr;
    }
    const double bpm = pCurrent->getLastMarkerBpm().value();
    const double anchor = pCurrent->getLastMarkerPosition().value();
    const double sampleRate = pCurrent->getSampleRate().toDouble();
    if (isTempoKind(correction.kind)) {
        // The full target grid: tempo + phase + parity, decided by brain.
        if (!correction.correctsTempo() ||
                !matchesAnalyzedGrid(bpm, anchor, sampleRate, correction)) {
            return nullptr;
        }
        return mixxx::Beats::fromConstTempo(pCurrent->getSampleRate(),
                mixxx::audio::FramePos(correction.correctedAnchorFrame),
                mixxx::Bpm(correction.correctedBpm),
                pCurrent->getSubVersion());
    }
    const std::optional<double> offset = offsetFrames(bpm, anchor, sampleRate, correction);
    if (!offset) {
        return nullptr;
    }
    const std::optional<mixxx::BeatsPointer> translated = pCurrent->tryTranslate(*offset);
    return translated ? *translated : nullptr;
}

// static
bool GridCorrector::applyTo(const TrackPointer& pTrack, const Correction& correction) {
    if (!pTrack) {
        return false;
    }
    const QString location = pTrack->getLocation();
    if (pTrack->isBpmLocked()) {
        qInfo() << "GridCorrector: BPM of" << location << "is locked, not correcting";
        return false;
    }
    const mixxx::BeatsPointer pBeats = pTrack->getBeats();
    const mixxx::BeatsPointer pCorrected = correctedBeats(pBeats, correction);
    if (!pCorrected) {
        qInfo() << "GridCorrector: grid of" << location
                << "differs from the analyzed one (already corrected or edited), left as is";
        return false;
    }
    if (!pTrack->trySetBeats(pCorrected)) {
        return false;
    }
    if (correction.correctsTempo()) {
        qInfo() << "GridCorrector: corrected grid of" << location << "from"
                << correction.originalBpm << "BPM to" << correction.correctedBpm
                << "BPM, first beat at frame" << correction.correctedAnchorFrame << "("
                << correction.reason << ")";
    } else {
        qInfo() << "GridCorrector: corrected grid of" << location << "by"
                << correction.correctionBeats << "beats (" << correction.reason << ")";
    }
    return true;
}

// static
QString GridCorrector::normalizedLocation(const QString& location) {
    QString normalized = location;
    normalized.replace(QChar('\\'), QChar('/'));
    return normalized.toLower();
}

// static
std::optional<GridCorrector::Correction> GridCorrector::lookup(
        const QString& dbPath, const QString& location) {
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
            const bool withTempo = query.exec(kQuery);
            if (!withTempo && !query.exec(kQueryBeforeTempo)) {
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
                    if (withTempo) {
                        correction.kind = query.value(6).toString();
                        // NULL target (should not happen for a tempo row) makes
                        // the row unusable instead of a wrong grid.
                        correction.correctedBpm =
                                query.value(7).isNull() ? 0.0 : query.value(7).toDouble();
                        correction.correctedAnchorFrame = query.value(8).isNull()
                                ? std::numeric_limits<double>::quiet_NaN()
                                : query.value(8).toDouble();
                    }
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
    // Constant grids only: skip the brain.db lookup for anything else.
    if (!pBeats || pBeats->cfirstmarker() != pBeats->clastmarker()) {
        return;
    }
    const std::optional<Correction> correction = lookup(
            m_pConfig->getValueString(ConfigKey(kConfigGroup, QStringLiteral("BrainDb"))),
            pTrack->getLocation());
    if (!correction) {
        return;
    }
    applyTo(pTrack, *correction);
}
