#include "library/djapp/djappanalysis.h"

#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <QSqlError>
#include <QSqlQuery>
#include <algorithm>
#include <cmath>

namespace {

const QString kDash = QStringLiteral("—");
const QString kSep = QStringLiteral(" · ");

QString number(double value, int decimals) {
    static const QLocale kRomanian(QLocale::Romanian, QLocale::Romania);
    return kRomanian.toString(value, 'f', decimals);
}

QString minutesSeconds(double seconds) {
    const int total = static_cast<int>(std::lround(std::max(0.0, seconds)));
    return QStringLiteral("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QLatin1Char('0'));
}

// Faza 2 / Etapa 2 step names, as brain writes them.
QString stepText(const QString& step) {
    static const QHash<QString, QString> kSteps = {
            {QStringLiteral("features"), QStringLiteral("caracteristici")},
            {QStringLiteral("beats"), QStringLiteral("beat-uri")},
            {QStringLiteral("sections"), QStringLiteral("secțiuni")},
            {QStringLiteral("stems"), QStringLiteral("stems")},
            {QStringLiteral("export"), QStringLiteral("export .stem")},
            {QStringLiteral("embeddings"), QStringLiteral("embeddings")},
            {QStringLiteral("refresh"), QStringLiteral("recalcul")},
            {QStringLiteral("gridcheck"), QStringLiteral("grilă")},
            {QStringLiteral("vocalmap"), QStringLiteral("harta vocii")},
            {QStringLiteral("mixpoints"), QStringLiteral("puncte de mix")},
            {QStringLiteral("index"), QStringLiteral("index")},
    };
    return kSteps.value(step, step);
}

QString percent(double fraction) {
    return QStringLiteral("%1 %").arg(std::lround(std::clamp(fraction, 0.0, 1.0) * 100.0));
}

// Etapa 2 (analysis_progress): the service's own view of the track.
bool stageFromProgress(const BrainTrackInfo& info, DJAppAnalysisRow* pRow) {
    const QString& state = info.progressState;
    if (state == QLatin1String("complet")) {
        pRow->stage = DJAppBrainStage::Done;
        pRow->stageText = QStringLiteral("complet");
    } else if (state == QLatin1String("in_coada")) {
        pRow->stage = DJAppBrainStage::Queued;
        pRow->stageText = QStringLiteral("în coadă");
    } else if (state == QLatin1String("asteapta_mixxx")) {
        pRow->stage = DJAppBrainStage::Queued;
        pRow->stageText = QStringLiteral("așteaptă Mixxx");
        pRow->stageTooltip = QStringLiteral(
                "Fără grilă sau tonalitate Mixxx: analizează piesa în Mixxx "
                "(BPM, key, grilă) sau încarc-o pe un deck.");
    } else if (state == QLatin1String("lucru")) {
        pRow->stage = DJAppBrainStage::Working;
        QString text = QStringLiteral("în lucru");
        if (!info.progressStep.isEmpty()) {
            text += QStringLiteral(": ") + stepText(info.progressStep);
        }
        if (info.stepProgress) {
            text += QStringLiteral(" ") + percent(*info.stepProgress);
        } else if (info.progress) {
            text += QStringLiteral(" ") + percent(*info.progress);
        }
        pRow->stageText = text;
    } else if (state == QLatin1String("eroare")) {
        pRow->stage = DJAppBrainStage::Error;
        pRow->stageText = QStringLiteral("eroare");
        pRow->stageTooltip = info.progressError;
    } else if (state == QLatin1String("lipsa")) {
        pRow->stage = DJAppBrainStage::Error;
        pRow->stageText = QStringLiteral("fișier lipsă");
        pRow->stageTooltip = info.progressError;
    } else {
        return false; // unknown state: use the queue instead
    }
    return true;
}

// Faza 2 pipeline (tracks + analysis_queue), today's tables.
void stageFromQueue(const BrainTrackInfo& info, DJAppAnalysisRow* pRow) {
    const QString& status = info.analysisStatus;
    const QString& stage = info.queueStage;
    if (status == QLatin1String("failed")) {
        pRow->stage = DJAppBrainStage::Error;
        pRow->stageText = QStringLiteral("eroare");
    } else if (stage == QLatin1String("done") ||
            (stage.isEmpty() && status == QLatin1String("done"))) {
        pRow->stage = DJAppBrainStage::Done;
        pRow->stageText = QStringLiteral("analizată");
    } else if ((stage.isEmpty() || stage == QLatin1String("pending")) &&
            status != QLatin1String("in_progress")) {
        pRow->stage = DJAppBrainStage::Queued;
        pRow->stageText = QStringLiteral("în coadă");
    } else {
        pRow->stage = DJAppBrainStage::Working;
        pRow->stageText = stage.isEmpty() || stage == QLatin1String("pending")
                ? QStringLiteral("în lucru")
                : QStringLiteral("în lucru: ") + stepText(stage);
    }
    if (!info.analysisError.isEmpty()) {
        pRow->stageTooltip = QStringLiteral("Ultima eroare (încercarea %1): %2")
                                     .arg(info.attempts)
                                     .arg(info.analysisError);
    }
}

void fillGrid(const DJAppLibraryTrack& track, const BrainTrackInfo* pGrid, DJAppAnalysisRow* pRow) {
    if (!pGrid || !pGrid->hasGridRow) {
        pRow->gridText = kDash;
        pRow->gridTooltip = QStringLiteral("gridcheck nu a analizat grila acestui fișier");
        return;
    }
    const BrainTrackInfo& g = *pGrid;
    const bool tempoKind = g.gridKind == QLatin1String("tempo") ||
            g.gridKind == QLatin1String("tempo+phase");
    QString text;
    if (g.gridApply && tempoKind && g.gridCorrectedBpm) {
        // Applied by Mixxx once the library BPM is the target (ADR 0025).
        const bool applied = std::abs(track.bpm - *g.gridCorrectedBpm) < 0.01;
        text = QStringLiteral("%1tempo %2 → %3")
                       .arg(applied ? QStringLiteral("aplicată: ")
                                    : QStringLiteral("de aplicat: "),
                               number(g.gridOriginalBpm, 2),
                               number(*g.gridCorrectedBpm, 2));
        pRow->gridToApply = !applied;
    } else if (g.gridApply) {
        text = QStringLiteral("de aplicat: %1 (%2 timpi)")
                       .arg(g.gridReason,
                               (g.gridCorrectionBeats > 0 ? QStringLiteral("+") : QString()) +
                                       number(g.gridCorrectionBeats, 2));
        pRow->gridToApply = true;
    } else {
        text = g.gridReason.isEmpty() ? QStringLiteral("ok") : g.gridReason;
    }
    pRow->variableTempo = g.tempoIsVariable();
    if (pRow->variableTempo) {
        text += kSep + QStringLiteral("tempo variabil");
    } else if (!g.tempoFlag.trimmed().isEmpty()) {
        text += kSep + g.tempoFlag.trimmed();
    }
    pRow->gridText = text;

    QStringList tip;
    tip << QStringLiteral("kind: %1 · apply: %2 · motiv: %3")
                    .arg(g.gridKind.isEmpty() ? QStringLiteral("phase") : g.gridKind,
                            g.gridApply ? QStringLiteral("1") : QStringLiteral("0"),
                            g.gridReason);
    if (g.gridOriginalBpm > 0.0) {
        tip << QStringLiteral("grila analizată: %1 BPM").arg(number(g.gridOriginalBpm, 2));
    }
    if (!g.tempoFlag.isEmpty()) {
        tip << QStringLiteral("tempo_flag: %1").arg(g.tempoFlag);
    }
    if (g.driftBefore) {
        tip << QStringLiteral("alunecare: %1 timpi/100%2")
                        .arg(number(*g.driftBefore, 2),
                                g.driftAfter ? QStringLiteral(" → ") + number(*g.driftAfter, 2)
                                             : QString());
    }
    tip << QStringLiteral("încredere: %1").arg(number(g.gridConfidence, 2));
    if (pRow->variableTempo) {
        tip << QStringLiteral(
                "Tempo variabil (trupă live): nicio grilă constantă nu o urmărește; "
                "MIX și quantize sunt aproximative (ADR 0025).");
    }
    pRow->gridTooltip = tip.join(QLatin1Char('\n'));
}

QString indexStateText(const QString& state) {
    if (state == QLatin1String("complet")) {
        return QStringLiteral("complet");
    }
    if (state == QLatin1String("rapid")) {
        return QStringLiteral("rapid (estimare)");
    }
    if (state == QLatin1String("asteapta_mixxx")) {
        return QStringLiteral("așteaptă Mixxx");
    }
    if (state == QLatin1String("lipsa")) {
        return QStringLiteral("lipsă");
    }
    if (state == QLatin1String("eroare")) {
        return QStringLiteral("eroare");
    }
    return state;
}

} // namespace

// static
QList<DJAppLibraryTrack> DJAppAnalysis::readLibraryTracks(
        const QSqlDatabase& mixxxDb, QString* pError) {
    QList<DJAppLibraryTrack> tracks;
    QSqlQuery query(mixxxDb);
    query.setForwardOnly(true);
    if (!query.exec(QStringLiteral(
                "SELECT library.id, library.artist, library.title, library.bpm, "
                "library.key, track_locations.location "
                "FROM library INNER JOIN track_locations "
                "ON library.location = track_locations.id "
                "WHERE library.mixxx_deleted = 0"))) {
        if (pError) {
            *pError = query.lastError().text();
        }
        return tracks;
    }
    while (query.next()) {
        DJAppLibraryTrack track;
        track.id = query.value(0).toInt();
        track.artist = query.value(1).toString();
        track.title = query.value(2).toString();
        track.bpm = query.value(3).toDouble();
        track.key = query.value(4).toString();
        track.location = query.value(5).toString();
        tracks.append(track);
    }
    return tracks;
}

// static
DJAppAnalysisRow DJAppAnalysis::buildRow(
        const DJAppLibraryTrack& track, const BrainSnapshot& brain) {
    DJAppAnalysisRow row;
    row.track = track;
    if (!track.artist.isEmpty() && !track.title.isEmpty()) {
        row.trackText = track.artist + QStringLiteral(" – ") + track.title;
    } else if (!track.title.isEmpty()) {
        row.trackText = track.title;
    } else {
        row.trackText = QFileInfo(track.location).fileName();
    }

    if (track.bpm <= 0.0) {
        row.mixxxText = QStringLiteral("fără BPM");
    } else if (track.key.trimmed().isEmpty()) {
        row.mixxxText = QStringLiteral("fără tonalitate");
    } else {
        row.mixxxText = QStringLiteral("ok");
        row.mixxxReady = true;
    }

    const BrainTrackInfo* pOwn = brain.find(track.location);
    // A .stem.mp4 exported by brain: its original's analysis applies (same
    // audio timeline); its grid is its own (Mixxx analyzes it separately).
    const BrainTrackInfo* pSource = nullptr;
    if (pOwn && !pOwn->stemSourceLocation.isEmpty()) {
        row.isStemCopy = true;
        pSource = brain.find(pOwn->stemSourceLocation);
    }
    const auto pick = [pOwn, pSource](bool (*has)(const BrainTrackInfo&)) {
        if (pOwn && has(*pOwn)) {
            return pOwn;
        }
        return (pSource && has(*pSource)) ? pSource : static_cast<const BrainTrackInfo*>(nullptr);
    };

    // Stage
    const BrainTrackInfo* pPipeline = pick([](const BrainTrackInfo& i) {
        return i.inTracks || i.hasProgress;
    });
    if (!pPipeline) {
        row.stage = DJAppBrainStage::NotInBrain;
        row.stageText = kDash;
        row.stageTooltip = QStringLiteral("brain nu a importat încă piesa");
    } else if (!(pPipeline->hasProgress && stageFromProgress(*pPipeline, &row))) {
        stageFromQueue(*pPipeline, &row);
    }
    if (row.isStemCopy && pPipeline == pSource) {
        row.stageTooltip = (row.stageTooltip.isEmpty() ? QString() : row.stageTooltip + '\n') +
                QStringLiteral("Starea originalului: ") + pOwn->stemSourceLocation;
    }

    // Stems export
    if (row.isStemCopy) {
        row.stemText = QStringLiteral("dublură .stem");
    } else if (pOwn && !pOwn->stemExportPath.isEmpty()) {
        row.stemText = QStringLiteral("da");
    } else {
        row.stemText = kDash;
    }

    // Vocal map
    const BrainTrackInfo* pVocal = pick([](const BrainTrackInfo& i) {
        return i.hasVocalMap;
    });
    if (!pVocal) {
        row.vocalText = kDash;
    } else if (pVocal->hasVocals) {
        row.vocalText = pVocal->vocalShareDb
                ? QStringLiteral("voce (%1 dB)").arg(number(*pVocal->vocalShareDb, 1))
                : QStringLiteral("voce");
    } else {
        row.vocalText = QStringLiteral("instrumentală");
    }

    // Grid correction: own file only.
    fillGrid(track, pOwn, &row);

    // Mix points
    const BrainTrackInfo* pPoints = pick([](const BrainTrackInfo& i) {
        return i.hasMixPoints;
    });
    if (!pPoints) {
        row.mixPointsText = kDash;
    } else {
        row.mixPointsText = QStringLiteral("da");
        row.mixPointsTooltip = QStringLiteral("intrare (Standard 8) la %1 · muzica %2–%3")
                                       .arg(minutesSeconds(pPoints->mixInSec),
                                               minutesSeconds(pPoints->musicStartSec),
                                               minutesSeconds(pPoints->musicEndSec));
    }

    // Index (ADR 0026)
    if (!brain.hasIndexTables) {
        row.indexText = kDash;
    } else {
        const BrainTrackInfo* pIndex = pick([](const BrainTrackInfo& i) {
            return i.inIndex;
        });
        row.indexText = pIndex ? indexStateText(pIndex->indexState) : kDash;
    }
    return row;
}

// static
QList<DJAppAnalysisRow> DJAppAnalysis::buildRows(
        const QList<DJAppLibraryTrack>& tracks, const BrainSnapshot& brain) {
    QList<DJAppAnalysisRow> rows;
    rows.reserve(tracks.size());
    for (const DJAppLibraryTrack& track : tracks) {
        rows.append(buildRow(track, brain));
    }
    return rows;
}

// static
DJAppAnalysisSummary DJAppAnalysis::summarize(const QList<DJAppAnalysisRow>& rows) {
    DJAppAnalysisSummary summary;
    summary.total = static_cast<int>(rows.size());
    for (const DJAppAnalysisRow& row : rows) {
        switch (row.stage) {
        case DJAppBrainStage::Done:
            ++summary.analysed;
            break;
        case DJAppBrainStage::Queued:
        case DJAppBrainStage::Working:
            ++summary.pending;
            break;
        case DJAppBrainStage::Error:
            ++summary.errors;
            break;
        case DJAppBrainStage::NotInBrain:
            ++summary.notInBrain;
            break;
        }
        if (row.variableTempo) {
            ++summary.variableTempo;
        }
        if (row.gridToApply) {
            ++summary.gridToApply;
        }
    }
    return summary;
}

// static
QString DJAppAnalysis::summaryText(const DJAppAnalysisSummary& summary) {
    return QStringLiteral(
            "Analizate: %1 · în coadă: %2 · tempo variabil: %3 · erori: %4 · "
            "nu sunt în brain: %5 · total: %6")
            .arg(summary.analysed)
            .arg(summary.pending)
            .arg(summary.variableTempo)
            .arg(summary.errors)
            .arg(summary.notInBrain)
            .arg(summary.total);
}

// static
QString DJAppAnalysis::snapshotText(const BrainSnapshot& brain) {
    if (!brain.ok) {
        return QStringLiteral("brain.db: ") + brain.error;
    }
    QStringList parts;
    parts << QStringLiteral("brain.db citit la %1 (%2 ms)")
                     .arg(brain.readAt.toString(QStringLiteral("HH:mm:ss")))
                     .arg(brain.readMs);
    parts << QStringLiteral("jurnal %1").arg(brain.journalMode == QLatin1String("wal")
                    ? QStringLiteral("WAL")
                    : brain.journalMode);
    parts << QStringLiteral("%1 piese în brain").arg(brain.pipelineTracks);
    if (brain.service.present) {
        const bool online = brain.service.isOnline(QDateTime::currentDateTimeUtc());
        parts << (online ? QStringLiteral("brain ● pornit (%1)").arg(brain.service.level)
                         : QStringLiteral("brain ○ oprit"));
    }
    if (!brain.missingTables.isEmpty()) {
        parts << QStringLiteral("lipsesc: ") + brain.missingTables.join(QStringLiteral(", "));
    }
    return parts.join(kSep);
}
