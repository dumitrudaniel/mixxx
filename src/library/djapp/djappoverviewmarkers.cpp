#include "library/djapp/djappoverviewmarkers.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <algorithm>
#include <cstring>

#include "library/djapp/braindbreader.h"
#include "library/djapp/djappautopilotlogic.h"

namespace djapp::overview {

QVector<VocalSegment> parseVocalSegmentsSec(const QByteArray& blob) {
    QVector<VocalSegment> result;
    const int floatCount = static_cast<int>(blob.size()) / static_cast<int>(sizeof(float));
    const int pairCount = floatCount / 2; // drop a trailing unpaired float
    if (pairCount <= 0) {
        return result;
    }
    result.reserve(pairCount);
    const char* data = blob.constData();
    for (int i = 0; i < pairCount; ++i) {
        float startF = 0.0f;
        float endF = 0.0f;
        std::memcpy(&startF, data + (2 * i) * sizeof(float), sizeof(float));
        std::memcpy(&endF, data + (2 * i + 1) * sizeof(float), sizeof(float));
        result.append(VocalSegment{static_cast<double>(startF), static_cast<double>(endF)});
    }
    return result;
}

QVector<double> dedupedMixOutValues(const QMap<int, double>& byLength) {
    QVector<double> values;
    values.reserve(byLength.size());
    for (auto it = byLength.constBegin(); it != byLength.constEnd(); ++it) {
        values.append(it.value());
    }
    std::sort(values.begin(), values.end());

    QVector<double> deduped;
    deduped.reserve(values.size());
    for (double v : values) {
        if (deduped.isEmpty() || std::abs(v - deduped.last()) > 0.05) {
            deduped.append(v);
        }
    }
    return deduped;
}

BrainMarkers fetchOverviewMarkers(
        const QString& dbPath, const QString& location, int busyTimeoutMs) {
    BrainMarkers out;
    if (dbPath.isEmpty() || location.isEmpty()) {
        return out;
    }

    QString targetKey = BrainDbReader::locationKey(location);
    QString error;
    BrainDbReader::withReadOnlyConnection(
            dbPath,
            busyTimeoutMs,
            [&](const QSqlDatabase& db) {
                // The deck may be playing a stem twin (.stem.mp4), not the
                // original -- mix_points/vocal_maps/sections are keyed by
                // the original's location (ADR 0028). Resolve it first,
                // exactly like DJAppAutopilot::readMixOutSec.
                QSqlQuery resolve(db);
                if (resolve.exec(QStringLiteral(
                            "SELECT source_location, stem_path FROM stem_exports"))) {
                    while (resolve.next()) {
                        if (BrainDbReader::locationKey(resolve.value(1).toString()) !=
                                targetKey) {
                            continue;
                        }
                        const QString original = resolve.value(0).toString();
                        if (!original.isEmpty()) {
                            targetKey = BrainDbReader::locationKey(original);
                        }
                        break;
                    }
                }

                QSqlQuery mixQuery(db);
                if (mixQuery.exec(QStringLiteral(
                            "SELECT location, mix_in_sec, mix_out_sec FROM mix_points"))) {
                    while (mixQuery.next()) {
                        if (BrainDbReader::locationKey(mixQuery.value(0).toString()) !=
                                targetKey) {
                            continue;
                        }
                        out.valid = true;
                        out.mixInSec = mixQuery.value(1).toDouble();
                        const QMap<int, double> byLength = djapp::autopilot::parseMixOutMap(
                                mixQuery.value(2).toString());
                        out.mixOutSec = dedupedMixOutValues(byLength);
                        break;
                    }
                }

                QSqlQuery vocalQuery(db);
                if (vocalQuery.exec(QStringLiteral(
                            "SELECT location, has_vocals, vocal_segments_sec "
                            "FROM vocal_maps"))) {
                    while (vocalQuery.next()) {
                        if (BrainDbReader::locationKey(vocalQuery.value(0).toString()) !=
                                targetKey) {
                            continue;
                        }
                        if (vocalQuery.value(1).toInt() != 0) {
                            out.vocalSegments = parseVocalSegmentsSec(
                                    vocalQuery.value(2).toByteArray());
                        }
                        break;
                    }
                }

                // sections.track_id -> tracks.id -> tracks.path (brain.db's
                // "location" equivalent for this table; see
                // brain/store/schema.sql).
                QSqlQuery sectionQuery(db);
                if (sectionQuery.exec(QStringLiteral(
                            "SELECT t.path, s.start_sec, s.end_sec, s.section_type "
                            "FROM sections s JOIN tracks t ON t.id = s.track_id"))) {
                    while (sectionQuery.next()) {
                        if (BrainDbReader::locationKey(sectionQuery.value(0).toString()) !=
                                targetKey) {
                            continue;
                        }
                        SectionMark mark;
                        mark.startSec = sectionQuery.value(1).toDouble();
                        mark.endSec = sectionQuery.value(2).toDouble();
                        mark.sectionType = sectionQuery.value(3).toString();
                        out.sections.append(mark);
                    }
                    std::sort(out.sections.begin(),
                            out.sections.end(),
                            [](const SectionMark& a, const SectionMark& b) {
                                return a.startSec < b.startSec;
                            });
                }
            },
            &error);
    return out;
}

} // namespace djapp::overview
