#include <gtest/gtest.h>

#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtDebug>

#include "library/djapp/braindbreader.h"
#include "library/djapp/djappanalysis.h"
#include "mixer/gridcorrector.h"

// DJ App, docs/decisions/0029: the read-only brain.db layer of the "DJ App"
// library feature and the rows of its "Analiză" view. Fixtures are temporary
// SQLite files shaped like brain/store/schema.sql (and older / newer variants).

namespace {

// brain writes Mixxx locations, sometimes with backslashes (older imports).
const QString kAsa = QStringLiteral("C:/Users/ADMIN/Downloads/DJ Daniel - Așa ești tu.mp3");
const QString kAsaBackslash =
        QStringLiteral("C:\\Users\\ADMIN\\Downloads\\DJ Daniel - Așa ești tu.mp3");
const QString kAsaStem = QStringLiteral(
        "D:/Projects/dj-app/brain/stems_export/DJ Daniel - Așa ești tu.stem.mp4");
const QString kAgua = QStringLiteral("C:/Users/ADMIN/Downloads/Água fresca.mp3");
const QString kRaissa = QStringLiteral("C:/Users/ADMIN/Downloads/Raïssa.mp3");
const QString kBroken = QStringLiteral("C:/Users/ADMIN/Downloads/Stricată.mp3");
const QString kNew = QStringLiteral("C:/Users/ADMIN/Downloads/Nouă.mp3");

// A writer connection for building fixtures (the code under test never writes).
class SqliteWriter {
  public:
    SqliteWriter(const QString& path, const QString& connectionName)
            : m_connectionName(connectionName) {
        m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
        m_db.setDatabaseName(path);
        m_open = m_db.open();
    }
    ~SqliteWriter() {
        close();
    }
    bool isOpen() const {
        return m_open;
    }
    bool exec(const QString& sql) {
        QSqlQuery query(m_db);
        if (!query.exec(sql)) {
            qWarning() << "fixture SQL failed:" << sql << query.lastError().text();
            return false;
        }
        return true;
    }
    QString scalar(const QString& sql) {
        QSqlQuery query(m_db);
        if (!query.exec(sql) || !query.next()) {
            return QString();
        }
        return query.value(0).toString();
    }
    bool execAll(const QStringList& statements) {
        for (const QString& sql : statements) {
            if (!exec(sql)) {
                return false;
            }
        }
        return true;
    }
    void close() {
        if (m_connectionName.isEmpty()) {
            return;
        }
        m_db.close();
        m_db = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connectionName);
        m_connectionName.clear();
    }

  private:
    QString m_connectionName;
    QSqlDatabase m_db;
    bool m_open = false;
};

const QStringList kSchema = {
        QStringLiteral("CREATE TABLE tracks (id INTEGER PRIMARY KEY, mixxx_track_id INTEGER, "
                       "path TEXT NOT NULL UNIQUE, audio_hash TEXT NOT NULL DEFAULT '', "
                       "analysis_status TEXT NOT NULL DEFAULT 'pending', analysis_error TEXT)"),
        QStringLiteral("CREATE TABLE analysis_queue (track_id INTEGER PRIMARY KEY, "
                       "priority INTEGER NOT NULL DEFAULT 0, "
                       "stage TEXT NOT NULL DEFAULT 'pending', "
                       "attempts INTEGER NOT NULL DEFAULT 0, last_error TEXT)"),
        QStringLiteral("CREATE TABLE stem_exports (source_location TEXT PRIMARY KEY, "
                       "track_id INTEGER, stem_path TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE vocal_maps (location TEXT PRIMARY KEY, "
                       "vocal_activity BLOB NOT NULL, "
                       "has_vocals INTEGER NOT NULL DEFAULT 1, vocal_share_db REAL)"),
        QStringLiteral("CREATE TABLE grid_corrections (location TEXT PRIMARY KEY, "
                       "original_bpm REAL NOT NULL, sample_rate INTEGER NOT NULL DEFAULT 44100, "
                       "correction_beats REAL NOT NULL, confidence REAL NOT NULL, "
                       "apply INTEGER NOT NULL, reason TEXT NOT NULL, "
                       "kind TEXT NOT NULL DEFAULT 'phase', corrected_bpm REAL, "
                       "corrected_first_beat_frame REAL, tempo_flag TEXT, "
                       "drift_before REAL, drift_after REAL)"),
        QStringLiteral("CREATE TABLE mix_points (location TEXT PRIMARY KEY, "
                       "music_start_sec REAL NOT NULL, music_end_sec REAL NOT NULL, "
                       "mix_in_sec REAL NOT NULL, mix_out_sec TEXT NOT NULL)"),
};

QStringList fixtureRows() {
    return {
            // Așa ești tu: done, backslash path (like the oldest real rows).
            QStringLiteral("INSERT INTO tracks (id, path, analysis_status) VALUES (1, '%1', 'done')")
                    .arg(kAsaBackslash),
            QStringLiteral("INSERT INTO analysis_queue (track_id, stage) VALUES (1, 'done')"),
            // Água fresca: waiting; an empty last_error is not an error.
            QStringLiteral("INSERT INTO tracks (id, path) VALUES (2, '%1')").arg(kAgua),
            QStringLiteral("INSERT INTO analysis_queue (track_id, stage, attempts, last_error) "
                           "VALUES (2, 'pending', 1, '')"),
            // Raïssa: Demucs running after a failed attempt.
            QStringLiteral("INSERT INTO tracks (id, path, analysis_status) "
                           "VALUES (3, '%1', 'in_progress')")
                    .arg(kRaissa),
            QStringLiteral("INSERT INTO analysis_queue (track_id, stage, attempts, last_error) "
                           "VALUES (3, 'stems', 2, 'Demucs: out of memory')"),
            // Stricată: attempts exhausted.
            QStringLiteral("INSERT INTO tracks (id, path, analysis_status, analysis_error) "
                           "VALUES (4, '%1', 'failed', 'cannot decode')")
                    .arg(kBroken),
            QStringLiteral("INSERT INTO analysis_queue (track_id, stage, attempts, last_error) "
                           "VALUES (4, 'features', 3, 'cannot decode')"),
            QStringLiteral("INSERT INTO stem_exports VALUES ('%1', 1, '%2')").arg(kAsa, kAsaStem),
            QStringLiteral("INSERT INTO vocal_maps VALUES ('%1', x'00', 1, -9.5)").arg(kAsa),
            // brain writes b'' (not NULL) for an instrumental.
            QStringLiteral("INSERT INTO vocal_maps VALUES ('%1', x'', 0, NULL)").arg(kAgua),
            QStringLiteral("INSERT INTO grid_corrections (location, original_bpm, "
                           "correction_beats, confidence, apply, reason) "
                           "VALUES ('%1', 105.0, 0.49, 0.8, 1, 'contratimp')")
                    .arg(kAsa),
            QStringLiteral("INSERT INTO grid_corrections (location, original_bpm, "
                           "correction_beats, confidence, apply, reason, tempo_flag, "
                           "drift_before) VALUES ('%1', 108.0, 0, 0.9, 0, 'ok', 'variabil', 0.45)")
                    .arg(kAgua),
            QStringLiteral("INSERT INTO grid_corrections (location, original_bpm, "
                           "correction_beats, confidence, apply, reason, kind, corrected_bpm, "
                           "corrected_first_beat_frame) VALUES ('%1', 118.73, 0, 0.7, 1, "
                           "'tempo x3/4', 'tempo', 89.041, 1234)")
                    .arg(kRaissa),
            QStringLiteral("INSERT INTO mix_points VALUES ('%1', 2.0, 271.0, 9.0, '{\"8\": 200.0}')")
                    .arg(kAsa),
    };
}

bool writeBrainDb(const QString& path, const QString& journalMode = QStringLiteral("delete")) {
    SqliteWriter writer(path, QStringLiteral("djapp_test_brain_writer"));
    return writer.isOpen() &&
            writer.scalar(QStringLiteral("PRAGMA journal_mode = %1").arg(journalMode)) ==
            journalMode &&
            writer.execAll(kSchema) && writer.execAll(fixtureRows());
}

QByteArray fileBytes(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return file.readAll();
}

DJAppLibraryTrack libraryTrack(int id,
        const QString& location,
        double bpm,
        const QString& key,
        const QString& artist = QString(),
        const QString& title = QString()) {
    DJAppLibraryTrack track;
    track.id = id;
    track.location = location;
    track.bpm = bpm;
    track.key = key;
    track.artist = artist;
    track.title = title;
    return track;
}

// Mixxx's view of the same files: other case, other slashes, plus a new track
// and the exported .stem.mp4 that Dan added to the library.
QList<DJAppLibraryTrack> libraryTracks() {
    return {
            libraryTrack(1,
                    QStringLiteral("C:/Users/ADMIN/Downloads/DJ Daniel - AȘA EȘTI TU.mp3"),
                    105.0,
                    QStringLiteral("9A"),
                    QStringLiteral("DJ Daniel"),
                    QStringLiteral("Așa ești tu")),
            libraryTrack(2,
                    QStringLiteral("c:\\users\\admin\\downloads\\ÁGUA FRESCA.mp3"),
                    108.0,
                    QString()),
            libraryTrack(3, kRaissa, 118.73, QStringLiteral("8A")),
            libraryTrack(4, kBroken, 0.0, QString()),
            libraryTrack(5, kNew, 100.0, QStringLiteral("1A")),
            libraryTrack(6, kAsaStem, 105.0, QStringLiteral("9A")),
    };
}

const DJAppAnalysisRow& rowFor(const QList<DJAppAnalysisRow>& rows, int trackId) {
    for (const DJAppAnalysisRow& row : rows) {
        if (row.track.id == trackId) {
            return row;
        }
    }
    static const DJAppAnalysisRow kNone;
    ADD_FAILURE() << "no row for track" << trackId;
    return kNone;
}

} // namespace

class DJAppBrainDbReaderTest : public testing::Test {};

// ---------------------------------------------------------------- location rule

TEST_F(DJAppBrainDbReaderTest, LocationKeyIsGridCorrectorRuleWithDiacritics) {
    // Same cases as brain/tests/test_store_indexdb.py (location_key).
    EXPECT_EQ(QStringLiteral("c:/users/admin/downloads/água fresca.mp3"),
            BrainDbReader::locationKey(
                    QStringLiteral("C:\\Users\\ADMIN\\Downloads\\Água FRESCA.mp3")));
    EXPECT_EQ(QStringLiteral("c:/muzica/dj daniel - așa ești tu.mp4"),
            BrainDbReader::locationKey(QStringLiteral("C:/Muzica/DJ Daniel - AȘA EȘTI TU.mp4")));
    EXPECT_EQ(QStringLiteral("c:/muzica/raïssa.mp3"),
            BrainDbReader::locationKey(QStringLiteral("C:/Muzica/RAÏSSA.mp3")));
    // Qt lower-cases without folding: "ß" stays, no final-sigma rule.
    EXPECT_EQ(QStringLiteral("c:/straße.mp3"), BrainDbReader::locationKey(QStringLiteral("C:/STRAßE.mp3")));
    EXPECT_EQ(QStringLiteral("c:/οδοσ.mp3"), BrainDbReader::locationKey(QStringLiteral("C:/ΟΔΟΣ.mp3")));
    // Qt 6 uses the full mapping here: "İ" (U+0130) -> "i" + U+0307, two
    // characters. brain's store.indexdb.location_key must do the same; the
    // DJ App reader is not affected (it normalizes both sides in C++).
    EXPECT_EQ(QStringLiteral("c:/i\u0307.mp3"), BrainDbReader::locationKey(QStringLiteral("C:/İ.mp3")));
    // One rule for every DJ App lookup.
    const QString mixed = QStringLiteral("C:\\Muzica\\Grace ÉVORA – Ñ.mp3");
    EXPECT_EQ(GridCorrector::normalizedLocation(mixed), BrainDbReader::locationKey(mixed));
}

// ---------------------------------------------------------------- reading brain.db

TEST_F(DJAppBrainDbReaderTest, ReadsTheExistingContractReadOnly) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("brain.db"));
    ASSERT_TRUE(writeBrainDb(dbPath));
    const QByteArray before = fileBytes(dbPath);

    const BrainSnapshot brain = BrainDbReader::read(dbPath);
    ASSERT_TRUE(brain.ok) << brain.error.toStdString();
    EXPECT_EQ(QStringLiteral("delete"), brain.journalMode);
    EXPECT_TRUE(brain.missingTables.isEmpty()) << brain.missingTables.join(',').toStdString();
    EXPECT_FALSE(brain.hasIndexTables);
    EXPECT_FALSE(brain.hasProgressTable);
    EXPECT_FALSE(brain.service.present);
    EXPECT_EQ(4, brain.pipelineTracks);

    // Found with any slashes and case, diacritics included.
    const BrainTrackInfo* pAsa = brain.find(
            QStringLiteral("c:/users/admin/downloads/dj daniel - aȘa eȘti tu.MP3"));
    ASSERT_NE(nullptr, pAsa);
    EXPECT_TRUE(pAsa->inTracks);
    EXPECT_EQ(QStringLiteral("done"), pAsa->queueStage);
    EXPECT_EQ(kAsaStem, pAsa->stemExportPath);
    EXPECT_TRUE(pAsa->hasVocalMap);
    EXPECT_TRUE(pAsa->hasVocals);
    ASSERT_TRUE(pAsa->vocalShareDb.has_value());
    EXPECT_DOUBLE_EQ(-9.5, *pAsa->vocalShareDb);
    EXPECT_TRUE(pAsa->hasGridRow);
    EXPECT_TRUE(pAsa->gridApply);
    EXPECT_EQ(QStringLiteral("phase"), pAsa->gridKind);
    EXPECT_TRUE(pAsa->hasMixPoints);
    EXPECT_DOUBLE_EQ(9.0, pAsa->mixInSec);

    const BrainTrackInfo* pStem = brain.find(kAsaStem);
    ASSERT_NE(nullptr, pStem);
    EXPECT_EQ(kAsa, pStem->stemSourceLocation);
    EXPECT_FALSE(pStem->inTracks);

    const BrainTrackInfo* pAgua = brain.find(QStringLiteral("C:/USERS/ADMIN/DOWNLOADS/ÁGUA FRESCA.MP3"));
    ASSERT_NE(nullptr, pAgua);
    EXPECT_TRUE(pAgua->hasVocalMap);
    EXPECT_FALSE(pAgua->hasVocals);
    EXPECT_TRUE(pAgua->tempoIsVariable());
    EXPECT_TRUE(pAgua->analysisError.isEmpty());

    const BrainTrackInfo* pRaissa = brain.find(QStringLiteral("C:/Users/ADMIN/Downloads/RAÏSSA.mp3"));
    ASSERT_NE(nullptr, pRaissa);
    EXPECT_EQ(QStringLiteral("tempo"), pRaissa->gridKind);
    ASSERT_TRUE(pRaissa->gridCorrectedBpm.has_value());
    EXPECT_DOUBLE_EQ(89.041, *pRaissa->gridCorrectedBpm);
    EXPECT_EQ(QStringLiteral("Demucs: out of memory"), pRaissa->analysisError);
    EXPECT_EQ(2, pRaissa->attempts);

    EXPECT_EQ(nullptr, brain.find(kNew));

    // Never written, not even touched.
    EXPECT_EQ(before, fileBytes(dbPath));
    EXPECT_FALSE(QFile::exists(dbPath + QStringLiteral("-journal")));
}

TEST_F(DJAppBrainDbReaderTest, AnalysisRowsAndSummary) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("brain.db"));
    ASSERT_TRUE(writeBrainDb(dbPath));
    const BrainSnapshot brain = BrainDbReader::read(dbPath);
    ASSERT_TRUE(brain.ok);

    const QList<DJAppAnalysisRow> rows = DJAppAnalysis::buildRows(libraryTracks(), brain);
    ASSERT_EQ(6, rows.size());

    const DJAppAnalysisRow& asa = rowFor(rows, 1);
    EXPECT_EQ(QStringLiteral("DJ Daniel – Așa ești tu"), asa.trackText);
    EXPECT_EQ(DJAppBrainStage::Done, asa.stage);
    EXPECT_EQ(QStringLiteral("analizată"), asa.stageText);
    EXPECT_EQ(QStringLiteral("ok"), asa.mixxxText);
    EXPECT_EQ(QStringLiteral("da"), asa.stemText);
    EXPECT_TRUE(asa.vocalText.startsWith(QStringLiteral("voce"))) << asa.vocalText.toStdString();
    EXPECT_TRUE(asa.gridText.startsWith(QStringLiteral("de aplicat: contratimp")))
            << asa.gridText.toStdString();
    EXPECT_TRUE(asa.gridToApply);
    EXPECT_FALSE(asa.variableTempo);
    EXPECT_EQ(QStringLiteral("da"), asa.mixPointsText);
    EXPECT_EQ(QStringLiteral("—"), asa.indexText);

    const DJAppAnalysisRow& agua = rowFor(rows, 2);
    EXPECT_EQ(DJAppBrainStage::Queued, agua.stage);
    EXPECT_EQ(QStringLiteral("în coadă"), agua.stageText);
    EXPECT_EQ(QStringLiteral("fără tonalitate"), agua.mixxxText);
    EXPECT_EQ(QStringLiteral("instrumentală"), agua.vocalText);
    EXPECT_EQ(QStringLiteral("ok · tempo variabil"), agua.gridText);
    EXPECT_TRUE(agua.variableTempo);
    EXPECT_EQ(QStringLiteral("—"), agua.mixPointsText);

    const DJAppAnalysisRow& raissa = rowFor(rows, 3);
    EXPECT_EQ(DJAppBrainStage::Working, raissa.stage);
    EXPECT_EQ(QStringLiteral("în lucru: stems"), raissa.stageText);
    EXPECT_TRUE(raissa.stageTooltip.contains(QStringLiteral("out of memory")));
    EXPECT_EQ(QStringLiteral("de aplicat: tempo 118,73 → 89,04"), raissa.gridText);
    EXPECT_TRUE(raissa.gridToApply);

    const DJAppAnalysisRow& broken = rowFor(rows, 4);
    EXPECT_EQ(DJAppBrainStage::Error, broken.stage);
    EXPECT_EQ(QStringLiteral("eroare"), broken.stageText);
    EXPECT_TRUE(broken.stageTooltip.contains(QStringLiteral("cannot decode")));
    EXPECT_EQ(QStringLiteral("fără BPM"), broken.mixxxText);
    EXPECT_EQ(QStringLiteral("Stricată.mp3"), broken.trackText);

    const DJAppAnalysisRow& fresh = rowFor(rows, 5);
    EXPECT_EQ(DJAppBrainStage::NotInBrain, fresh.stage);
    EXPECT_EQ(QStringLiteral("—"), fresh.stageText);
    EXPECT_EQ(QStringLiteral("—"), fresh.gridText);

    // The .stem.mp4 copy: the original's analysis, its own grid.
    const DJAppAnalysisRow& stem = rowFor(rows, 6);
    EXPECT_TRUE(stem.isStemCopy);
    EXPECT_EQ(DJAppBrainStage::Done, stem.stage);
    EXPECT_EQ(QStringLiteral("dublură .stem"), stem.stemText);
    EXPECT_TRUE(stem.vocalText.startsWith(QStringLiteral("voce")));
    EXPECT_EQ(QStringLiteral("da"), stem.mixPointsText);
    EXPECT_EQ(QStringLiteral("—"), stem.gridText);

    const DJAppAnalysisSummary summary = DJAppAnalysis::summarize(rows);
    EXPECT_EQ(6, summary.total);
    EXPECT_EQ(2, summary.analysed);
    EXPECT_EQ(2, summary.pending);
    EXPECT_EQ(1, summary.errors);
    EXPECT_EQ(1, summary.notInBrain);
    EXPECT_EQ(1, summary.variableTempo);
    EXPECT_EQ(2, summary.gridToApply);
    EXPECT_EQ(3, summary.waiting());
    EXPECT_EQ(QStringLiteral(
                      "Analizate: 2 · în coadă: 2 · tempo variabil: 1 · erori: 1 · "
                      "nu sunt în brain: 1 · total: 6"),
            DJAppAnalysis::summaryText(summary));
}

TEST_F(DJAppBrainDbReaderTest, AppliedTempoCorrectionIsShownAsApplied) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("brain.db"));
    ASSERT_TRUE(writeBrainDb(dbPath));
    const BrainSnapshot brain = BrainDbReader::read(dbPath);
    ASSERT_TRUE(brain.ok);
    // Mixxx already set the corrected grid (library BPM = target).
    const DJAppAnalysisRow row = DJAppAnalysis::buildRow(
            libraryTrack(3, kRaissa, 89.041, QStringLiteral("8A")), brain);
    EXPECT_EQ(QStringLiteral("aplicată: tempo 118,73 → 89,04"), row.gridText);
    EXPECT_FALSE(row.gridToApply);
}

// ---------------------------------------------------------------- WAL and locks

TEST_F(DJAppBrainDbReaderTest, WalReaderIsNotBlockedByAWriterTransaction) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("brain.db"));
    ASSERT_TRUE(writeBrainDb(dbPath, QStringLiteral("wal")));

    SqliteWriter brainWriting(dbPath, QStringLiteral("djapp_test_wal_writer"));
    ASSERT_TRUE(brainWriting.isOpen());
    ASSERT_TRUE(brainWriting.exec(QStringLiteral("BEGIN IMMEDIATE")));
    ASSERT_TRUE(brainWriting.exec(QStringLiteral(
            "INSERT INTO tracks (id, path) VALUES (99, 'C:/x/uncommitted.mp3')")));

    QElapsedTimer timer;
    timer.start();
    // No busy wait allowed: in WAL the reader must not need one.
    const BrainSnapshot brain = BrainDbReader::read(dbPath, 0);
    EXPECT_LT(timer.elapsed(), 2000);
    ASSERT_TRUE(brain.ok) << brain.error.toStdString();
    EXPECT_EQ(QStringLiteral("wal"), brain.journalMode);
    // The last committed state, without the open transaction's row.
    EXPECT_EQ(4, brain.pipelineTracks);
    EXPECT_EQ(nullptr, brain.find(QStringLiteral("C:/x/uncommitted.mp3")));

    EXPECT_TRUE(brainWriting.exec(QStringLiteral("ROLLBACK")));
}

TEST_F(DJAppBrainDbReaderTest, WalDatabaseWithoutWalFilesIsReadable) {
    // brain stopped: its last connection closed, so -wal / -shm are gone.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("brain.db"));
    ASSERT_TRUE(writeBrainDb(dbPath, QStringLiteral("wal")));
    ASSERT_FALSE(QFile::exists(dbPath + QStringLiteral("-wal")));
    const QByteArray before = fileBytes(dbPath);

    const BrainSnapshot brain = BrainDbReader::read(dbPath);
    ASSERT_TRUE(brain.ok) << brain.error.toStdString();
    EXPECT_EQ(QStringLiteral("wal"), brain.journalMode);
    EXPECT_EQ(4, brain.pipelineTracks);
    EXPECT_EQ(before, fileBytes(dbPath));
}

TEST_F(DJAppBrainDbReaderTest, LockedRollbackJournalFailsFastInsteadOfHanging) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("brain.db"));
    ASSERT_TRUE(writeBrainDb(dbPath));

    SqliteWriter brainWriting(dbPath, QStringLiteral("djapp_test_exclusive_writer"));
    ASSERT_TRUE(brainWriting.isOpen());
    ASSERT_TRUE(brainWriting.exec(QStringLiteral("BEGIN EXCLUSIVE")));

    QElapsedTimer timer;
    timer.start();
    const BrainSnapshot brain = BrainDbReader::read(dbPath, 200);
    // Bounded by the busy timeout (SQLite's busy handler is coarse on Windows).
    EXPECT_LT(timer.elapsed(), 5000);
    EXPECT_FALSE(brain.ok);
    EXPECT_FALSE(brain.error.isEmpty());
    EXPECT_TRUE(brain.tracks.isEmpty());

    EXPECT_TRUE(brainWriting.exec(QStringLiteral("ROLLBACK")));
    brainWriting.close();
    // Once brain is done, the next read works.
    EXPECT_TRUE(BrainDbReader::read(dbPath, 200).ok);
}

// ---------------------------------------------------------------- schema variants

TEST_F(DJAppBrainDbReaderTest, OlderBrainDbWithoutNewTablesOrColumns) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("brain.db"));
    {
        SqliteWriter writer(dbPath, QStringLiteral("djapp_test_old_writer"));
        ASSERT_TRUE(writer.isOpen());
        ASSERT_TRUE(writer.execAll({
                QStringLiteral("CREATE TABLE tracks (id INTEGER PRIMARY KEY, path TEXT, "
                               "analysis_status TEXT)"),
                // grid_corrections before ADR 0025: no kind / tempo columns.
                QStringLiteral("CREATE TABLE grid_corrections (location TEXT PRIMARY KEY, "
                               "original_bpm REAL, correction_beats REAL, confidence REAL, "
                               "apply INTEGER, reason TEXT)"),
                QStringLiteral("INSERT INTO tracks VALUES (1, '%1', 'done')").arg(kAsa),
                QStringLiteral("INSERT INTO grid_corrections VALUES ('%1', 105, 1, 0.9, 1, "
                               "'paritate')")
                        .arg(kAsa),
        }));
    }
    const BrainSnapshot brain = BrainDbReader::read(dbPath);
    ASSERT_TRUE(brain.ok) << brain.error.toStdString();
    EXPECT_TRUE(brain.missingTables.contains(QStringLiteral("analysis_queue")));
    EXPECT_TRUE(brain.missingTables.contains(QStringLiteral("vocal_maps")));
    EXPECT_TRUE(brain.missingTables.contains(QStringLiteral("mix_points")));

    const DJAppAnalysisRow row = DJAppAnalysis::buildRow(
            libraryTrack(1, kAsa, 105.0, QStringLiteral("9A")), brain);
    EXPECT_EQ(DJAppBrainStage::Done, row.stage);
    EXPECT_EQ(QStringLiteral("de aplicat: paritate (+1,00 timpi)"), row.gridText);
    EXPECT_EQ(QStringLiteral("—"), row.vocalText);
    EXPECT_EQ(QStringLiteral("—"), row.mixPointsText);
    EXPECT_EQ(QStringLiteral("—"), row.stemText);
}

TEST_F(DJAppBrainDbReaderTest, IndexProgressAndStatusTablesWhenPresent) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("brain.db"));
    ASSERT_TRUE(writeBrainDb(dbPath));
    {
        SqliteWriter writer(dbPath, QStringLiteral("djapp_test_index_writer"));
        ASSERT_TRUE(writer.isOpen());
        ASSERT_TRUE(writer.execAll({
                // ADR 0026 (store/indexdb.py), the columns the reader uses.
                QStringLiteral("CREATE TABLE track_index (id INTEGER PRIMARY KEY, "
                               "location TEXT NOT NULL UNIQUE, location_key TEXT NOT NULL, "
                               "state TEXT NOT NULL, error TEXT)"),
                QStringLiteral("INSERT INTO track_index (location, location_key, state) "
                               "VALUES ('%1', 'k', 'rapid')")
                        .arg(kAgua),
                // Etapa 2 draft (brain-service, store/analysisprogress.py).
                QStringLiteral("CREATE TABLE analysis_progress (track_id INTEGER PRIMARY KEY, "
                               "location TEXT, location_key TEXT, state TEXT, step TEXT, "
                               "step_progress REAL, progress REAL, error TEXT, "
                               "attempts INTEGER)"),
                QStringLiteral("INSERT INTO analysis_progress (track_id, location, state, step, "
                               "step_progress, progress) VALUES (2, '%1', 'lucru', 'stems', "
                               "0.74, 0.5)")
                        .arg(kAgua),
                QStringLiteral("CREATE TABLE brain_status (id INTEGER PRIMARY KEY, pid INTEGER, "
                               "heartbeat_at TEXT, level TEXT, paused_reason TEXT, "
                               "workers_active INTEGER, workers_limit INTEGER, queued INTEGER, "
                               "running INTEGER, failed INTEGER, message TEXT)"),
                QStringLiteral("INSERT INTO brain_status (id, pid, heartbeat_at, level, "
                               "workers_active, workers_limit, queued, running, failed) "
                               "VALUES (1, 4242, '2026-10-08 20:00:00', 'light', 1, 3, 12, "
                               "1, 2)"),
        }));
    }
    const BrainSnapshot brain = BrainDbReader::read(dbPath);
    ASSERT_TRUE(brain.ok) << brain.error.toStdString();
    EXPECT_TRUE(brain.hasIndexTables);
    EXPECT_TRUE(brain.hasProgressTable);

    const DJAppAnalysisRow agua = DJAppAnalysis::buildRow(
            libraryTrack(2, kAgua, 108.0, QStringLiteral("9B")), brain);
    // The service's progress wins over the queue's coarser stage.
    EXPECT_EQ(DJAppBrainStage::Working, agua.stage);
    EXPECT_EQ(QStringLiteral("în lucru: stems 74 %"), agua.stageText);
    EXPECT_EQ(QStringLiteral("rapid (estimare)"), agua.indexText);
    const DJAppAnalysisRow asa = DJAppAnalysis::buildRow(
            libraryTrack(1, kAsa, 105.0, QStringLiteral("9A")), brain);
    EXPECT_EQ(QStringLiteral("—"), asa.indexText); // not indexed yet

    const BrainServiceStatus& status = brain.service;
    ASSERT_TRUE(status.present);
    EXPECT_EQ(QStringLiteral("light"), status.level);
    EXPECT_EQ(12, status.queued);
    const QDateTime heartbeat(QDate(2026, 10, 8), QTime(20, 0, 0), QTimeZone::UTC);
    EXPECT_EQ(heartbeat, status.heartbeatUtc);
    EXPECT_TRUE(status.isOnline(heartbeat.addSecs(3)));
    EXPECT_FALSE(status.isOnline(heartbeat.addSecs(60)));
    BrainServiceStatus stopped = status;
    stopped.level = QStringLiteral("stopped");
    EXPECT_FALSE(stopped.isOnline(heartbeat.addSecs(1)));
}

TEST_F(DJAppBrainDbReaderTest, MissingOrUnsetBrainDbIsNeverCreated) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    EXPECT_FALSE(BrainDbReader::read(QString()).ok);
    const QString missing = dir.filePath(QStringLiteral("missing.db"));
    const BrainSnapshot brain = BrainDbReader::read(missing);
    EXPECT_FALSE(brain.ok);
    EXPECT_FALSE(brain.error.isEmpty());
    EXPECT_FALSE(QFile::exists(missing));

    // Without brain.db every library track is simply "not in brain".
    const QList<DJAppAnalysisRow> rows = DJAppAnalysis::buildRows(libraryTracks(), brain);
    EXPECT_EQ(6, DJAppAnalysis::summarize(rows).notInBrain);
}

TEST_F(DJAppBrainDbReaderTest, NotADatabaseIsAnErrorNotACrash) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("brain.db"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("this is not sqlite, just bytes that are long enough to look like a header");
    }
    const QByteArray before = fileBytes(path);
    const BrainSnapshot brain = BrainDbReader::read(path);
    EXPECT_FALSE(brain.ok);
    EXPECT_EQ(before, fileBytes(path));
}

// ---------------------------------------------------------------- Mixxx side

TEST_F(DJAppBrainDbReaderTest, ReadsLibraryTracksSkippingDeletedOnes) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("mixxxdb.sqlite"));
    SqliteWriter mixxxDb(path, QStringLiteral("djapp_test_mixxxdb"));
    ASSERT_TRUE(mixxxDb.isOpen());
    ASSERT_TRUE(mixxxDb.execAll({
            QStringLiteral("CREATE TABLE track_locations (id INTEGER PRIMARY KEY, "
                           "location TEXT)"),
            QStringLiteral("CREATE TABLE library (id INTEGER PRIMARY KEY, artist TEXT, "
                           "title TEXT, bpm REAL, key TEXT, location INTEGER, "
                           "mixxx_deleted INTEGER DEFAULT 0)"),
            QStringLiteral("INSERT INTO track_locations VALUES (10, '%1'), (11, '%2')")
                    .arg(kAsa, kAgua),
            QStringLiteral("INSERT INTO library VALUES (1, 'DJ Daniel', 'Așa ești tu', 105.0, "
                           "'9A', 10, 0), (2, 'X', 'Água fresca', 108, '9B', 11, 1)"),
    }));
    QSqlDatabase db = QSqlDatabase::database(QStringLiteral("djapp_test_mixxxdb"));
    QString error;
    const QList<DJAppLibraryTrack> tracks = DJAppAnalysis::readLibraryTracks(db, &error);
    db = QSqlDatabase();
    EXPECT_TRUE(error.isEmpty()) << error.toStdString();
    ASSERT_EQ(1, tracks.size());
    EXPECT_EQ(1, tracks[0].id);
    EXPECT_EQ(kAsa, tracks[0].location);
    EXPECT_EQ(QStringLiteral("Așa ești tu"), tracks[0].title);
    EXPECT_DOUBLE_EQ(105.0, tracks[0].bpm);
    EXPECT_EQ(QStringLiteral("9A"), tracks[0].key);
}
