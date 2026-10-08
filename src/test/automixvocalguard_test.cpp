#include "mixer/automixvocalguard.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtEndian>
#include <cmath>
#include <cstring>
#include <limits>

#include "mixer/automixtransitionmath.h"
#include "track/beats.h"

// Mirrors brain/tests/test_automix_vocalguard.py (plan_guard) plus the C++
// specifics: float32 blobs, seconds -> grid beats on the deck's current grid,
// and the brain.db lookup (original file vs exported .stem.mp4).

namespace {

constexpr double kMaxWait = 32.0; // 8 bars, the recipe default
constexpr double kFade = 2.0;
// No handover cap: a recipe long enough not to matter.
constexpr double kNoHandoverCap = std::numeric_limits<double>::infinity();

AutomixVocalGuardPlan plan(const std::vector<AutomixVocalBlock>& outgoing,
        const std::vector<AutomixVocalBlock>& incoming) {
    return AutomixVocalGuardPlanner::plan(outgoing, incoming, kMaxWait, kFade, kNoHandoverCap);
}

QByteArray float32Blob(std::initializer_list<float> values) {
    QByteArray blob;
    for (const float value : values) {
        quint32 bits;
        std::memcpy(&bits, &value, sizeof(bits));
        char bytes[4];
        qToLittleEndian(bits, bytes);
        blob.append(bytes, 4);
    }
    return blob;
}

// 120 BPM at 44.1 kHz: one beat = 22050 frames = 0.5 s; beat 0 at 0.5 s.
mixxx::BeatsPointer grid120() {
    return mixxx::Beats::fromConstTempo(mixxx::audio::SampleRate(44100),
            mixxx::audio::FramePos(22050),
            mixxx::Bpm(120.0));
}

const QString kOriginal = QStringLiteral("C:/Users/ADMIN/Downloads/DJ Daniel - Așa ești tu.mp3");
const QString kStemFile = QStringLiteral(
        "D:/Projects/dj-app/brain/stems_export/DJ Daniel - Așa ești tu.stem.mp4");
const QString kInstrumental = QStringLiteral("C:/Users/ADMIN/Downloads/Água fresca.mp3");
const QString kInstrumentalStem =
        QStringLiteral("D:/Projects/dj-app/brain/stems_export/Água fresca.stem.mp4");

// A minimal brain.db with the columns the lookup reads.
bool writeBrainDb(const QString& path) {
    const QString connection = QStringLiteral("automixvocalguard_test_writer");
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(path);
        if (db.open()) {
            QSqlQuery query(db);
            ok = query.exec(QStringLiteral(
                         "CREATE TABLE vocal_maps (location TEXT PRIMARY KEY, "
                         "has_vocals INTEGER NOT NULL DEFAULT 1, "
                         "vocal_segments_sec BLOB NOT NULL)")) &&
                    query.exec(QStringLiteral(
                            "CREATE TABLE stem_exports (source_location TEXT PRIMARY KEY, "
                            "stem_path TEXT NOT NULL)"));
            const auto addMap = [&query](const QString& location,
                                        int hasVocals,
                                        const QByteArray& segments) {
                query.prepare(QStringLiteral(
                        "INSERT INTO vocal_maps (location, has_vocals, vocal_segments_sec) "
                        "VALUES (?, ?, ?)"));
                query.addBindValue(location);
                query.addBindValue(hasVocals);
                query.addBindValue(segments);
                return query.exec();
            };
            const auto addStem = [&query](const QString& source, const QString& stem) {
                query.prepare(QStringLiteral(
                        "INSERT INTO stem_exports (source_location, stem_path) VALUES (?, ?)"));
                query.addBindValue(source);
                query.addBindValue(stem);
                return query.exec();
            };
            ok = ok && addMap(kOriginal, 1, float32Blob({10.0f, 20.0f, 30.5f, 40.25f})) &&
                    // brain writes b'' (empty, not NULL) for an instrumental.
                    addMap(kInstrumental, 0, QByteArray("")) &&
                    addStem(kOriginal, kStemFile) && addStem(kInstrumental, kInstrumentalStem);
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return ok;
}

QByteArray fileBytes(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return file.readAll();
}

} // namespace

class AutomixVocalGuardTest : public testing::Test {
};

TEST_F(AutomixVocalGuardTest, OutgoingFinishesItsPhraseThenHandsOver) {
    const AutomixVocalGuardPlan p = plan({{-10, 6}}, {{4, 30}});
    EXPECT_DOUBLE_EQ(6.0, p.outgoingFadeStart);
    EXPECT_DOUBLE_EQ(8.0, p.incomingFadeStart);
    EXPECT_FALSE(p.cutMidPhrase);
    EXPECT_TRUE(p.incomingMidPhrase); // the new phrase started at 4
}

TEST_F(AutomixVocalGuardTest, SilentOutgoingHandsOverImmediately) {
    const AutomixVocalGuardPlan p = plan({{-20, -2}, {12, 30}}, {{10, 20}});
    EXPECT_DOUBLE_EQ(0.0, p.outgoingFadeStart);
    EXPECT_DOUBLE_EQ(2.0, p.incomingFadeStart);
    EXPECT_FALSE(p.cutMidPhrase);
    EXPECT_FALSE(p.incomingMidPhrase);
}

TEST_F(AutomixVocalGuardTest, EndlessPhraseIsCutAfterMaxWait) {
    const AutomixVocalGuardPlan p = plan({{-4, 100}}, {});
    EXPECT_DOUBLE_EQ(32.0, p.outgoingFadeStart); // 8 bars
    EXPECT_DOUBLE_EQ(32.0, p.incomingFadeStart); // cut: crossfade, no hole
    EXPECT_TRUE(p.cutMidPhrase);
}

TEST_F(AutomixVocalGuardTest, GainsAreEqualPowerAndComplete) {
    const AutomixVocalGuardPlan p = plan({{-1, 6}}, {});
    for (int i = 0; i <= 120; ++i) {
        const double beat = i * 0.1;
        if (beat < 6.0) {
            EXPECT_DOUBLE_EQ(1.0, p.outgoingGain(beat)) << beat;
        }
        if (beat >= 8.0) {
            EXPECT_DOUBLE_EQ(0.0, p.outgoingGain(beat)) << beat;
        }
        if (beat <= 8.0) {
            EXPECT_DOUBLE_EQ(0.0, p.incomingGain(beat)) << beat;
        }
        if (beat >= 10.0) {
            EXPECT_DOUBLE_EQ(1.0, p.incomingGain(beat)) << beat;
        }
    }
    // Same shapes as the band swaps: cos out, sin in.
    for (int i = 0; i <= 20; ++i) {
        const double x = i * 0.1;
        const double out = p.outgoingGain(6.0 + x);
        const double in = p.incomingGain(8.0 + x);
        EXPECT_NEAR(1.0, out * out + in * in, 1e-12) << x;
    }
    EXPECT_NEAR(std::sqrt(0.5), p.outgoingGain(7.0), 1e-12);
    EXPECT_NEAR(std::sqrt(0.5), p.incomingGain(9.0), 1e-12);
    EXPECT_DOUBLE_EQ(10.0, p.endBeat());
}

TEST_F(AutomixVocalGuardTest, EndlessPhraseIsCutAtTheLatestHandover) {
    // Standard 8 (32 beats): the outgoing voice hands over by the bass/high
    // swap at beat 16 even mid phrase, not after the whole transition.
    const AutomixVocalGuardPlan p = AutomixVocalGuardPlanner::plan({{-4, 100}},
            {{0, 200}},
            kMaxWait,
            kFade,
            AutomixVocalGuardPlanner::latestHandoverBeats(32.0));
    EXPECT_DOUBLE_EQ(16.0, p.outgoingFadeStart);
    EXPECT_DOUBLE_EQ(16.0, p.incomingFadeStart); // cut: crossfade, no hole
    EXPECT_TRUE(p.cutMidPhrase);
    EXPECT_DOUBLE_EQ(18.0, p.endBeat());
    // The crossfade is equal-power over the same two beats.
    for (int i = 0; i <= 20; ++i) {
        const double beat = 16.0 + i * 0.1;
        EXPECT_NEAR(1.0,
                p.outgoingGain(beat) * p.outgoingGain(beat) +
                        p.incomingGain(beat) * p.incomingGain(beat),
                1e-12)
                << beat;
    }
}

TEST_F(AutomixVocalGuardTest, PhraseEndingBeforeTheHandoverIsNotCut) {
    const AutomixVocalGuardPlan p =
            AutomixVocalGuardPlanner::plan({{-4, 10}}, {}, kMaxWait, kFade, 16.0);
    EXPECT_DOUBLE_EQ(10.0, p.outgoingFadeStart);
    EXPECT_DOUBLE_EQ(12.0, p.incomingFadeStart);
    EXPECT_FALSE(p.cutMidPhrase);
    // A recipe max wait shorter than the handover point still wins.
    const AutomixVocalGuardPlan shortWait =
            AutomixVocalGuardPlanner::plan({{-4, 10}}, {}, 8.0, kFade, 16.0);
    EXPECT_DOUBLE_EQ(8.0, shortWait.outgoingFadeStart);
    EXPECT_DOUBLE_EQ(8.0, shortWait.incomingFadeStart);
    EXPECT_TRUE(shortWait.cutMidPhrase);
}

TEST_F(AutomixVocalGuardTest, LatestHandoverIsHalfTheRecipe) {
    EXPECT_DOUBLE_EQ(4.0, AutomixVocalGuardPlanner::latestHandoverBeats(8.0));   // Urgenta 2
    EXPECT_DOUBLE_EQ(8.0, AutomixVocalGuardPlanner::latestHandoverBeats(16.0));  // Scurt 4
    EXPECT_DOUBLE_EQ(16.0, AutomixVocalGuardPlanner::latestHandoverBeats(32.0)); // Standard 8
    EXPECT_DOUBLE_EQ(32.0, AutomixVocalGuardPlanner::latestHandoverBeats(64.0)); // Lung 16
}

TEST_F(AutomixVocalGuardTest, ZeroFadeIsAStep) {
    const AutomixVocalGuardPlan p =
            AutomixVocalGuardPlanner::plan({{-1, 6}}, {}, kMaxWait, 0.0, kNoHandoverCap);
    EXPECT_DOUBLE_EQ(1.0, p.outgoingGain(5.99));
    EXPECT_DOUBLE_EQ(0.0, p.outgoingGain(6.0));
    EXPECT_DOUBLE_EQ(0.0, p.incomingGain(5.99));
    EXPECT_DOUBLE_EQ(1.0, p.incomingGain(6.0));
}

TEST_F(AutomixVocalGuardTest, BlocksMergeOverGapsShorterThanABar) {
    const std::vector<AutomixVocalBlock> merged = AutomixVocalGuardPlanner::mergeBlocks(
            {{10, 20}, {0, 8}, {22, 30}, {34, 40}});
    ASSERT_EQ(2u, merged.size());
    EXPECT_DOUBLE_EQ(0.0, merged[0].start);
    EXPECT_DOUBLE_EQ(30.0, merged[0].end); // gaps of 2 beats are one block
    EXPECT_DOUBLE_EQ(34.0, merged[1].start); // a gap of exactly 4 beats splits
    EXPECT_DOUBLE_EQ(40.0, merged[1].end);
}

TEST_F(AutomixVocalGuardTest, BreathBetweenLinesDoesNotHandOver) {
    // Silent at beat 0, but only for a 3-beat breath between two lines: the
    // voice is still in its block (vocal_end_after) and finishes it.
    const AutomixVocalGuardPlan p = plan({{-8, -1}, {2, 10}, {20, 30}}, {});
    EXPECT_DOUBLE_EQ(10.0, p.outgoingFadeStart);
    EXPECT_DOUBLE_EQ(12.0, p.incomingFadeStart);
}

TEST_F(AutomixVocalGuardTest, PhrasesMoveOntoTheTransitionAxis) {
    // Outgoing: grid beat 96 is the start bar.
    const auto outgoing = AutomixVocalGuardPlanner::toTransitionBeats({{100, 110}}, 96.0, 0.0);
    ASSERT_EQ(1u, outgoing.size());
    EXPECT_DOUBLE_EQ(4.0, outgoing[0].start);
    EXPECT_DOUBLE_EQ(14.0, outgoing[0].end);
    // Incoming: cue on its beat 8, started at transition beat 0.
    const auto incoming = AutomixVocalGuardPlanner::toTransitionBeats({{16, 40}}, 8.0, 0.0);
    ASSERT_EQ(1u, incoming.size());
    EXPECT_DOUBLE_EQ(8.0, incoming[0].start);
    EXPECT_DOUBLE_EQ(32.0, incoming[0].end);
    // Started on beat 6 (Fade curat, bar 1.5), playing at half the tempo of
    // the outgoing track: one of its beats lasts two transition beats.
    const auto late = AutomixVocalGuardPlanner::toTransitionBeats({{16, 40}}, 8.0, 6.0, 2.0);
    ASSERT_EQ(1u, late.size());
    EXPECT_DOUBLE_EQ(22.0, late[0].start);
    EXPECT_DOUBLE_EQ(70.0, late[0].end);
}

TEST_F(AutomixVocalGuardTest, ParsesLittleEndianFloat32Pairs) {
    const auto pairs = AutomixVocalGuardPlanner::parseFloat32Pairs(
            float32Blob({22.55f, 26.85f, 28.5f, 36.3f, 99.0f}));
    ASSERT_EQ(2u, pairs.size()); // the trailing odd value is dropped
    EXPECT_FLOAT_EQ(22.55f, static_cast<float>(pairs[0].start));
    EXPECT_FLOAT_EQ(26.85f, static_cast<float>(pairs[0].end));
    EXPECT_FLOAT_EQ(28.5f, static_cast<float>(pairs[1].start));
    EXPECT_FLOAT_EQ(36.3f, static_cast<float>(pairs[1].end));

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const auto clean = AutomixVocalGuardPlanner::parseFloat32Pairs(
            float32Blob({1.0f, nan, 2.0f, 3.0f}));
    ASSERT_EQ(1u, clean.size());
    EXPECT_DOUBLE_EQ(2.0, clean[0].start);
    EXPECT_TRUE(AutomixVocalGuardPlanner::parseFloat32Pairs(QByteArray()).empty());
}

TEST_F(AutomixVocalGuardTest, GridBeatAtUsesTheClockIndexing) {
    const mixxx::BeatsPointer pBeats = grid120();
    ASSERT_TRUE(pBeats);
    const auto beat = AutomixTransitionMath::gridBeatAt(
            *pBeats, mixxx::audio::FramePos(22050 + 3.25 * 22050));
    ASSERT_TRUE(beat.has_value());
    EXPECT_NEAR(3.25, *beat, 1e-9);
    // Before the anchor: negative beats.
    const auto before = AutomixTransitionMath::gridBeatAt(*pBeats, mixxx::audio::FramePos(0));
    ASSERT_TRUE(before.has_value());
    EXPECT_NEAR(-1.0, *before, 1e-9);
    EXPECT_FALSE(AutomixTransitionMath::gridBeatAt(*pBeats, mixxx::audio::FramePos()));
}

TEST_F(AutomixVocalGuardTest, SecondsBecomeBeatsOfTheCurrentGrid) {
    const mixxx::BeatsPointer pBeats = grid120();
    ASSERT_TRUE(pBeats);
    // beat = 2 * sec - 1
    const auto blocks = AutomixVocalGuardPlanner::secondsToGridBeats(
            *pBeats, {{0.5, 2.5}, {0.25, 1.0}, {3.0, 3.0}});
    ASSERT_EQ(2u, blocks.size()); // the empty segment is dropped
    EXPECT_NEAR(0.0, blocks[0].start, 1e-9);
    EXPECT_NEAR(4.0, blocks[0].end, 1e-9);
    EXPECT_NEAR(-0.5, blocks[1].start, 1e-9);
    EXPECT_NEAR(1.0, blocks[1].end, 1e-9);

    // Same audio after a contratimp grid correction (anchor moved by half a
    // beat): the phrases stay on the audio, so their beats move.
    const mixxx::BeatsPointer pCorrected = mixxx::Beats::fromConstTempo(
            mixxx::audio::SampleRate(44100),
            mixxx::audio::FramePos(22050 + 11025),
            mixxx::Bpm(120.0));
    ASSERT_TRUE(pCorrected);
    const auto corrected =
            AutomixVocalGuardPlanner::secondsToGridBeats(*pCorrected, {{0.5, 2.5}});
    ASSERT_EQ(1u, corrected.size());
    EXPECT_NEAR(-0.5, corrected[0].start, 1e-9);
    EXPECT_NEAR(3.5, corrected[0].end, 1e-9);
}

TEST_F(AutomixVocalGuardTest, SimulatorCaseFromSecondsToPlan) {
    // test_guard_removes_most_vocal_overlap_in_the_simulator: 100 BPM grid
    // from 0, transition on beat 32, outgoing voice until beat 38, silent
    // until 44 -> the voice fades at transition beat 6.
    const mixxx::BeatsPointer pBeats = mixxx::Beats::fromConstTempo(
            mixxx::audio::SampleRate(44100), mixxx::audio::FramePos(0), mixxx::Bpm(100.0));
    ASSERT_TRUE(pBeats);
    const double period = 0.6;
    const auto outgoing = AutomixVocalGuardPlanner::toTransitionBeats(
            AutomixVocalGuardPlanner::secondsToGridBeats(*pBeats,
                    {{16 * period, 38 * period}, {44 * period, 70 * period}}),
            32.0,
            0.0);
    const AutomixVocalGuardPlan p = plan(outgoing, {{0, 1000}});
    EXPECT_NEAR(6.0, p.outgoingFadeStart, 1e-6);
    EXPECT_NEAR(8.0, p.incomingFadeStart, 1e-6);
    EXPECT_FALSE(p.cutMidPhrase);
}

TEST_F(AutomixVocalGuardTest, LookupFindsTheOriginalAndItsStemFile) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("brain.db"));
    ASSERT_TRUE(writeBrainDb(dbPath));
    const QByteArray before = fileBytes(dbPath);

    // The original, with backslashes and other case (incl. non-ASCII letters).
    const auto original = AutomixVocalMapStore::lookup(dbPath,
            QStringLiteral("c:\\users\\admin\\downloads\\DJ DANIEL - AȘA EȘTI TU.mp3"));
    ASSERT_TRUE(original.has_value());
    EXPECT_EQ(kOriginal, original->location);
    EXPECT_FALSE(original->viaStemExport);
    EXPECT_TRUE(original->hasVocals);
    ASSERT_EQ(2u, original->segmentsSec.size());
    EXPECT_DOUBLE_EQ(30.5, original->segmentsSec[1].start);
    EXPECT_DOUBLE_EQ(40.25, original->segmentsSec[1].end);

    // The exported .stem.mp4 (what Dan loads in Mixxx 2.6): its original's map.
    const auto stem = AutomixVocalMapStore::lookup(dbPath, kStemFile);
    ASSERT_TRUE(stem.has_value());
    EXPECT_EQ(kOriginal, stem->location);
    EXPECT_TRUE(stem->viaStemExport);
    EXPECT_TRUE(stem->hasVocals);
    EXPECT_EQ(2u, stem->segmentsSec.size());

    // brain.db is opened read-only.
    EXPECT_EQ(before, fileBytes(dbPath));
}

TEST_F(AutomixVocalGuardTest, LookupReportsInstrumentalsAndUnknownTracks) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("brain.db"));
    ASSERT_TRUE(writeBrainDb(dbPath));

    const auto instrumental = AutomixVocalMapStore::lookup(dbPath, kInstrumentalStem);
    ASSERT_TRUE(instrumental.has_value());
    EXPECT_FALSE(instrumental->hasVocals);
    EXPECT_TRUE(instrumental->segmentsSec.empty());

    EXPECT_FALSE(AutomixVocalMapStore::lookup(
            dbPath, QStringLiteral("C:/Users/ADMIN/Downloads/Nu exista.mp3")));
    EXPECT_FALSE(AutomixVocalMapStore::lookup(QString(), kOriginal));

    // A missing brain.db is not created by the lookup.
    const QString missing = dir.filePath(QStringLiteral("missing.db"));
    EXPECT_FALSE(AutomixVocalMapStore::lookup(missing, kOriginal));
    EXPECT_FALSE(QFile::exists(missing));
}
