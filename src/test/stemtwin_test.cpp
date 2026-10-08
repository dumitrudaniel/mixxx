#include "mixer/stemtwin.h"

#include <gtest/gtest.h>

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <tuple>

#include "library/trackcollection.h"
#include "test/librarytest.h"
#include "track/cue.h"
#include "track/keyfactory.h"
#include "track/track.h"

// Mirrors brain/tests/test_analysis_stemexport.py's contract for
// stem_exports and docs/decisions/0028 ("opțiunea A").

namespace {

constexpr double kSampleRate = 44100.0;

mixxx::BeatsPointer constGrid(double sampleRate, double anchorFrame, double bpm) {
    return mixxx::Beats::fromConstTempo(mixxx::audio::SampleRate(sampleRate),
            mixxx::audio::FramePos(anchorFrame),
            mixxx::Bpm(bpm),
            QStringLiteral("rounding=V4"));
}

CuePointer hotCue(int index, double startFrame, const QString& label = QString()) {
    CuePointer pCue(new Cue(mixxx::CueType::HotCue,
            index,
            mixxx::audio::FramePos(startFrame),
            mixxx::audio::kInvalidFramePos,
            mixxx::PredefinedColorPalettes::kDefaultCueColor));
    if (!label.isEmpty()) {
        pCue->setLabel(label);
    }
    return pCue;
}

// A brain.db with just the stem_exports columns StemTwinStore reads.
bool writeBrainDb(const QString& path,
        const QList<std::tuple<QString, QString, double, double>>& rows) {
    const QString connection = QStringLiteral("stemtwin_test_writer");
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(path);
        if (db.open()) {
            QSqlQuery query(db);
            ok = query.exec(QStringLiteral(
                    "CREATE TABLE stem_exports (source_location TEXT PRIMARY KEY, "
                    "stem_path TEXT, sample_rate INTEGER, frames INTEGER)"));
            for (const auto& [source, stem, sampleRate, frames] : rows) {
                if (!ok) {
                    break;
                }
                query.prepare(QStringLiteral(
                        "INSERT INTO stem_exports (source_location, stem_path, "
                        "sample_rate, frames) VALUES (?, ?, ?, ?)"));
                query.addBindValue(source);
                query.addBindValue(stem);
                query.addBindValue(sampleRate);
                query.addBindValue(frames);
                ok = query.exec();
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return ok;
}

} // namespace

// ---------------------------------------------------------------- pure logic

TEST(StemTwinExportCurrencyTest, MatchingSampleRateAndLengthIsCurrent) {
    EXPECT_TRUE(StemTwinController::exportIsCurrent(44100.0, 10.0, 44100.0, 441000.0));
    // A fraction-of-a-frame rounding error from Track::getDuration() still passes.
    EXPECT_TRUE(StemTwinController::exportIsCurrent(44100.0, 10.0, 44100.0, 441001.3));
}

TEST(StemTwinExportCurrencyTest, SampleRateMismatchIsStale) {
    EXPECT_FALSE(StemTwinController::exportIsCurrent(44100.0, 10.0, 48000.0, 480000.0));
}

TEST(StemTwinExportCurrencyTest, LengthMismatchBeyondToleranceIsStale) {
    // Off by 2 seconds' worth of frames (a different export entirely).
    EXPECT_FALSE(StemTwinController::exportIsCurrent(44100.0, 10.0, 44100.0, 529200.0));
}

TEST(StemTwinExportCurrencyTest, MissingAudioPropertiesAreStale) {
    EXPECT_FALSE(StemTwinController::exportIsCurrent(0.0, 10.0, 44100.0, 441000.0));
    EXPECT_FALSE(StemTwinController::exportIsCurrent(44100.0, 0.0, 44100.0, 441000.0));
}

TEST(StemTwinLocationTest, DetectsStemFilesByExtension) {
    EXPECT_TRUE(StemTwinController::isStemFileLocation(QStringLiteral("C:/x/Song.stem.mp4")));
    EXPECT_TRUE(StemTwinController::isStemFileLocation(QStringLiteral("C:/x/Song.STEM.M4A")));
    EXPECT_FALSE(StemTwinController::isStemFileLocation(QStringLiteral("C:/x/Song.mp3")));
}

// -------------------------------------------------------- copy, no aliasing

TEST(StemTwinCopyTest, CopiesGridKeyReplayGainAndClonedCues) {
    TrackPointer pOriginal(Track::newTemporary());
    pOriginal->setAudioProperties(mixxx::audio::ChannelCount(2),
            mixxx::audio::SampleRate(kSampleRate),
            mixxx::audio::Bitrate(),
            mixxx::Duration::fromSeconds(10));
    const mixxx::BeatsPointer pBeats = constGrid(kSampleRate, 441.0, 120.0);
    pOriginal->trySetBeats(pBeats);
    pOriginal->setKeys(KeyFactory::makeBasicKeys(
            mixxx::track::io::key::A_MINOR, mixxx::track::io::key::Source::USER));
    pOriginal->setReplayGain(mixxx::ReplayGain(0.5, mixxx::ReplayGain::kPeakUndefined));
    pOriginal->setCuePoints({hotCue(0, 4410.0, QStringLiteral("drop"))});

    TrackPointer pTwin(Track::newTemporary());
    StemTwinController::copyOntoTwin(*pOriginal, pTwin.get());

    EXPECT_EQ(pOriginal->getBeats(), pTwin->getBeats());
    EXPECT_EQ(pOriginal->getKeys().getGlobalKey(), pTwin->getKeys().getGlobalKey());
    EXPECT_DOUBLE_EQ(pOriginal->getReplayGain().getRatio(), pTwin->getReplayGain().getRatio());
    ASSERT_EQ(1, pTwin->getCuePoints().size());
    EXPECT_EQ(QStringLiteral("drop"), pTwin->getCuePoints().front()->getLabel());

    // Not aliased: a cue added on the twin never touches the original's list.
    QList<CuePointer> twinCues = pTwin->getCuePoints();
    twinCues.append(hotCue(1, 8820.0, QStringLiteral("breakdown")));
    pTwin->setCuePoints(twinCues);
    EXPECT_EQ(1, pOriginal->getCuePoints().size());
    EXPECT_EQ(2, pTwin->getCuePoints().size());
}

// ------------------------------------------------------------- brain.db read

class StemTwinStoreTest : public testing::Test {
  protected:
    QTemporaryDir m_dir;
};

TEST_F(StemTwinStoreTest, FindsTheRowByNormalizedLocation) {
    const QString dbPath = m_dir.filePath(QStringLiteral("brain.db"));
    ASSERT_TRUE(writeBrainDb(dbPath,
            {{QStringLiteral("C:/Música/Raïssa.mp3"),
                    QStringLiteral("D:/stems/Raissa.stem.mp4"),
                    44100.0,
                    441000.0}}));
    // Backslashes and case differ, like a Windows path typed differently.
    const auto info = StemTwinStore::lookup(
            dbPath, QStringLiteral("c:\\MÚSICA\\raïssa.mp3"));
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(QStringLiteral("D:/stems/Raissa.stem.mp4"), info->stemPath);
    EXPECT_DOUBLE_EQ(44100.0, info->sampleRate);
    EXPECT_DOUBLE_EQ(441000.0, info->frames);
}

TEST_F(StemTwinStoreTest, NoRowNoDbOrEmptyPathAllReturnNullopt) {
    const QString dbPath = m_dir.filePath(QStringLiteral("brain.db"));
    ASSERT_TRUE(writeBrainDb(dbPath, {{QStringLiteral("C:/a.mp3"), QStringLiteral("C:/a.stem.mp4"),
            44100.0, 100.0}}));
    EXPECT_FALSE(StemTwinStore::lookup(dbPath, QStringLiteral("C:/other.mp3")).has_value());
    EXPECT_FALSE(StemTwinStore::lookup(QString(), QStringLiteral("C:/a.mp3")).has_value());
    EXPECT_FALSE(StemTwinStore::lookup(m_dir.filePath(QStringLiteral("missing.db")),
            QStringLiteral("C:/a.mp3"))
                         .has_value());
}

// ---------------------------------------------------------- full load cycle

class StemTwinControllerTest : public LibraryTest {
  protected:
    QString originalPath() const {
        return getTestDir().filePath(
                QStringLiteral("id3-test-data/cover-test-øé~ł€˚-png.mp3"));
    }
    QString twinPath() const {
        return getTestDir().filePath(
                QStringLiteral("stems/stem01/sin_AAC_256kbps_VBR.stem.mp4"));
    }

    // Known, test-controlled audio properties (independent of whatever the
    // fixture files' own tags/decode would give).
    TrackPointer loadOriginal() {
        TrackPointer pTrack = getOrAddTrackByLocation(originalPath());
        pTrack->setAudioProperties(mixxx::audio::ChannelCount(2),
                mixxx::audio::SampleRate(kSampleRate),
                mixxx::audio::Bitrate(),
                mixxx::Duration::fromSeconds(10));
        pTrack->trySetBeats(constGrid(kSampleRate, 441.0, 120.0));
        pTrack->setKeys(KeyFactory::makeBasicKeys(
                mixxx::track::io::key::A_MINOR, mixxx::track::io::key::Source::USER));
        pTrack->setReplayGain(mixxx::ReplayGain(0.5, mixxx::ReplayGain::kPeakUndefined));
        pTrack->setCuePoints({hotCue(0, 4410.0, QStringLiteral("drop"))});
        return pTrack;
    }

    bool isHidden(const TrackPointer& pTrack) const {
        QSqlQuery query(internalCollection()->database());
        query.prepare(QStringLiteral("SELECT mixxx_deleted FROM library WHERE id = ?"));
        query.addBindValue(pTrack->getId().toVariant());
        EXPECT_TRUE(query.exec()) << query.lastError().text().toStdString();
        EXPECT_TRUE(query.next());
        return query.value(0).toBool();
    }

    QTemporaryDir m_dir;
};

TEST_F(StemTwinControllerTest, SubstitutesACurrentTwinAndHidesIt) {
    const TrackPointer pOriginal = loadOriginal();
    const QString dbPath = m_dir.filePath(QStringLiteral("brain.db"));
    ASSERT_TRUE(writeBrainDb(dbPath,
            {{pOriginal->getLocation(), twinPath(), kSampleRate, kSampleRate * 10.0}}));
    config()->setValue(ConfigKey("[DJApp]", "BrainDb"), dbPath);
    config()->setValue(ConfigKey("[DJApp]", "StemTwins"), true);

    StemTwinController controller(config(), nullptr);
    controller.setTrackCollectionManager(trackCollectionManager());

    const TrackPointer pLoaded = controller.substituteForLoad(
            pOriginal, QStringLiteral("[Channel1]"));
    ASSERT_NE(nullptr, pLoaded);
    EXPECT_NE(pOriginal, pLoaded);
    EXPECT_EQ(QFileInfo(twinPath()).absoluteFilePath(),
            QFileInfo(pLoaded->getLocation()).absoluteFilePath());
    EXPECT_TRUE(isHidden(pLoaded));

    // One-way copy landed on the twin.
    EXPECT_EQ(pOriginal->getBeats(), pLoaded->getBeats());
    EXPECT_EQ(pOriginal->getKeys().getGlobalKey(), pLoaded->getKeys().getGlobalKey());
    ASSERT_EQ(1, pLoaded->getCuePoints().size());
    EXPECT_EQ(QStringLiteral("drop"), pLoaded->getCuePoints().front()->getLabel());
}

TEST_F(StemTwinControllerTest, HotcuesSetOnTheTwinComeBackToTheOriginalOnRelease) {
    const TrackPointer pOriginal = loadOriginal();
    const QString dbPath = m_dir.filePath(QStringLiteral("brain.db"));
    ASSERT_TRUE(writeBrainDb(dbPath,
            {{pOriginal->getLocation(), twinPath(), kSampleRate, kSampleRate * 10.0}}));
    config()->setValue(ConfigKey("[DJApp]", "BrainDb"), dbPath);
    config()->setValue(ConfigKey("[DJApp]", "StemTwins"), true);

    StemTwinController controller(config(), nullptr);
    controller.setTrackCollectionManager(trackCollectionManager());
    const TrackPointer pTwin = controller.substituteForLoad(
            pOriginal, QStringLiteral("[Channel1]"));
    ASSERT_NE(nullptr, pTwin);

    // Dan drops a new hotcue while the twin is on the deck.
    QList<CuePointer> cues = pTwin->getCuePoints();
    cues.append(hotCue(1, 88200.0, QStringLiteral("breakdown")));
    pTwin->setCuePoints(cues);

    // Any later load on the same group (here: nothing, group going empty)
    // releases the mapping and copies the twin's cues back.
    controller.substituteForLoad(TrackPointer(), QStringLiteral("[Channel1]"));

    ASSERT_EQ(2, pOriginal->getCuePoints().size());
    bool foundBreakdown = false;
    for (const CuePointer& pCue : pOriginal->getCuePoints()) {
        foundBreakdown |= pCue->getLabel() == QStringLiteral("breakdown");
    }
    EXPECT_TRUE(foundBreakdown);
}

TEST_F(StemTwinControllerTest, StaleExportFallsBackToTheOriginal) {
    const TrackPointer pOriginal = loadOriginal();
    const QString dbPath = m_dir.filePath(QStringLiteral("brain.db"));
    // Wrong sample rate: the re-export is stale.
    ASSERT_TRUE(writeBrainDb(
            dbPath, {{pOriginal->getLocation(), twinPath(), 48000.0, 48000.0 * 10.0}}));
    config()->setValue(ConfigKey("[DJApp]", "BrainDb"), dbPath);
    config()->setValue(ConfigKey("[DJApp]", "StemTwins"), true);

    StemTwinController controller(config(), nullptr);
    controller.setTrackCollectionManager(trackCollectionManager());
    const TrackPointer pLoaded = controller.substituteForLoad(
            pOriginal, QStringLiteral("[Channel1]"));
    EXPECT_EQ(pOriginal, pLoaded);
}

TEST_F(StemTwinControllerTest, NoExportRowPlaysTheOriginal) {
    const TrackPointer pOriginal = loadOriginal();
    const QString dbPath = m_dir.filePath(QStringLiteral("brain.db"));
    ASSERT_TRUE(writeBrainDb(dbPath, {}));
    config()->setValue(ConfigKey("[DJApp]", "BrainDb"), dbPath);
    config()->setValue(ConfigKey("[DJApp]", "StemTwins"), true);

    StemTwinController controller(config(), nullptr);
    controller.setTrackCollectionManager(trackCollectionManager());
    EXPECT_EQ(pOriginal,
            controller.substituteForLoad(pOriginal, QStringLiteral("[Channel1]")));
}

TEST_F(StemTwinControllerTest, ToggleOffPlaysTheOriginalEvenWithACurrentTwin) {
    const TrackPointer pOriginal = loadOriginal();
    const QString dbPath = m_dir.filePath(QStringLiteral("brain.db"));
    ASSERT_TRUE(writeBrainDb(dbPath,
            {{pOriginal->getLocation(), twinPath(), kSampleRate, kSampleRate * 10.0}}));
    config()->setValue(ConfigKey("[DJApp]", "BrainDb"), dbPath);
    config()->setValue(ConfigKey("[DJApp]", "StemTwins"), false);

    StemTwinController controller(config(), nullptr);
    controller.setTrackCollectionManager(trackCollectionManager());
    EXPECT_EQ(pOriginal,
            controller.substituteForLoad(pOriginal, QStringLiteral("[Channel1]")));
}

TEST_F(StemTwinControllerTest, LoadingAStemFileDirectlyNeverLooksForItsOwnTwin) {
    const TrackPointer pStemFile = getOrAddTrackByLocation(twinPath());
    const QString dbPath = m_dir.filePath(QStringLiteral("brain.db"));
    // Even a (nonsensical) row keyed on the stem file itself must be ignored.
    ASSERT_TRUE(writeBrainDb(
            dbPath, {{pStemFile->getLocation(), twinPath(), kSampleRate, 1.0}}));
    config()->setValue(ConfigKey("[DJApp]", "BrainDb"), dbPath);
    config()->setValue(ConfigKey("[DJApp]", "StemTwins"), true);

    StemTwinController controller(config(), nullptr);
    controller.setTrackCollectionManager(trackCollectionManager());
    EXPECT_EQ(pStemFile,
            controller.substituteForLoad(pStemFile, QStringLiteral("[Channel1]")));
}
