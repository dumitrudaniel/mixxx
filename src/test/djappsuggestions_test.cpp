#include "library/djapp/djappsuggestions.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtDebug>

#include "library/djapp/braindbreader.h"

// DJ App, docs/decisions/0029 (Sugestii): the suggestions query layer, on
// fixture SQLite files shaped like brain/store/indexdb.py (track_index +
// pair_scores, ADR 0026) and like mixxxdb.sqlite. index-builder's
// automix/pairindex.py SUGGEST_SQL is the contract this mirrors.

namespace {

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

const QString kSourceA = QStringLiteral("C:/Users/ADMIN/Downloads/Kakile Remix - Dj Znobia.mp3");
const QString kB1 = QStringLiteral("C:/Users/ADMIN/Downloads/Livongh.mp3");
const QString kB2 = QStringLiteral("C:/Users/ADMIN/Downloads/Café Koka.mp3");
const QString kB3 = QStringLiteral("C:/Users/ADMIN/Downloads/Raïssa.mp3");
const QString kRisky1 = QStringLiteral("C:/Users/ADMIN/Downloads/Grace Evora.mp3");
const QString kRisky2 = QStringLiteral("C:/Users/ADMIN/Downloads/Nigivir.mp3");
const QString kAsaStem = QStringLiteral(
        "D:/Projects/dj-app/brain/stems_export/DJ Daniel - Așa ești tu.stem.mp4");
const QString kAsaOriginal = QStringLiteral("C:/Users/ADMIN/Downloads/DJ Daniel - Așa ești tu.mp3");

const QStringList kIndexSchema = {
        QStringLiteral(
                "CREATE TABLE track_index (id INTEGER PRIMARY KEY, location TEXT NOT NULL "
                "UNIQUE, location_key TEXT NOT NULL, state TEXT NOT NULL DEFAULT 'complet', "
                "bpm REAL, camelot TEXT, key TEXT)"),
        QStringLiteral(
                "CREATE TABLE pair_scores (recipe TEXT NOT NULL, a_id INTEGER NOT NULL, "
                "b_id INTEGER NOT NULL, score REAL NOT NULL, allowed INTEGER NOT NULL, "
                "hard_ok INTEGER NOT NULL DEFAULT 1, clash INTEGER NOT NULL, "
                "step_bpm REAL NOT NULL, terms TEXT NOT NULL, parts TEXT NOT NULL DEFAULT '{}', "
                "flags TEXT NOT NULL, intro_note TEXT NOT NULL, outro_note TEXT NOT NULL, "
                "grid_bad TEXT NOT NULL DEFAULT '', loud_jump_db REAL, bright_jump_db REAL, "
                "key_guard INTEGER NOT NULL, recipe_hint TEXT NOT NULL, "
                "PRIMARY KEY (recipe, a_id, b_id))"),
        QStringLiteral("CREATE TABLE stem_exports (source_location TEXT PRIMARY KEY, "
                       "track_id INTEGER, stem_path TEXT NOT NULL)"),
};

QString termsJson(double key, double intro) {
    return QStringLiteral(
            "{\"key\":%1,\"intro\":%2,\"outro\":0.9,\"energy\":1.0,\"play\":0.8,"
            "\"tempo\":0.9,\"vibe\":0.5}")
            .arg(key)
            .arg(intro);
}

QString indexRow(int id, const QString& location, double bpm, const QString& camelot) {
    return QStringLiteral(
            "INSERT INTO track_index (id, location, location_key, bpm, camelot) VALUES "
            "(%1, '%2', '%3', %4, '%5')")
            .arg(id)
            .arg(location, BrainDbReader::locationKey(location))
            .arg(bpm)
            .arg(camelot);
}

QString pairRow(int aId,
        int bId,
        double score,
        bool allowed,
        bool clash,
        double stepBpm,
        const QString& flags = QString(),
        bool keyGuard = false,
        const QString& recipeHint = QStringLiteral("standard8")) {
    return QStringLiteral(
            "INSERT INTO pair_scores (recipe, a_id, b_id, score, allowed, clash, step_bpm, "
            "terms, flags, intro_note, outro_note, key_guard, recipe_hint) VALUES "
            "('standard8', %1, %2, %3, %4, %5, %6, '%7', '%8', 'tobe bara 0, bas +1dB', "
            "'voce gata la 30', %9, '%10')")
            .arg(aId)
            .arg(bId)
            .arg(score)
            .arg(allowed ? 1 : 0)
            .arg(clash ? 1 : 0)
            .arg(stepBpm)
            .arg(termsJson(clash ? 0.0 : 0.9, 0.95))
            .arg(flags)
            .arg(keyGuard ? 1 : 0)
            .arg(recipeHint);
}

bool writeBasicIndex(const QString& path) {
    SqliteWriter writer(path, QStringLiteral("djapp_sugg_test_writer"));
    if (!writer.isOpen() || !writer.execAll(kIndexSchema)) {
        return false;
    }
    QStringList rows;
    rows << indexRow(1, kSourceA, 105.0, QStringLiteral("9A"));
    rows << indexRow(2, kB1, 106.0, QStringLiteral("9A"));
    rows << indexRow(3, kB2, 102.0, QStringLiteral("10A"));
    rows << indexRow(4, kB3, 104.0, QStringLiteral("8A"));
    rows << indexRow(5, kRisky1, 104.0, QStringLiteral("5A"));
    rows << indexRow(6, kRisky2, 107.0, QStringLiteral("5A"));
    // Allowed, descending score.
    rows << pairRow(1, 2, 0.94, true, false, 1.0);
    rows << pairRow(1, 3, 0.91, true, false, -3.0, QStringLiteral("salt de volum"));
    rows << pairRow(1, 4, 0.82, true, false, -1.0);
    // Risky (keys clash), descending score.
    rows << pairRow(1, 5, 0.66, false, true, -1.0, QString(), true);
    rows << pairRow(1, 6, 0.60, false, true, 2.0, QString(), true);
    return writer.execAll(rows);
}

} // namespace

class DJAppSuggestionsTest : public testing::Test {};

// ---------------------------------------------------------------- formatting

TEST_F(DJAppSuggestionsTest, ParseTermsOrdersKnownTermsThenExtras) {
    const auto terms = DJAppSuggestions::parseTerms(
            QStringLiteral("{\"vibe\":0.5,\"key\":0.9,\"release_drop_db\":-2.0,\"intro\":1.0}"));
    ASSERT_EQ(4, terms.size());
    EXPECT_EQ(QStringLiteral("key"), terms[0].name);
    EXPECT_EQ(QStringLiteral("intro"), terms[1].name);
    EXPECT_EQ(QStringLiteral("vibe"), terms[2].name);
    EXPECT_EQ(QStringLiteral("release_drop_db"), terms[3].name); // unknown term, listed last
    EXPECT_DOUBLE_EQ(0.9, terms[0].value);
    EXPECT_DOUBLE_EQ(-2.0, terms[3].value);
}

TEST_F(DJAppSuggestionsTest, ParseFlagsSplitsAndTrims) {
    EXPECT_EQ(QStringList({"voce veche taiata", "salt de volum"}),
            DJAppSuggestions::parseFlags(QStringLiteral("voce veche taiata|salt de volum")));
    EXPECT_TRUE(DJAppSuggestions::parseFlags(QString()).isEmpty());
    EXPECT_TRUE(DJAppSuggestions::parseFlags(QStringLiteral("|")).isEmpty());
}

TEST_F(DJAppSuggestionsTest, ScoreTextUsesDansCommaNotation) {
    EXPECT_EQ(QStringLiteral(",94"), DJAppSuggestions::scoreText(0.94));
    EXPECT_EQ(QStringLiteral("1,00"), DJAppSuggestions::scoreText(1.0));
    // Only 1,00 keeps its leading digit; 0,xx (including 0,00) is stripped.
    EXPECT_EQ(QStringLiteral(",00"), DJAppSuggestions::scoreText(0.0));
}

TEST_F(DJAppSuggestionsTest, ScoreBarLengthTracksScore) {
    EXPECT_TRUE(DJAppSuggestions::scoreBar(0.0).isEmpty());
    EXPECT_GT(DJAppSuggestions::scoreBar(1.0).size(), DJAppSuggestions::scoreBar(0.5).size());
}

TEST_F(DJAppSuggestionsTest, StepTextSignsAndRoundsToOneDecimal) {
    EXPECT_EQ(QStringLiteral("+2,0"), DJAppSuggestions::stepText(2.0));
    EXPECT_EQ(QStringLiteral("−3,0"), DJAppSuggestions::stepText(-3.0));
    EXPECT_EQ(QStringLiteral("±0"), DJAppSuggestions::stepText(0.02));
}

TEST_F(DJAppSuggestionsTest, KeyTextReportsMatchClashUnknownAndGuard) {
    DJAppSuggestion s;
    s.camelot = QStringLiteral("9B");
    s.clash = false;
    EXPECT_EQ(QStringLiteral("9A→9B ok"), DJAppSuggestions::keyText(QStringLiteral("9A"), s));
    s.clash = true;
    s.keyGuard = true;
    EXPECT_EQ(QStringLiteral("9A→9B se bat · GARDĂ TON"),
            DJAppSuggestions::keyText(QStringLiteral("9A"), s));
    DJAppSuggestion unknown;
    unknown.camelot = QString();
    EXPECT_EQ(QStringLiteral("9A→? necunoscută"),
            DJAppSuggestions::keyText(QStringLiteral("9A"), unknown));
}

TEST_F(DJAppSuggestionsTest, ShortReasonPrefersFlagsThenNotes) {
    DJAppSuggestion s;
    s.flags = {QStringLiteral("voce veche taiata")};
    EXPECT_EQ(QStringLiteral("voce veche taiata"), DJAppSuggestions::shortReason(s));
    DJAppSuggestion noFlags;
    noFlags.introNote = QStringLiteral("tobe bara 0, bas +1dB");
    noFlags.outroNote = QStringLiteral("voce gata la 30, -2->-1dB tine-0");
    EXPECT_EQ(QStringLiteral("tobe bara 0 · voce gata la 30"),
            DJAppSuggestions::shortReason(noFlags));
}

TEST_F(DJAppSuggestionsTest, WhyLinesCoverKeyIntroOutroFlagsAndScore) {
    DJAppSuggestion s;
    s.camelot = QStringLiteral("9A");
    s.stepBpm = 1.0;
    s.recipeHint = QStringLiteral("standard8");
    s.allowed = true;
    s.introNote = QStringLiteral("tobe bara 0");
    s.outroNote = QStringLiteral("voce gata la 30");
    s.flags = {QStringLiteral("salt de volum")};
    s.score = 0.82;
    s.terms = {{QStringLiteral("key"), 0.85}, {QStringLiteral("intro"), 0.9}};
    const QStringList lines = DJAppSuggestions::whyLines(QStringLiteral("9A"), s);
    ASSERT_EQ(5, lines.size());
    EXPECT_TRUE(lines[0].contains(QStringLiteral("Standard 8")));
    EXPECT_TRUE(lines[1].contains(QStringLiteral("tobe bara 0")));
    EXPECT_TRUE(lines[2].contains(QStringLiteral("voce gata la 30")));
    EXPECT_TRUE(lines[3].contains(QStringLiteral("salt de volum")));
    EXPECT_TRUE(lines[4].contains(QStringLiteral(",82")));
}

TEST_F(DJAppSuggestionsTest, DjDayStartIsSixAmLocalCrossingMidnight) {
    const QDateTime before(QDate(2026, 10, 9), QTime(2, 30), QTimeZone::systemTimeZone());
    EXPECT_EQ(QDate(2026, 10, 8), DJAppSuggestions::djDayStart(before).date());
    const QDateTime after(QDate(2026, 10, 9), QTime(10, 0), QTimeZone::systemTimeZone());
    EXPECT_EQ(QDate(2026, 10, 9), DJAppSuggestions::djDayStart(after).date());
}

// ---------------------------------------------------------------- query()

TEST_F(DJAppSuggestionsTest, AllowedAndRiskySplitOrderedByScore) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("brain.db"));
    ASSERT_TRUE(writeBasicIndex(dbPath));

    DJAppSuggestionRequest request;
    request.location = kSourceA;
    request.limit = 10;
    request.riskyLimit = 10;
    const DJAppSuggestionResult result = DJAppSuggestions::query(dbPath, request);

    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_TRUE(result.hasIndex);
    EXPECT_TRUE(result.sourceIndexed);
    EXPECT_EQ(QStringLiteral("9A"), result.sourceCamelot);
    ASSERT_EQ(3, result.allowed.size());
    EXPECT_EQ(kB1, result.allowed[0].location);
    EXPECT_DOUBLE_EQ(0.94, result.allowed[0].score);
    EXPECT_EQ(kB2, result.allowed[1].location);
    EXPECT_EQ(kB3, result.allowed[2].location);
    EXPECT_TRUE(result.allowed[0].allowed);
    EXPECT_FALSE(result.allowed[0].clash);

    ASSERT_EQ(2, result.risky.size());
    EXPECT_EQ(kRisky1, result.risky[0].location);
    EXPECT_TRUE(result.risky[0].clash);
    EXPECT_TRUE(result.risky[0].keyGuard);
    EXPECT_FALSE(result.risky[0].allowed);
}

TEST_F(DJAppSuggestionsTest, LimitsAreRespected) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("brain.db"));
    ASSERT_TRUE(writeBasicIndex(dbPath));

    DJAppSuggestionRequest request;
    request.location = kSourceA;
    request.limit = 2;
    request.riskyLimit = 1;
    const DJAppSuggestionResult result = DJAppSuggestions::query(dbPath, request);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(2, result.allowed.size());
    EXPECT_EQ(1, result.risky.size());
    EXPECT_EQ(kRisky1, result.risky[0].location); // the higher-scored risky one
}

TEST_F(DJAppSuggestionsTest, ExcludesSourceAndExplicitExclusions) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("brain.db"));
    ASSERT_TRUE(writeBasicIndex(dbPath));

    DJAppSuggestionRequest request;
    request.location = kSourceA;
    request.exclude = {kB1}; // "on a deck" / "played today" / dismissed
    const DJAppSuggestionResult result = DJAppSuggestions::query(dbPath, request);
    ASSERT_TRUE(result.ok);
    for (const DJAppSuggestion& s : result.allowed) {
        EXPECT_NE(kB1, s.location);
        EXPECT_NE(kSourceA, s.location);
    }
    EXPECT_GE(result.skipped, 1);
}

TEST_F(DJAppSuggestionsTest, RestrictsToPoolAndWidensLimitWhenManyAreFilteredOut) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("brain.db"));
    {
        SqliteWriter writer(dbPath, QStringLiteral("djapp_sugg_test_wide_writer"));
        ASSERT_TRUE(writer.isOpen());
        ASSERT_TRUE(writer.execAll(kIndexSchema));
        QStringList rows;
        rows << indexRow(1, kSourceA, 105.0, QStringLiteral("9A"));
        rows << indexRow(2, kB1, 106.0, QStringLiteral("9A"));
        rows << indexRow(3, kB3, 104.0, QStringLiteral("8A")); // the only pool member, score 0.82
        rows << pairRow(1, 2, 0.90, true, false, 1.0);
        rows << pairRow(1, 3, 0.82, true, false, -1.0);
        // 20 more candidates, all scored above kB3 (0.82) and not in the pool:
        // the initial LIMIT (tuned for "most rows pass") fills up with these
        // before reaching kB3, so SuggestionReader::fetch must retry with a
        // larger limit to find the one pool member.
        for (int i = 0; i < 20; ++i) {
            const int id = 100 + i;
            const QString location =
                    QStringLiteral("C:/Users/ADMIN/Downloads/Filler %1.mp3").arg(i);
            rows << indexRow(id, location, 105.0, QStringLiteral("9A"));
            rows << pairRow(1, id, 0.83 + i * 0.005, true, false, 0.5);
        }
        ASSERT_TRUE(writer.execAll(rows));
    }

    DJAppSuggestionRequest request;
    request.location = kSourceA;
    request.limit = 2;
    request.riskyLimit = 0;
    request.restrictToPool = true;
    request.pool = {kSourceA, kB3};
    const DJAppSuggestionResult result = DJAppSuggestions::query(dbPath, request);
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    ASSERT_EQ(1, result.allowed.size());
    EXPECT_EQ(kB3, result.allowed[0].location);
    EXPECT_EQ(kB3, result.allowed[0].poolLocation);
    EXPECT_GE(result.outsidePool, 20); // kB1 + the 20 fillers, at least
}

TEST_F(DJAppSuggestionsTest, StemExportResolvesToTheOriginalsRowOnBothSides) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("brain.db"));
    SqliteWriter writer(dbPath, QStringLiteral("djapp_sugg_test_stem_writer"));
    ASSERT_TRUE(writer.isOpen());
    ASSERT_TRUE(writer.execAll(kIndexSchema));
    ASSERT_TRUE(writer.execAll({
            indexRow(1, kAsaOriginal, 105.0, QStringLiteral("9A")),
            indexRow(2, kB1, 106.0, QStringLiteral("9A")),
            pairRow(1, 2, 0.9, true, false, 1.0),
            QStringLiteral("INSERT INTO stem_exports VALUES ('%1', 1, '%2')")
                    .arg(kAsaOriginal, kAsaStem),
    }));
    writer.close();

    // Dan loaded the .stem.mp4 in 2.6: the original's row is used.
    DJAppSuggestionRequest request;
    request.location = kAsaStem;
    const DJAppSuggestionResult result = DJAppSuggestions::query(dbPath, request);
    ASSERT_TRUE(result.ok);
    EXPECT_TRUE(result.sourceIndexed);
    EXPECT_TRUE(result.sourceViaStemExport);
    EXPECT_EQ(kAsaOriginal, result.sourceLocation);
    ASSERT_EQ(1, result.allowed.size());
    EXPECT_EQ(kB1, result.allowed[0].location);
}

TEST_F(DJAppSuggestionsTest, NoIndexTablesIsOkWithHasIndexFalse) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("brain.db"));
    {
        SqliteWriter writer(dbPath, QStringLiteral("djapp_sugg_test_empty_writer"));
        ASSERT_TRUE(writer.isOpen());
        ASSERT_TRUE(writer.exec(QStringLiteral("CREATE TABLE tracks (id INTEGER PRIMARY KEY)")));
    }
    DJAppSuggestionRequest request;
    request.location = kSourceA;
    const DJAppSuggestionResult result = DJAppSuggestions::query(dbPath, request);
    EXPECT_TRUE(result.ok);
    EXPECT_FALSE(result.hasIndex);
    EXPECT_TRUE(result.allowed.isEmpty());
}

TEST_F(DJAppSuggestionsTest, SourceNotIndexedIsOkWithSourceIndexedFalse) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("brain.db"));
    ASSERT_TRUE(writeBasicIndex(dbPath));

    DJAppSuggestionRequest request;
    request.location = QStringLiteral("C:/Users/ADMIN/Downloads/Piesa noua nou.mp3");
    const DJAppSuggestionResult result = DJAppSuggestions::query(dbPath, request);
    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(result.hasIndex);
    EXPECT_FALSE(result.sourceIndexed);
    EXPECT_TRUE(result.allowed.isEmpty());
}

TEST_F(DJAppSuggestionsTest, PairScoresMissingAKnownColumnIsAnError) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("brain.db"));
    SqliteWriter writer(dbPath, QStringLiteral("djapp_sugg_test_bad_schema_writer"));
    ASSERT_TRUE(writer.isOpen());
    ASSERT_TRUE(writer.execAll({
            QStringLiteral("CREATE TABLE track_index (id INTEGER PRIMARY KEY, location TEXT, "
                           "location_key TEXT)"),
            // Missing key_guard / recipe_hint (a hypothetical future schema change).
            QStringLiteral("CREATE TABLE pair_scores (recipe TEXT, a_id INTEGER, b_id INTEGER, "
                           "score REAL, allowed INTEGER, clash INTEGER, step_bpm REAL, "
                           "terms TEXT, flags TEXT, intro_note TEXT, outro_note TEXT)"),
    }));
    writer.close();

    DJAppSuggestionRequest request;
    request.location = kSourceA;
    const DJAppSuggestionResult result = DJAppSuggestions::query(dbPath, request);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.hasIndex);
    EXPECT_FALSE(result.error.isEmpty());
}

TEST_F(DJAppSuggestionsTest, MissingBrainDbIsNeverCreated) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString missing = dir.filePath(QStringLiteral("missing.db"));
    DJAppSuggestionRequest request;
    request.location = kSourceA;
    const DJAppSuggestionResult result = DJAppSuggestions::query(missing, request);
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(QFile::exists(missing));
}

// ---------------------------------------------------------------- Mixxx side

TEST_F(DJAppSuggestionsTest, ReadPlayedSinceUnionsHistoryAndLastPlayedAt) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("mixxxdb.sqlite"));
    SqliteWriter mixxxDb(path, QStringLiteral("djapp_sugg_test_mixxxdb"));
    ASSERT_TRUE(mixxxDb.isOpen());
    ASSERT_TRUE(mixxxDb.execAll({
            QStringLiteral("CREATE TABLE track_locations (id INTEGER PRIMARY KEY, "
                           "location TEXT)"),
            QStringLiteral("CREATE TABLE library (id INTEGER PRIMARY KEY, location INTEGER, "
                           "last_played_at TEXT, mixxx_deleted INTEGER DEFAULT 0)"),
            QStringLiteral("CREATE TABLE Playlists (id INTEGER PRIMARY KEY, hidden INTEGER)"),
            QStringLiteral("CREATE TABLE PlaylistTracks (playlist_id INTEGER, track_id INTEGER, "
                           "pl_datetime_added TEXT)"),
            QStringLiteral(
                    "INSERT INTO track_locations VALUES (1, '%1'), (2, '%2'), (3, '%3')")
                    .arg(kB1, kB2, kB3),
            // kB1: played today via last_played_at.
            QStringLiteral("INSERT INTO library VALUES (1, 1, '2026-10-08 20:00:00', 0)"),
            // kB2: in the history playlist today.
            QStringLiteral("INSERT INTO library VALUES (2, 2, NULL, 0)"),
            // kB3: played yesterday only; must not be excluded.
            QStringLiteral("INSERT INTO library VALUES (3, 3, '2026-10-07 10:00:00', 0)"),
            QStringLiteral("INSERT INTO Playlists VALUES (1, 2)"), // hidden = 2 = history
            QStringLiteral("INSERT INTO PlaylistTracks VALUES (1, 2, '2026-10-08 21:00:00')"),
    }));
    QSqlDatabase db = QSqlDatabase::database(QStringLiteral("djapp_sugg_test_mixxxdb"));
    const QDateTime since(QDate(2026, 10, 8), QTime(6, 0), QTimeZone::UTC);
    QString error;
    const QStringList played = DJAppSuggestions::readPlayedSince(db, since, &error);
    db = QSqlDatabase();
    EXPECT_TRUE(error.isEmpty()) << error.toStdString();
    EXPECT_TRUE(played.contains(kB1));
    EXPECT_TRUE(played.contains(kB2));
    EXPECT_FALSE(played.contains(kB3));
}

// ---------------------------------------------------------------- deck choice

TEST_F(DJAppSuggestionsTest, ChooseFreeDeckPrefersAnEmptyStoppedDeck) {
    const QList<DJAppDeckState> decks = {
            {QStringLiteral("[Channel1]"), true, true},
            {QStringLiteral("[Channel2]"), false, true},  // stopped, loaded
            {QStringLiteral("[Channel3]"), false, false}, // stopped, empty
    };
    EXPECT_EQ(QStringLiteral("[Channel3]"),
            DJAppSuggestions::chooseFreeDeck(decks, QStringLiteral("[Channel1]")));
}

TEST_F(DJAppSuggestionsTest, ChooseFreeDeckFallsBackToAnyStoppedDeckOtherThanSource) {
    const QList<DJAppDeckState> decks = {
            {QStringLiteral("[Channel1]"), true, true},
            {QStringLiteral("[Channel2]"), false, true},
    };
    EXPECT_EQ(QStringLiteral("[Channel2]"),
            DJAppSuggestions::chooseFreeDeck(decks, QStringLiteral("[Channel1]")));
}

TEST_F(DJAppSuggestionsTest, ChooseFreeDeckReturnsEmptyWhenEveryDeckIsBusy) {
    const QList<DJAppDeckState> decks = {
            {QStringLiteral("[Channel1]"), true, true},
            {QStringLiteral("[Channel2]"), true, true},
    };
    EXPECT_TRUE(DJAppSuggestions::chooseFreeDeck(decks, QStringLiteral("[Channel1]")).isEmpty());
}

// ---------------------------------------------------------------- feedback stub

TEST_F(DJAppSuggestionsTest, FeedbackJsonHasTheExpectedFields) {
    DJAppSuggestion s;
    s.location = kB1;
    s.score = 0.94;
    s.allowed = true;
    s.recipeHint = QStringLiteral("standard8");
    const QByteArray json =
            DJAppSuggestions::feedbackJson(kSourceA, s, QStringLiteral("ignored"), 2);
    const QString text = QString::fromUtf8(json);
    EXPECT_TRUE(text.contains(QStringLiteral("\"source\"")));
    EXPECT_TRUE(text.contains(kB1));
    EXPECT_TRUE(text.contains(QStringLiteral("\"outcome\":\"ignored\"")));
    EXPECT_TRUE(text.contains(QStringLiteral("\"position\":2")));
}
