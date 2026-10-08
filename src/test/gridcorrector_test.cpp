#include "mixer/gridcorrector.h"

#include <gtest/gtest.h>

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <cmath>

#include "track/beats.h"
#include "track/track.h"

namespace {

GridCorrector::Correction kakile() {
    // Grila Mixxx a piesei Kakile: 105 BPM, ancora la cadrul 10551 @ 44.1 kHz,
    // pe contratimpi -> mutata cu 0.49 timp (ADR 0018).
    GridCorrector::Correction c;
    c.originalBpm = 105.0;
    c.originalAnchorFrame = 10551.0;
    c.sampleRate = 44100.0;
    c.correctionBeats = 0.49;
    c.reason = QStringLiteral("contratimp");
    return c;
}

// ADR 0025: Miroir era in Mixxx la 170 BPM (ancora 10059 @ 48 kHz), piesa e la
// 85. Randul tine grila tinta completa: 85 BPM, plus contratimpul decis de
// consens pe grila de 85 (0.5 timp).
constexpr double kMiroirTargetAnchor = 10059.0 + 0.5 * 60.0 / 85.0 * 48000.0;

GridCorrector::Correction miroir() {
    GridCorrector::Correction c;
    c.originalBpm = 170.0;
    c.originalAnchorFrame = 10059.0;
    c.sampleRate = 48000.0;
    c.correctionBeats = 0.5;
    c.reason = QStringLiteral("octava x1/2+contratimp");
    c.kind = QStringLiteral("tempo+phase");
    c.correctedBpm = 85.0;
    c.correctedAnchorFrame = kMiroirTargetAnchor;
    return c;
}

mixxx::BeatsPointer constGrid(double sampleRate, double anchorFrame, double bpm) {
    return mixxx::Beats::fromConstTempo(mixxx::audio::SampleRate(sampleRate),
            mixxx::audio::FramePos(anchorFrame),
            mixxx::Bpm(bpm),
            QStringLiteral("rounding=V4|vamp_plugin_id=qm-tempotracker:0"));
}

TrackPointer newTrack(double sampleRate, const mixxx::BeatsPointer& pBeats) {
    TrackPointer pTrack(Track::newTemporary());
    pTrack->setAudioProperties(mixxx::audio::ChannelCount(2),
            mixxx::audio::SampleRate(sampleRate),
            mixxx::audio::Bitrate(),
            mixxx::Duration::fromSeconds(240));
    pTrack->trySetBeats(pBeats);
    return pTrack;
}

// A brain.db with grid_corrections, either with the ADR 0025 columns or as
// written before them (no kind / corrected_*).
bool writeBrainDb(const QString& path, bool withTempoColumns) {
    const QString connection = QStringLiteral("gridcorrector_test_writer");
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(path);
        if (db.open()) {
            QSqlQuery query(db);
            const QString tempoColumns = withTempoColumns
                    ? QStringLiteral(
                              ", kind TEXT NOT NULL DEFAULT 'phase', corrected_bpm REAL, "
                              "corrected_first_beat_frame REAL")
                    : QString();
            ok = query.exec(QStringLiteral(
                    "CREATE TABLE grid_corrections (location TEXT PRIMARY KEY, "
                    "original_bpm REAL, original_first_beat_frame REAL, sample_rate "
                    "INTEGER, correction_beats REAL, reason TEXT, apply INTEGER%1)")
                                 .arg(tempoColumns));
            const auto add = [&](const QString& location,
                                     double bpm,
                                     double frame,
                                     double beats,
                                     int apply,
                                     const QString& kind,
                                     double correctedBpm,
                                     double correctedFrame) {
                if (withTempoColumns) {
                    query.prepare(QStringLiteral(
                            "INSERT INTO grid_corrections VALUES (?, ?, ?, 48000, ?, "
                            "'r', ?, ?, ?, ?)"));
                } else {
                    query.prepare(QStringLiteral(
                            "INSERT INTO grid_corrections VALUES (?, ?, ?, 48000, ?, "
                            "'r', ?)"));
                }
                query.addBindValue(location);
                query.addBindValue(bpm);
                query.addBindValue(frame);
                query.addBindValue(beats);
                query.addBindValue(apply);
                if (withTempoColumns) {
                    query.addBindValue(kind);
                    query.addBindValue(correctedBpm);
                    query.addBindValue(correctedFrame);
                }
                return query.exec();
            };
            ok = ok &&
                    add(QStringLiteral("C:/Users/ADMIN/Downloads/Miroir - DJ Inno.mp3"),
                            170.0,
                            10059.0,
                            0.5,
                            1,
                            QStringLiteral("tempo+phase"),
                            85.0,
                            kMiroirTargetAnchor) &&
                    add(QStringLiteral("C:/Users/ADMIN/Downloads/Kakile.mp3"),
                            105.0,
                            10551.0,
                            0.49,
                            1,
                            QStringLiteral("phase"),
                            105.0,
                            10551.0 + 0.49 * 60.0 / 105.0 * 48000.0) &&
                    add(QStringLiteral("C:/Users/ADMIN/Downloads/Santimantal.mp3"),
                            100.917,
                            11488.0,
                            0.0,
                            0,
                            QStringLiteral("phase"),
                            100.917,
                            11488.0);
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return ok;
}

} // namespace

class GridCorrectorTest : public testing::Test {
};

TEST_F(GridCorrectorTest, OffsetIsCorrectionTimesBeatLength) {
    const auto offset = GridCorrector::offsetFrames(105.0, 10551.0, 44100.0, kakile());
    ASSERT_TRUE(offset.has_value());
    EXPECT_DOUBLE_EQ(0.49 * 60.0 / 105.0 * 44100.0, *offset);
}

TEST_F(GridCorrectorTest, AppliesOnlyToTheAnalyzedGrid) {
    // Already corrected (anchor moved) -> not applied twice.
    EXPECT_FALSE(GridCorrector::offsetFrames(105.0, 10551.0 + 12348.0, 44100.0, kakile()));
    // Dan changed the tempo by hand -> left alone.
    EXPECT_FALSE(GridCorrector::offsetFrames(105.2, 10551.0, 44100.0, kakile()));
    // Different sample rate than the one the frames refer to.
    EXPECT_FALSE(GridCorrector::offsetFrames(105.0, 10551.0, 48000.0, kakile()));
    // Within rounding tolerance it still applies.
    EXPECT_TRUE(GridCorrector::offsetFrames(105.00001, 10551.6, 44100.0, kakile()));
}

TEST_F(GridCorrectorTest, ParityCorrectionMovesOneWholeBeat) {
    GridCorrector::Correction c = kakile();
    c.originalBpm = 108.0;
    c.originalAnchorFrame = 9922.0;
    c.correctionBeats = 1.0;
    const auto offset = GridCorrector::offsetFrames(108.0, 9922.0, 44100.0, c);
    ASSERT_TRUE(offset.has_value());
    EXPECT_DOUBLE_EQ(60.0 / 108.0 * 44100.0, *offset);
}

TEST_F(GridCorrectorTest, ZeroCorrectionDoesNothing) {
    GridCorrector::Correction c = kakile();
    c.correctionBeats = 0.0;
    EXPECT_FALSE(GridCorrector::offsetFrames(105.0, 10551.0, 44100.0, c));
}

TEST_F(GridCorrectorTest, LocationsMatchAcrossSlashesAndCase) {
    EXPECT_EQ(GridCorrector::normalizedLocation(
                      QStringLiteral("C:/Users/ADMIN/Downloads/Água fresca.mp3")),
            GridCorrector::normalizedLocation(
                    QStringLiteral("c:\\users\\admin\\downloads\\água FRESCA.mp3")));
}

// ---------------------------------------------------------------- ADR 0025: tempo

TEST_F(GridCorrectorTest, PhaseRowTranslatesTheGrid) {
    // Kind "phase" and an empty kind (brain.db from before ADR 0025) behave alike.
    for (const QString& kind : {QString(), QStringLiteral("phase")}) {
        GridCorrector::Correction c = kakile();
        c.kind = kind;
        const auto pBeats = GridCorrector::correctedBeats(constGrid(44100.0, 10551.0, 105.0), c);
        ASSERT_TRUE(pBeats);
        EXPECT_DOUBLE_EQ(105.0, pBeats->getLastMarkerBpm().value());
        EXPECT_DOUBLE_EQ(std::floor(10551.0 + 0.49 * 60.0 / 105.0 * 44100.0),
                pBeats->getLastMarkerPosition().value());
    }
}

TEST_F(GridCorrectorTest, TempoRowSetsTheFullTargetGrid) {
    const auto pBeats =
            GridCorrector::correctedBeats(constGrid(48000.0, 10059.0, 170.0), miroir());
    ASSERT_TRUE(pBeats);
    EXPECT_TRUE(pBeats->hasConstantTempo());
    EXPECT_DOUBLE_EQ(85.0, pBeats->getLastMarkerBpm().value());
    EXPECT_DOUBLE_EQ(kMiroirTargetAnchor, pBeats->getLastMarkerPosition().value());
    EXPECT_EQ(48000.0, pBeats->getSampleRate().toDouble());
    // Kept like a manual edit, so Mixxx does not re-analyze the track.
    EXPECT_EQ(QStringLiteral("rounding=V4|vamp_plugin_id=qm-tempotracker:0"),
            pBeats->getSubVersion());
}

TEST_F(GridCorrectorTest, SmallTempoCorrectionSnapsToTheRoundTempo) {
    // Un tempo constant usor gresit (109.917 -> 110.00): tot grila tinta completa.
    GridCorrector::Correction c;
    c.originalBpm = 109.91666666666667;
    c.originalAnchorFrame = 13708.0;
    c.sampleRate = 48000.0;
    c.kind = QStringLiteral("tempo");
    c.reason = QStringLiteral("tempo");
    c.correctedBpm = 110.0;
    c.correctedAnchorFrame = 13702.25;
    const auto pBeats =
            GridCorrector::correctedBeats(constGrid(48000.0, 13708.0, 109.91666666666667), c);
    ASSERT_TRUE(pBeats);
    EXPECT_DOUBLE_EQ(110.0, pBeats->getLastMarkerBpm().value());
    EXPECT_DOUBLE_EQ(13702.25, pBeats->getLastMarkerPosition().value());
}

TEST_F(GridCorrectorTest, TempoRowIsAppliedOnlyOnce) {
    const auto pOnce = GridCorrector::correctedBeats(constGrid(48000.0, 10059.0, 170.0), miroir());
    ASSERT_TRUE(pOnce);
    // The corrected grid no longer matches the analyzed original.
    EXPECT_FALSE(GridCorrector::correctedBeats(pOnce, miroir()));
    // Nor does a grid Dan edited by hand (other tempo or anchor).
    EXPECT_FALSE(GridCorrector::correctedBeats(constGrid(48000.0, 10059.0, 171.0), miroir()));
    EXPECT_FALSE(GridCorrector::correctedBeats(constGrid(48000.0, 10100.0, 170.0), miroir()));
    EXPECT_FALSE(GridCorrector::correctedBeats(constGrid(44100.0, 10059.0, 170.0), miroir()));
}

TEST_F(GridCorrectorTest, TempoRowWithoutTargetIsSkipped) {
    GridCorrector::Correction c = miroir();
    c.correctedBpm = 0.0;
    EXPECT_FALSE(c.correctsTempo());
    // Not silently applied as a phase shift of the 170 BPM grid either.
    EXPECT_FALSE(GridCorrector::correctedBeats(constGrid(48000.0, 10059.0, 170.0), c));
    c = miroir();
    c.correctedAnchorFrame = std::nan("");
    EXPECT_FALSE(GridCorrector::correctedBeats(constGrid(48000.0, 10059.0, 170.0), c));
}

TEST_F(GridCorrectorTest, VariableGridIsNeverTouched) {
    QVector<mixxx::audio::FramePos> positions;
    double frame = 10059.0;
    for (int i = 0; i < 64; ++i) {
        positions.append(mixxx::audio::FramePos(frame));
        frame += (i < 32 ? 16941.0 : 16800.0); // two tempos -> a marker
    }
    const auto pMap = mixxx::Beats::fromBeatPositions(
            mixxx::audio::SampleRate(48000), positions);
    ASSERT_TRUE(pMap);
    ASSERT_FALSE(pMap->hasConstantTempo());
    EXPECT_FALSE(GridCorrector::correctedBeats(pMap, miroir()));
    EXPECT_FALSE(GridCorrector::correctedBeats(pMap, kakile()));
}

TEST_F(GridCorrectorTest, AppliesToTheTrackUnlessTheBpmIsLocked) {
    TrackPointer pTrack = newTrack(48000.0, constGrid(48000.0, 10059.0, 170.0));
    pTrack->setBpmLocked(true);
    EXPECT_FALSE(GridCorrector::applyTo(pTrack, miroir()));
    EXPECT_DOUBLE_EQ(170.0, pTrack->getBeats()->getLastMarkerBpm().value());

    pTrack->setBpmLocked(false);
    EXPECT_TRUE(GridCorrector::applyTo(pTrack, miroir()));
    EXPECT_DOUBLE_EQ(85.0, pTrack->getBeats()->getLastMarkerBpm().value());
    EXPECT_NEAR(85.0, pTrack->getBpm(), 1e-6);
    // Loading the track again does nothing (idempotent).
    EXPECT_FALSE(GridCorrector::applyTo(pTrack, miroir()));
    EXPECT_DOUBLE_EQ(kMiroirTargetAnchor, pTrack->getBeats()->getLastMarkerPosition().value());
}

TEST_F(GridCorrectorTest, LookupReadsTheTargetGrid) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("brain.db"));
    ASSERT_TRUE(writeBrainDb(path, true));

    const auto tempo = GridCorrector::lookup(
            path, QStringLiteral("c:\\users\\admin\\downloads\\MIROIR - dj inno.mp3"));
    ASSERT_TRUE(tempo.has_value());
    EXPECT_EQ(QStringLiteral("tempo+phase"), tempo->kind);
    EXPECT_TRUE(tempo->correctsTempo());
    EXPECT_DOUBLE_EQ(170.0, tempo->originalBpm);
    EXPECT_DOUBLE_EQ(85.0, tempo->correctedBpm);
    EXPECT_DOUBLE_EQ(kMiroirTargetAnchor, tempo->correctedAnchorFrame);

    const auto phase =
            GridCorrector::lookup(path, QStringLiteral("C:/Users/ADMIN/Downloads/Kakile.mp3"));
    ASSERT_TRUE(phase.has_value());
    EXPECT_FALSE(phase->correctsTempo());
    EXPECT_DOUBLE_EQ(0.49, phase->correctionBeats);

    // apply = 0 rows are not read.
    EXPECT_FALSE(GridCorrector::lookup(
            path, QStringLiteral("C:/Users/ADMIN/Downloads/Santimantal.mp3")));
}

TEST_F(GridCorrectorTest, LookupReadsABrainDbFromBeforeTempoCorrections) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("brain.db"));
    ASSERT_TRUE(writeBrainDb(path, false));
    const auto kakileRow =
            GridCorrector::lookup(path, QStringLiteral("C:/Users/ADMIN/Downloads/Kakile.mp3"));
    ASSERT_TRUE(kakileRow.has_value());
    EXPECT_TRUE(kakileRow->kind.isEmpty());
    EXPECT_FALSE(kakileRow->correctsTempo());
    EXPECT_DOUBLE_EQ(0.49, kakileRow->correctionBeats);
    EXPECT_FALSE(GridCorrector::lookup(QString(), QStringLiteral("C:/x.mp3")));
}
