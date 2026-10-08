#pragma once

#include <QDateTime>
#include <QHash>
#include <QString>
#include <QStringList>
#include <optional>

// DJ App (docs/decisions/0029): read-only access to brain.db for the "DJ App"
// library feature.
//
// brain/ is the only writer of brain.db. Mixxx opens it with
// QSQLITE_OPEN_READONLY (like GridCorrector and AutomixVocalMapStore), never
// creates it, never writes it. brain.db can be in journal_mode=delete (today)
// or WAL (index-builder, ADR 0026); both work. A busy timeout bounds the wait
// when brain holds a write lock in delete mode.
//
// read() is synchronous and meant for a worker thread (it opens its own
// uniquely named connection and removes it before returning), so the UI thread
// never waits on SQLite. All tables are read in one deferred transaction, a
// consistent snapshot even while brain writes.
//
// Locations are matched in C++ (GridCorrector::normalizedLocation: '/',
// QString::toLower, Unicode-aware), never with SQLite's ASCII-only lower().
// Tables or columns missing in an older brain.db are skipped, not errors.

// What brain.db knows about one audio file.
struct BrainTrackInfo {
    QString location; // as brain wrote it (first table that mentioned it)

    // tracks + analysis_queue (Faza 2 pipeline)
    bool inTracks = false;
    QString analysisStatus; // tracks.analysis_status: pending|in_progress|done|failed
    QString analysisError;  // tracks.analysis_error, else analysis_queue.last_error
    bool inQueue = false;
    QString queueStage; // analysis_queue.stage: pending|features|...|done
    int attempts = 0;

    // stem_exports: this file has a .stem.mp4 export (path), or this file IS
    // the export of another one (source).
    QString stemExportPath;
    QString stemSourceLocation;

    // vocal_maps
    bool hasVocalMap = false;
    bool hasVocals = false;
    std::optional<double> vocalShareDb;

    // grid_corrections (ADR 0019, 0025)
    bool hasGridRow = false;
    QString gridKind; // phase | tempo | tempo+phase ("" = before ADR 0025)
    bool gridApply = false;
    QString gridReason;
    QString tempoFlag; // "variabil", "in afara genului ...", "nesigur", "" = none
    double gridOriginalBpm = 0.0;
    std::optional<double> gridCorrectedBpm;
    double gridCorrectionBeats = 0.0;
    double gridConfidence = 0.0;
    std::optional<double> driftBefore;
    std::optional<double> driftAfter;

    // mix_points (ADR 0021, 0023)
    bool hasMixPoints = false;
    double mixInSec = 0.0;
    double musicStartSec = 0.0;
    double musicEndSec = 0.0;

    // track_index (ADR 0026, index-builder; optional table)
    bool inIndex = false;
    QString indexState; // complet | rapid | asteapta_mixxx | lipsa | eroare
    QString indexError;

    // analysis_progress (brain-service, Etapa 2, store/analysisprogress.py;
    // optional table, draft contract)
    bool hasProgress = false;
    QString progressState; // in_coada | lucru | asteapta_mixxx | complet | eroare | lipsa
    QString progressStep;  // features | beats | sections | stems | ... | "" = none
    std::optional<double> stepProgress; // 0..1 (Demucs); nullopt = indeterminate
    std::optional<double> progress;     // 0..1, whole track
    QString progressError;

    bool tempoIsVariable() const;
};

// brain_status (brain-service, Etapa 2; optional single-row table, draft).
struct BrainServiceStatus {
    bool present = false;
    QString level; // full | light | paused | stopped
    QString pausedReason;
    QDateTime heartbeatUtc; // invalid = never
    QString message;
    int workersActive = 0;
    int workersLimit = 0;
    int queued = 0;
    int running = 0;
    int failed = 0;

    // brain is running: a heartbeat younger than `maxAgeSec` and not stopped.
    bool isOnline(const QDateTime& nowUtc, int maxAgeSec = 10) const;
};

struct BrainSnapshot {
    QString dbPath;
    // brain.db was opened and read (possibly with some tables missing).
    bool ok = false;
    // Why it could not be read; empty when ok.
    QString error;
    QString journalMode; // "delete", "wal", ...
    QDateTime readAt;
    qint64 readMs = 0;
    // Tables of the existing contract found / not found in this brain.db.
    QStringList tables;
    QStringList missingTables;
    bool hasIndexTables = false;    // track_index (ADR 0026) present
    bool hasProgressTable = false;  // analysis_progress (Etapa 2) present
    BrainServiceStatus service;     // brain_status (Etapa 2), if present
    int pipelineTracks = 0;         // rows in brain's `tracks` table

    // Key: BrainDbReader::locationKey(location).
    QHash<QString, BrainTrackInfo> tracks;

    // The info for `location` (any slashes / case), or nullptr.
    const BrainTrackInfo* find(const QString& location) const;
};

class BrainDbReader {
  public:
    // [DJApp],BrainDb in mixxx.cfg (docs/decisions/0019).
    static QString configuredPath(const QString& configValue);

    // Reads everything the DJ App views show from brain.db at `dbPath`,
    // read-only. Never creates the file. `busyTimeoutMs` bounds the wait for
    // a writer's lock (delete-journal mode); WAL readers never wait.
    static BrainSnapshot read(const QString& dbPath, int busyTimeoutMs = 2000);

    // Same rule as GridCorrector::normalizedLocation and brain's
    // store.indexdb.location_key: '\' -> '/', QString::toLower().
    static QString locationKey(const QString& location);
};
