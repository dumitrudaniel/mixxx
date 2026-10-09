#include "library/djapp/djappoverviewmarkers.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <cstring>

// DJ App (docs/decisions/0032): pure parsing/positioning logic for the
// WOverview brain-markers feature. The DB read (fetchOverviewMarkers)
// mirrors the already-covered DJAppAutopilot::readMixOutSec pattern and is
// exercised live, not here -- only the pure functions are unit tested, same
// split as djappautopilot_test.cpp (logic) vs djappautopilot.cpp (driver).

using namespace djapp::overview;

namespace {

QByteArray packFloats(const QVector<float>& values) {
    QByteArray blob;
    blob.resize(static_cast<int>(values.size() * sizeof(float)));
    std::memcpy(blob.data(), values.constData(), static_cast<size_t>(blob.size()));
    return blob;
}

} // namespace

TEST(DJAppOverviewMarkersTest, EmptyBlobGivesEmptySegments) {
    EXPECT_TRUE(parseVocalSegmentsSec(QByteArray()).isEmpty());
}

TEST(DJAppOverviewMarkersTest, ParsesPairsOfFloats) {
    const QByteArray blob = packFloats({1.5f, 3.25f, 10.0f, 12.0f});
    const QVector<VocalSegment> segments = parseVocalSegmentsSec(blob);
    ASSERT_EQ(segments.size(), 2);
    EXPECT_FLOAT_EQ(static_cast<float>(segments[0].startSec), 1.5f);
    EXPECT_FLOAT_EQ(static_cast<float>(segments[0].endSec), 3.25f);
    EXPECT_FLOAT_EQ(static_cast<float>(segments[1].startSec), 10.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(segments[1].endSec), 12.0f);
}

TEST(DJAppOverviewMarkersTest, DropsTrailingUnpairedFloat) {
    const QByteArray blob = packFloats({1.0f, 2.0f, 3.0f});
    EXPECT_EQ(parseVocalSegmentsSec(blob).size(), 1);
}

TEST(DJAppOverviewMarkersTest, TooShortBlobGivesEmptySegments) {
    const QByteArray blob(2, '\0'); // smaller than one float
    EXPECT_TRUE(parseVocalSegmentsSec(blob).isEmpty());
}

TEST(DJAppOverviewMarkersTest, DedupSortsAscending) {
    QMap<int, double> byLength;
    byLength.insert(32, 50.0);
    byLength.insert(8, 10.0);
    byLength.insert(16, 25.0);
    const QVector<double> values = dedupedMixOutValues(byLength);
    ASSERT_EQ(values.size(), 3);
    EXPECT_DOUBLE_EQ(values[0], 10.0);
    EXPECT_DOUBLE_EQ(values[1], 25.0);
    EXPECT_DOUBLE_EQ(values[2], 50.0);
}

TEST(DJAppOverviewMarkersTest, DedupCollapsesNearDuplicates) {
    QMap<int, double> byLength;
    byLength.insert(8, 10.0);
    byLength.insert(16, 10.02); // within 0.05s of the previous value
    byLength.insert(32, 40.0);
    const QVector<double> values = dedupedMixOutValues(byLength);
    ASSERT_EQ(values.size(), 2);
    EXPECT_DOUBLE_EQ(values[0], 10.0);
    EXPECT_DOUBLE_EQ(values[1], 40.0);
}

TEST(DJAppOverviewMarkersTest, DedupOfEmptyMapIsEmpty) {
    EXPECT_TRUE(dedupedMixOutValues(QMap<int, double>()).isEmpty());
}

TEST(DJAppOverviewMarkersTest, FetchWithEmptyDbPathReturnsInvalidMarkers) {
    const BrainMarkers markers = fetchOverviewMarkers(QString(), QStringLiteral("C:/x.mp3"));
    EXPECT_FALSE(markers.valid);
    EXPECT_TRUE(markers.mixOutSec.isEmpty());
    EXPECT_TRUE(markers.vocalSegments.isEmpty());
    EXPECT_TRUE(markers.sections.isEmpty());
}

TEST(DJAppOverviewMarkersTest, FetchWithEmptyLocationReturnsInvalidMarkers) {
    const BrainMarkers markers = fetchOverviewMarkers(QStringLiteral("C:/brain.db"), QString());
    EXPECT_FALSE(markers.valid);
}

TEST(DJAppOverviewMarkersTest, FetchWithMissingDbFileDegradesGracefully) {
    // A path that cannot possibly exist: fetchOverviewMarkers must not
    // crash or throw, only return a default (invalid) result -- same
    // graceful-degradation contract as DJAppAutopilot::readMixOutSec.
    const BrainMarkers markers = fetchOverviewMarkers(
            QStringLiteral("Z:/does/not/exist/brain.db"), QStringLiteral("C:/x.mp3"));
    EXPECT_FALSE(markers.valid);
    EXPECT_TRUE(markers.sections.isEmpty());
}
