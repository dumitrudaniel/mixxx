#include "mixer/automixvocalguard.h"

#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QtDebug>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "mixer/automixtransitionmath.h"
#include "mixer/gridcorrector.h"
#include "track/beats.h"

namespace {
const QString kConnectionName = QStringLiteral("djapp_brain_vocalguard");
constexpr double kPi = 3.14159265358979323846;

// Normalized fade position in [0, 1]; a zero-length fade is a step at `start`
// (vocalguard.py _fade).
double fadePosition(double beat, double start, double length) {
    if (length <= 0.0) {
        return beat >= start ? 1.0 : 0.0;
    }
    return std::clamp((beat - start) / length, 0.0, 1.0);
}

std::optional<AutomixVocalMap> lookupIn(const QSqlDatabase& db, const QString& trackLocation) {
    const QString wanted = GridCorrector::normalizedLocation(trackLocation);
    QSqlQuery query(db);
    // Paths are matched here, not in SQL: SQLite's lower() only folds ASCII
    // and Dan's file names have diacritics.
    if (!query.exec(QStringLiteral("SELECT location FROM vocal_maps"))) {
        // Older brain.db without vocal maps.
        qDebug() << "Automix vocal guard: no vocal_maps in brain.db" << query.lastError().text();
        return std::nullopt;
    }
    QStringList mapLocations;
    QString matched;
    while (query.next()) {
        const QString location = query.value(0).toString();
        if (matched.isEmpty() && GridCorrector::normalizedLocation(location) == wanted) {
            matched = location;
        }
        mapLocations.append(location);
    }
    bool viaStemExport = false;
    if (matched.isEmpty()) {
        // A .stem.mp4 exported by brain shares the audio timeline of its
        // original (aligned sample-accurately), so the original's map applies.
        QString source;
        if (query.exec(QStringLiteral("SELECT source_location, stem_path FROM stem_exports"))) {
            while (query.next()) {
                if (GridCorrector::normalizedLocation(query.value(1).toString()) == wanted) {
                    source = query.value(0).toString();
                    break;
                }
            }
        }
        if (!source.isEmpty()) {
            const QString wantedSource = GridCorrector::normalizedLocation(source);
            for (const QString& location : std::as_const(mapLocations)) {
                if (GridCorrector::normalizedLocation(location) == wantedSource) {
                    matched = location;
                    viaStemExport = true;
                    break;
                }
            }
        }
    }
    if (matched.isEmpty()) {
        return std::nullopt;
    }
    query.prepare(QStringLiteral(
            "SELECT has_vocals, vocal_segments_sec FROM vocal_maps WHERE location = ?"));
    query.addBindValue(matched);
    if (!query.exec() || !query.next()) {
        qWarning() << "Automix vocal guard: cannot read the vocal map of" << matched
                   << query.lastError().text();
        return std::nullopt;
    }
    AutomixVocalMap map;
    map.location = matched;
    map.viaStemExport = viaStemExport;
    map.hasVocals = query.value(0).toInt() != 0;
    map.segmentsSec = AutomixVocalGuardPlanner::parseFloat32Pairs(query.value(1).toByteArray());
    return map;
}

} // namespace

double AutomixVocalGuardPlan::outgoingGain(double beat) const {
    const double t = fadePosition(beat, outgoingFadeStart, fadeBeats);
    if (t <= 0.0) {
        return 1.0;
    }
    if (t >= 1.0) {
        return 0.0;
    }
    return std::cos(t * kPi / 2.0);
}

double AutomixVocalGuardPlan::incomingGain(double beat) const {
    const double t = fadePosition(beat, incomingFadeStart, fadeBeats);
    if (t <= 0.0) {
        return 0.0;
    }
    if (t >= 1.0) {
        return 1.0;
    }
    return std::sin(t * kPi / 2.0);
}

// static
std::vector<AutomixVocalBlock> AutomixVocalGuardPlanner::mergeBlocks(
        std::vector<AutomixVocalBlock> blocks, double minGapBeats) {
    std::stable_sort(blocks.begin(),
            blocks.end(),
            [](const AutomixVocalBlock& a, const AutomixVocalBlock& b) {
                return a.start < b.start;
            });
    std::vector<AutomixVocalBlock> merged;
    merged.reserve(blocks.size());
    for (const AutomixVocalBlock& block : blocks) {
        if (!merged.empty() && block.start - merged.back().end < minGapBeats) {
            merged.back().end = std::max(merged.back().end, block.end);
        } else {
            merged.push_back(block);
        }
    }
    return merged;
}

// static
AutomixVocalGuardPlan AutomixVocalGuardPlanner::plan(
        const std::vector<AutomixVocalBlock>& outgoing,
        const std::vector<AutomixVocalBlock>& incoming,
        double maxWaitBeats,
        double fadeBeats,
        double latestHandoverBeats) {
    AutomixVocalGuardPlan plan;
    plan.fadeBeats = std::max(0.0, fadeBeats);
    bool singing = false;
    double blockEnd = 0.0;
    for (const AutomixVocalBlock& block : mergeBlocks(outgoing)) {
        if (block.start <= 0.0 && 0.0 < block.end) {
            singing = true;
            blockEnd = std::max(blockEnd, block.end);
        }
    }
    if (singing) {
        const double wait = std::max(0.0, std::min(maxWaitBeats, latestHandoverBeats));
        plan.outgoingFadeStart = std::min(blockEnd, wait);
        plan.cutMidPhrase = blockEnd > wait;
    }
    // Phrase finished: the new voice comes in after the old one is gone (no
    // overlap). Phrase cut: crossfade over the same beats, otherwise there is
    // a hole with no voice at all.
    plan.incomingFadeStart = plan.cutMidPhrase
            ? plan.outgoingFadeStart
            : plan.outgoingFadeStart + plan.fadeBeats;
    for (const AutomixVocalBlock& block : mergeBlocks(incoming)) {
        if (block.start < plan.incomingFadeStart && plan.incomingFadeStart < block.end) {
            plan.incomingMidPhrase = true;
        }
    }
    return plan;
}

// static
double AutomixVocalGuardPlanner::latestHandoverBeats(double lengthBeats) {
    return std::max(0.0, lengthBeats / 2.0);
}

// static
std::vector<AutomixVocalBlock> AutomixVocalGuardPlanner::parseFloat32Pairs(
        const QByteArray& blob) {
    const int count = static_cast<int>(blob.size() / 4);
    std::vector<AutomixVocalBlock> pairs;
    pairs.reserve(count / 2);
    const auto* pData = reinterpret_cast<const uchar*>(blob.constData());
    const auto valueAt = [pData](int index) {
        const quint32 bits = qFromLittleEndian<quint32>(pData + 4 * index);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return static_cast<double>(value);
    };
    for (int i = 0; i + 1 < count; i += 2) {
        const double start = valueAt(i);
        const double end = valueAt(i + 1);
        if (std::isfinite(start) && std::isfinite(end)) {
            pairs.push_back({start, end});
        }
    }
    return pairs;
}

// static
std::vector<AutomixVocalBlock> AutomixVocalGuardPlanner::secondsToGridBeats(
        const mixxx::Beats& beats, const std::vector<AutomixVocalBlock>& segmentsSec) {
    const double sampleRate = beats.getSampleRate().toDouble();
    std::vector<AutomixVocalBlock> blocks;
    if (sampleRate <= 0.0) {
        return blocks;
    }
    blocks.reserve(segmentsSec.size());
    for (const AutomixVocalBlock& segment : segmentsSec) {
        const std::optional<double> start = AutomixTransitionMath::gridBeatAt(
                beats, mixxx::audio::FramePos(segment.start * sampleRate));
        const std::optional<double> end = AutomixTransitionMath::gridBeatAt(
                beats, mixxx::audio::FramePos(segment.end * sampleRate));
        if (start && end && *end > *start) {
            blocks.push_back({*start, *end});
        }
    }
    return blocks;
}

// static
std::vector<AutomixVocalBlock> AutomixVocalGuardPlanner::toTransitionBeats(
        const std::vector<AutomixVocalBlock>& gridBlocks,
        double referenceGridBeat,
        double referenceTransitionBeat,
        double transitionBeatsPerGridBeat) {
    std::vector<AutomixVocalBlock> blocks;
    blocks.reserve(gridBlocks.size());
    for (const AutomixVocalBlock& block : gridBlocks) {
        blocks.push_back({
                referenceTransitionBeat +
                        (block.start - referenceGridBeat) * transitionBeatsPerGridBeat,
                referenceTransitionBeat +
                        (block.end - referenceGridBeat) * transitionBeatsPerGridBeat,
        });
    }
    return blocks;
}

// static
std::optional<AutomixVocalMap> AutomixVocalMapStore::lookup(
        const QString& dbPath, const QString& trackLocation) {
    if (dbPath.isEmpty() || trackLocation.isEmpty() || !QFileInfo::exists(dbPath)) {
        return std::nullopt;
    }
    std::optional<AutomixVocalMap> result;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), kConnectionName);
        db.setDatabaseName(dbPath);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (!db.open()) {
            qWarning() << "Automix vocal guard: cannot open" << dbPath << db.lastError().text();
        } else {
            result = lookupIn(db, trackLocation);
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(kConnectionName);
    return result;
}
