#include "library/djapp/braindbreader.h"

#include <QAtomicInt>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimeZone>
#include <QVariant>
#include <QtDebug>

#include "mixer/gridcorrector.h"

namespace {

QAtomicInt s_connectionCounter;

// The existing brain.db contract the DJ App views read (brain/store/schema.sql).
const QStringList kContractTables = {
        QStringLiteral("tracks"),
        QStringLiteral("analysis_queue"),
        QStringLiteral("stem_exports"),
        QStringLiteral("vocal_maps"),
        QStringLiteral("grid_corrections"),
        QStringLiteral("mix_points"),
};
// ADR 0026 (index-builder, store/indexdb.py); optional.
const QString kTrackIndexTable = QStringLiteral("track_index");
// Etapa 2 (brain-service, store/analysisprogress.py); optional, draft.
const QString kProgressTable = QStringLiteral("analysis_progress");
const QString kStatusTable = QStringLiteral("brain_status");

QSet<QString> tablesOf(const QSqlDatabase& db, QString* pError) {
    QSet<QString> tables;
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type = 'table'"))) {
        *pError = query.lastError().text();
        return tables;
    }
    while (query.next()) {
        tables.insert(query.value(0).toString());
    }
    return tables;
}

// `table` only ever comes from the fixed lists above.
QSet<QString> columnsOf(const QSqlDatabase& db, const QString& table) {
    QSet<QString> columns;
    QSqlQuery query(db);
    if (query.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
        while (query.next()) {
            columns.insert(query.value(1).toString());
        }
    }
    return columns;
}

// The column if this brain.db has it, else NULL (older schema).
QString column(const QSet<QString>& columns, const QString& name) {
    return columns.contains(name) ? name : QStringLiteral("NULL");
}

std::optional<double> optionalDouble(const QVariant& value) {
    if (value.isNull()) {
        return std::nullopt;
    }
    bool ok = false;
    const double result = value.toDouble(&ok);
    return ok ? std::optional<double>(result) : std::nullopt;
}

BrainTrackInfo& entryFor(BrainSnapshot* pSnapshot, const QString& location) {
    const QString key = BrainDbReader::locationKey(location);
    auto it = pSnapshot->tracks.find(key);
    if (it == pSnapshot->tracks.end()) {
        it = pSnapshot->tracks.insert(key, BrainTrackInfo());
        it->location = location;
    }
    return *it;
}

// How far the Faza 2 pipeline got; used when brain has the same file twice
// (e.g. once with '\' and once with '/').
int pipelineRank(const QString& status, const QString& stage) {
    if (stage == QLatin1String("done") || status == QLatin1String("done")) {
        return 3;
    }
    if (!stage.isEmpty() && stage != QLatin1String("pending")) {
        return 2;
    }
    if (status == QLatin1String("in_progress")) {
        return 2;
    }
    return 1;
}

bool readPipeline(const QSqlDatabase& db,
        const QSet<QString>& tables,
        BrainSnapshot* pSnapshot,
        QString* pError) {
    if (!tables.contains(QStringLiteral("tracks"))) {
        return true;
    }
    const QSet<QString> trackColumns = columnsOf(db, QStringLiteral("tracks"));
    const bool withQueue = tables.contains(QStringLiteral("analysis_queue"));
    const QString sql = withQueue
            ? QStringLiteral(
                      "SELECT t.path, t.%1, t.%2, q.stage, q.attempts, q.last_error, "
                      "q.track_id IS NOT NULL "
                      "FROM tracks t LEFT JOIN analysis_queue q ON q.track_id = t.id")
                      .arg(column(trackColumns, QStringLiteral("analysis_status")),
                              column(trackColumns, QStringLiteral("analysis_error")))
            : QStringLiteral(
                      "SELECT path, %1, %2, NULL, NULL, NULL, 0 FROM tracks")
                      .arg(column(trackColumns, QStringLiteral("analysis_status")),
                              column(trackColumns, QStringLiteral("analysis_error")));
    QSqlQuery query(db);
    if (!query.exec(sql)) {
        *pError = query.lastError().text();
        return false;
    }
    while (query.next()) {
        const QString location = query.value(0).toString();
        if (location.isEmpty()) {
            continue;
        }
        ++pSnapshot->pipelineTracks;
        const QString status = query.value(1).toString();
        const QString stage = query.value(3).toString();
        BrainTrackInfo& info = entryFor(pSnapshot, location);
        if (info.inTracks &&
                pipelineRank(info.analysisStatus, info.queueStage) >=
                        pipelineRank(status, stage)) {
            continue;
        }
        info.inTracks = true;
        info.analysisStatus = status;
        info.inQueue = query.value(6).toBool();
        info.queueStage = stage;
        info.attempts = query.value(4).toInt();
        const QString trackError = query.value(2).toString().trimmed();
        info.analysisError = trackError.isEmpty() ? query.value(5).toString().trimmed() : trackError;
    }
    return true;
}

bool readStemExports(const QSqlDatabase& db, BrainSnapshot* pSnapshot, QString* pError) {
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("SELECT source_location, stem_path FROM stem_exports"))) {
        *pError = query.lastError().text();
        return false;
    }
    while (query.next()) {
        const QString source = query.value(0).toString();
        const QString stemPath = query.value(1).toString();
        if (source.isEmpty() || stemPath.isEmpty()) {
            continue;
        }
        entryFor(pSnapshot, source).stemExportPath = stemPath;
        entryFor(pSnapshot, stemPath).stemSourceLocation = source;
    }
    return true;
}

bool readVocalMaps(const QSqlDatabase& db, BrainSnapshot* pSnapshot, QString* pError) {
    const QSet<QString> columns = columnsOf(db, QStringLiteral("vocal_maps"));
    QSqlQuery query(db);
    // has_vocals / vocal_share_db come after the per-beat blobs; still cheap
    // at library sizes (a few KB per row).
    if (!query.exec(QStringLiteral("SELECT location, %1, %2 FROM vocal_maps")
                            .arg(column(columns, QStringLiteral("has_vocals")),
                                    column(columns, QStringLiteral("vocal_share_db"))))) {
        *pError = query.lastError().text();
        return false;
    }
    while (query.next()) {
        BrainTrackInfo& info = entryFor(pSnapshot, query.value(0).toString());
        info.hasVocalMap = true;
        // Before has_vocals existed every map had vocals.
        info.hasVocals = query.value(1).isNull() || query.value(1).toInt() != 0;
        info.vocalShareDb = optionalDouble(query.value(2));
    }
    return true;
}

bool readGridCorrections(const QSqlDatabase& db, BrainSnapshot* pSnapshot, QString* pError) {
    const QSet<QString> columns = columnsOf(db, QStringLiteral("grid_corrections"));
    QSqlQuery query(db);
    const QString sql = QStringLiteral(
            "SELECT location, apply, reason, correction_beats, %1, %2, %3, %4, %5, %6, %7 "
            "FROM grid_corrections")
                                .arg(column(columns, QStringLiteral("confidence")),
                                        column(columns, QStringLiteral("original_bpm")),
                                        column(columns, QStringLiteral("kind")),
                                        column(columns, QStringLiteral("corrected_bpm")),
                                        column(columns, QStringLiteral("tempo_flag")),
                                        column(columns, QStringLiteral("drift_before")),
                                        column(columns, QStringLiteral("drift_after")));
    if (!query.exec(sql)) {
        *pError = query.lastError().text();
        return false;
    }
    while (query.next()) {
        BrainTrackInfo& info = entryFor(pSnapshot, query.value(0).toString());
        info.hasGridRow = true;
        info.gridApply = query.value(1).toInt() != 0;
        info.gridReason = query.value(2).toString();
        info.gridCorrectionBeats = query.value(3).toDouble();
        info.gridConfidence = query.value(4).toDouble();
        info.gridOriginalBpm = query.value(5).toDouble();
        info.gridKind = query.value(6).toString();
        info.gridCorrectedBpm = optionalDouble(query.value(7));
        info.tempoFlag = query.value(8).toString();
        info.driftBefore = optionalDouble(query.value(9));
        info.driftAfter = optionalDouble(query.value(10));
    }
    return true;
}

bool readMixPoints(const QSqlDatabase& db, BrainSnapshot* pSnapshot, QString* pError) {
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral(
                "SELECT location, mix_in_sec, music_start_sec, music_end_sec FROM mix_points"))) {
        *pError = query.lastError().text();
        return false;
    }
    while (query.next()) {
        BrainTrackInfo& info = entryFor(pSnapshot, query.value(0).toString());
        info.hasMixPoints = true;
        info.mixInSec = query.value(1).toDouble();
        info.musicStartSec = query.value(2).toDouble();
        info.musicEndSec = query.value(3).toDouble();
    }
    return true;
}

bool readTrackIndex(const QSqlDatabase& db, BrainSnapshot* pSnapshot, QString* pError) {
    const QSet<QString> columns = columnsOf(db, kTrackIndexTable);
    if (!columns.contains(QStringLiteral("location")) ||
            !columns.contains(QStringLiteral("state"))) {
        // Not the ADR 0026 table we know: ignore it rather than guess.
        pSnapshot->hasIndexTables = false;
        return true;
    }
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("SELECT location, state, %1 FROM track_index")
                            .arg(column(columns, QStringLiteral("error"))))) {
        *pError = query.lastError().text();
        return false;
    }
    while (query.next()) {
        BrainTrackInfo& info = entryFor(pSnapshot, query.value(0).toString());
        info.inIndex = true;
        info.indexState = query.value(1).toString();
        info.indexError = query.value(2).toString();
    }
    return true;
}

bool readProgress(const QSqlDatabase& db, BrainSnapshot* pSnapshot, QString* pError) {
    const QSet<QString> columns = columnsOf(db, kProgressTable);
    if (!columns.contains(QStringLiteral("location")) ||
            !columns.contains(QStringLiteral("state"))) {
        return true; // not the shape we know: ignore rather than guess
    }
    pSnapshot->hasProgressTable = true;
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("SELECT location, state, %1, %2, %3, %4 FROM analysis_progress")
                            .arg(column(columns, QStringLiteral("step")),
                                    column(columns, QStringLiteral("step_progress")),
                                    column(columns, QStringLiteral("progress")),
                                    column(columns, QStringLiteral("error"))))) {
        *pError = query.lastError().text();
        return false;
    }
    while (query.next()) {
        BrainTrackInfo& info = entryFor(pSnapshot, query.value(0).toString());
        info.hasProgress = true;
        info.progressState = query.value(1).toString();
        info.progressStep = query.value(2).toString();
        info.stepProgress = optionalDouble(query.value(3));
        info.progress = optionalDouble(query.value(4));
        info.progressError = query.value(5).toString().trimmed();
    }
    return true;
}

void readServiceStatus(const QSqlDatabase& db, BrainSnapshot* pSnapshot) {
    const QSet<QString> columns = columnsOf(db, kStatusTable);
    if (!columns.contains(QStringLiteral("level")) ||
            !columns.contains(QStringLiteral("heartbeat_at"))) {
        return;
    }
    QSqlQuery query(db);
    const QString sql = QStringLiteral(
            "SELECT level, heartbeat_at, %1, %2, %3, %4, %5, %6, %7 FROM brain_status "
            "ORDER BY %8 LIMIT 1")
                                .arg(column(columns, QStringLiteral("paused_reason")),
                                        column(columns, QStringLiteral("message")),
                                        column(columns, QStringLiteral("workers_active")),
                                        column(columns, QStringLiteral("workers_limit")),
                                        column(columns, QStringLiteral("queued")),
                                        column(columns, QStringLiteral("running")),
                                        column(columns, QStringLiteral("failed")),
                                        columns.contains(QStringLiteral("id"))
                                                ? QStringLiteral("id")
                                                : QStringLiteral("rowid"));
    if (!query.exec(sql) || !query.next()) {
        return; // status is informative only; never fail the snapshot for it
    }
    BrainServiceStatus& status = pSnapshot->service;
    status.present = true;
    status.level = query.value(0).toString();
    // SQLite datetime('now'): "YYYY-MM-DD HH:MM:SS", UTC.
    QDateTime heartbeat = QDateTime::fromString(
            query.value(1).toString().left(19), QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    if (!heartbeat.isValid()) {
        heartbeat = QDateTime::fromString(query.value(1).toString(), Qt::ISODate);
    }
    if (heartbeat.isValid()) {
        heartbeat.setTimeZone(QTimeZone::UTC);
    }
    status.heartbeatUtc = heartbeat;
    status.pausedReason = query.value(2).toString();
    status.message = query.value(3).toString();
    status.workersActive = query.value(4).toInt();
    status.workersLimit = query.value(5).toInt();
    status.queued = query.value(6).toInt();
    status.running = query.value(7).toInt();
    status.failed = query.value(8).toInt();
}

void readAll(const QSqlDatabase& db, BrainSnapshot* pSnapshot) {
    QString error;
    {
        QSqlQuery query(db);
        if (query.exec(QStringLiteral("PRAGMA journal_mode")) && query.next()) {
            pSnapshot->journalMode = query.value(0).toString().toLower();
        }
    }
    const QSet<QString> tables = tablesOf(db, &error);
    if (!error.isEmpty()) {
        // Not a database, unreadable, or locked longer than the busy timeout.
        pSnapshot->error = error;
        return;
    }
    for (const QString& table : kContractTables) {
        if (tables.contains(table)) {
            pSnapshot->tables.append(table);
        } else {
            pSnapshot->missingTables.append(table);
        }
    }
    pSnapshot->hasIndexTables = tables.contains(kTrackIndexTable);
    if (pSnapshot->hasIndexTables) {
        pSnapshot->tables.append(kTrackIndexTable);
    }

    bool ok = readPipeline(db, tables, pSnapshot, &error);
    if (ok && tables.contains(QStringLiteral("stem_exports"))) {
        ok = readStemExports(db, pSnapshot, &error);
    }
    if (ok && tables.contains(QStringLiteral("vocal_maps"))) {
        ok = readVocalMaps(db, pSnapshot, &error);
    }
    if (ok && tables.contains(QStringLiteral("grid_corrections"))) {
        ok = readGridCorrections(db, pSnapshot, &error);
    }
    if (ok && tables.contains(QStringLiteral("mix_points"))) {
        ok = readMixPoints(db, pSnapshot, &error);
    }
    if (ok && pSnapshot->hasIndexTables) {
        ok = readTrackIndex(db, pSnapshot, &error);
    }
    if (ok && tables.contains(kProgressTable)) {
        ok = readProgress(db, pSnapshot, &error);
        if (pSnapshot->hasProgressTable) {
            pSnapshot->tables.append(kProgressTable);
        }
    }
    if (ok && tables.contains(kStatusTable)) {
        readServiceStatus(db, pSnapshot);
        if (pSnapshot->service.present) {
            pSnapshot->tables.append(kStatusTable);
        }
    }
    if (!ok) {
        pSnapshot->error = error;
        pSnapshot->tracks.clear();
        return;
    }
    pSnapshot->ok = true;
}

} // namespace

bool BrainTrackInfo::tempoIsVariable() const {
    return tempoFlag.trimmed().startsWith(QLatin1String("variabil"), Qt::CaseInsensitive);
}

bool BrainServiceStatus::isOnline(const QDateTime& nowUtc, int maxAgeSec) const {
    if (!present || !heartbeatUtc.isValid() || level == QLatin1String("stopped")) {
        return false;
    }
    const qint64 age = heartbeatUtc.secsTo(nowUtc);
    // A heartbeat slightly in the future (clock skew) still counts.
    return age <= maxAgeSec && age >= -maxAgeSec;
}

const BrainTrackInfo* BrainSnapshot::find(const QString& location) const {
    const auto it = tracks.constFind(BrainDbReader::locationKey(location));
    return it == tracks.constEnd() ? nullptr : &it.value();
}

// static
QString BrainDbReader::configuredPath(const QString& configValue) {
    return configValue.trimmed();
}

// static
QString BrainDbReader::locationKey(const QString& location) {
    return GridCorrector::normalizedLocation(location);
}

// static
bool BrainDbReader::withReadOnlyConnection(const QString& dbPath,
        int busyTimeoutMs,
        const std::function<void(const QSqlDatabase&)>& readFn,
        QString* pError) {
    if (dbPath.isEmpty()) {
        *pError = QStringLiteral("[DJApp],BrainDb nu e setat în mixxx.cfg");
        return false;
    }
    if (!QFileInfo::exists(dbPath)) {
        // QSQLITE_OPEN_READONLY would refuse it anyway; never create one.
        *pError = QStringLiteral("brain.db nu există: %1").arg(dbPath);
        return false;
    }
    bool opened = false;
    const QString connectionName = QStringLiteral("djapp_braindb_reader_%1")
                                           .arg(s_connectionCounter.fetchAndAddRelaxed(1));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        db.setDatabaseName(dbPath);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=%1")
                                     .arg(qMax(0, busyTimeoutMs)));
        if (!db.open()) {
            *pError = db.lastError().text();
        } else {
            opened = true;
            // One deferred (read) transaction: every table from the same
            // snapshot, even while brain commits. Never writes; rolled back.
            const bool inTransaction = db.transaction();
            readFn(db);
            if (inTransaction) {
                db.rollback();
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);
    return opened;
}

// static
QSet<QString> BrainDbReader::tableNames(const QSqlDatabase& db, QString* pError) {
    return tablesOf(db, pError);
}

// static
QSet<QString> BrainDbReader::columnNames(const QSqlDatabase& db, const QString& table) {
    return columnsOf(db, table);
}

// static
BrainSnapshot BrainDbReader::read(const QString& dbPath, int busyTimeoutMs) {
    BrainSnapshot snapshot;
    snapshot.dbPath = dbPath;
    snapshot.readAt = QDateTime::currentDateTime();
    QElapsedTimer timer;
    timer.start();
    withReadOnlyConnection(
            dbPath,
            busyTimeoutMs,
            [&snapshot](const QSqlDatabase& db) {
                readAll(db, &snapshot);
            },
            &snapshot.error);
    snapshot.readMs = timer.elapsed();
    if (!snapshot.ok) {
        qWarning() << "DJ App: cannot read brain.db" << dbPath << snapshot.error;
    }
    return snapshot;
}
