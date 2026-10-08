#include "mixer/gridcorrector.h"

#include <gtest/gtest.h>

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
