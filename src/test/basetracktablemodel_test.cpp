#include "library/basetracktablemodel.h"

#include <gtest/gtest.h>

#include "track/keyutils.h"

// Unit tests for BaseTrackTableModel::formatDualNotationKeyText(), the pure
// helper extracted from BaseTrackTableModel::roleValue()'s
// Qt::DisplayRole / COLUMN_LIBRARYTABLE_KEY branch (see
// docs/decisions/0004-dual-notation-key-column.md and
// docs/decisions/0007-*.md). Testing roleValue() itself would require a
// fully constructed BaseTrackTableModel backed by a real/mock database
// connection; the actual string formatting logic has no such dependency,
// so it was extracted into this static function purely to make it
// testable in isolation.
class BaseTrackTableModelTest : public testing::Test {};

TEST_F(BaseTrackTableModelTest, FormatDualNotationKeyText_InvalidKeyReturnsEmpty) {
    EXPECT_TRUE(BaseTrackTableModel::formatDualNotationKeyText(
            mixxx::track::io::key::INVALID)
                        .isEmpty());
}

TEST_F(BaseTrackTableModelTest, FormatDualNotationKeyText_CombinesCamelotAndTraditional) {
    // D_MINOR == "7A" in Lancelot/Camelot notation (see keyutilstest.cpp),
    // "Dm" in traditional notation.
    EXPECT_EQ(QStringLiteral("7A · Dm"),
            BaseTrackTableModel::formatDualNotationKeyText(
                    mixxx::track::io::key::D_MINOR));
}

TEST_F(BaseTrackTableModelTest, FormatDualNotationKeyText_MajorKeyExample) {
    // C_SHARP_MINOR == "12A"; its relative major D_FLAT_MAJOR == "12B",
    // traditional "D♭".
    EXPECT_EQ(QStringLiteral("12A · C♯m"),
            BaseTrackTableModel::formatDualNotationKeyText(
                    mixxx::track::io::key::C_SHARP_MINOR));
}

TEST_F(BaseTrackTableModelTest, FormatDualNotationKeyText_CamelotComesFirst) {
    // Per the ADR: Camelot notation is shown first (Dan is most familiar
    // with it), separated from the traditional notation by " · ".
    const QString result = BaseTrackTableModel::formatDualNotationKeyText(
            mixxx::track::io::key::A_MINOR);
    EXPECT_TRUE(result.startsWith(QStringLiteral("8A")));
    EXPECT_TRUE(result.contains(QStringLiteral(" · ")));
    EXPECT_TRUE(result.endsWith(QStringLiteral("Am")));
}
