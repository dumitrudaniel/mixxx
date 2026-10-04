#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <QColor>
#include <QDebug>
#include <QSet>

#include "proto/keys.pb.h"
#include "track/keyutils.h"

using ::testing::UnorderedElementsAre;

class KeyUtilsTest : public testing::Test {
};

TEST_F(KeyUtilsTest, OpenKeyNotation) {
    // Lower-case.
    EXPECT_EQ(mixxx::track::io::key::B_MAJOR,
              KeyUtils::guessKeyFromText("6d"));
    EXPECT_EQ(mixxx::track::io::key::B_MINOR,
              KeyUtils::guessKeyFromText("3m"));

    // whitespace is ok
    EXPECT_EQ(mixxx::track::io::key::B_MINOR,
              KeyUtils::guessKeyFromText(" 3m\t\t"));
    // but other stuff is not
    EXPECT_EQ(mixxx::track::io::key::INVALID,
              KeyUtils::guessKeyFromText(" 33m\t\t"));
}

TEST_F(KeyUtilsTest, LancelotNotation) {
    EXPECT_EQ(mixxx::track::io::key::C_MAJOR,
              KeyUtils::guessKeyFromText("8B"));
    EXPECT_EQ(mixxx::track::io::key::C_SHARP_MINOR,
              KeyUtils::guessKeyFromText("12A"));

    // Allow lower-case to be more flexible when parsing search queries
    EXPECT_EQ(mixxx::track::io::key::C_MAJOR,
            KeyUtils::guessKeyFromText("8b"));
    EXPECT_EQ(mixxx::track::io::key::C_SHARP_MINOR,
            KeyUtils::guessKeyFromText("12a"));

    // whitespace is ok
    EXPECT_EQ(mixxx::track::io::key::B_MINOR,
            KeyUtils::guessKeyFromText("\t10A  "));
    // but other stuff is not
    EXPECT_EQ(mixxx::track::io::key::INVALID,
            KeyUtils::guessKeyFromText("\t10AA  "));
}

TEST_F(KeyUtilsTest, KeyNameNotation) {
    // Invalid letter
    // (actually valid in traditional German notation where B is H and Bb is B -
    //  everyone confused?)
    EXPECT_EQ(mixxx::track::io::key::INVALID,
              KeyUtils::guessKeyFromText("H"));

    // whitespace is ok
    EXPECT_EQ(mixxx::track::io::key::C_MAJOR,
              KeyUtils::guessKeyFromText(" \tC   \t  "));
    // but other crap is not
    EXPECT_EQ(mixxx::track::io::key::INVALID,
              KeyUtils::guessKeyFromText(" c\tC   c\t  "));

    // Major is upper-case, minor is lower-case.
    EXPECT_EQ(mixxx::track::io::key::C_MAJOR,
              KeyUtils::guessKeyFromText("C"));
    EXPECT_EQ(mixxx::track::io::key::C_MINOR,
              KeyUtils::guessKeyFromText("c"));

    // ... or explicitly written out
    EXPECT_EQ(mixxx::track::io::key::C_MAJOR,
              KeyUtils::guessKeyFromText("cmaj"));
    EXPECT_EQ(mixxx::track::io::key::C_MINOR,
              KeyUtils::guessKeyFromText("Cmin"));
    EXPECT_EQ(mixxx::track::io::key::C_MAJOR,
              KeyUtils::guessKeyFromText("cmajor"));
    EXPECT_EQ(mixxx::track::io::key::C_MINOR,
              KeyUtils::guessKeyFromText("Cminor"));

    // Sharps
    EXPECT_EQ(mixxx::track::io::key::D_FLAT_MAJOR,
              KeyUtils::guessKeyFromText("C#"));
    EXPECT_EQ(mixxx::track::io::key::D_FLAT_MAJOR,
              KeyUtils::guessKeyFromText(QString::fromUtf8("C♯")));

    // Flats
    EXPECT_EQ(mixxx::track::io::key::D_FLAT_MAJOR,
              KeyUtils::guessKeyFromText("Db"));
    EXPECT_EQ(mixxx::track::io::key::D_FLAT_MAJOR,
              KeyUtils::guessKeyFromText(QString::fromUtf8("D♭")));

    // Mixed sharps and flats.
    EXPECT_EQ(mixxx::track::io::key::C_MINOR,
              KeyUtils::guessKeyFromText("c#bb#"));

    // No matter what ending in m is minor.
    EXPECT_EQ(mixxx::track::io::key::C_MINOR,
              KeyUtils::guessKeyFromText("CM"));
    EXPECT_EQ(mixxx::track::io::key::C_MINOR,
              KeyUtils::guessKeyFromText("cm"));
    EXPECT_EQ(mixxx::track::io::key::C_SHARP_MINOR,
              KeyUtils::guessKeyFromText("C#m"));
    EXPECT_EQ(mixxx::track::io::key::C_SHARP_MINOR,
              KeyUtils::guessKeyFromText("Dbm"));
    EXPECT_EQ(mixxx::track::io::key::C_MINOR,
              KeyUtils::guessKeyFromText("C#bb#m"));

    // ... as is min
    EXPECT_EQ(mixxx::track::io::key::C_MINOR,
              KeyUtils::guessKeyFromText("CMiN"));
    EXPECT_EQ(mixxx::track::io::key::C_MINOR,
              KeyUtils::guessKeyFromText("cmIn"));
    EXPECT_EQ(mixxx::track::io::key::C_SHARP_MINOR,
              KeyUtils::guessKeyFromText("C#mIN"));
    EXPECT_EQ(mixxx::track::io::key::C_SHARP_MINOR,
              KeyUtils::guessKeyFromText("Dbmin"));
    EXPECT_EQ(mixxx::track::io::key::C_MINOR,
              KeyUtils::guessKeyFromText("C#bb#miN"));

    // but maj is major
    EXPECT_EQ(mixxx::track::io::key::C_MAJOR,
              KeyUtils::guessKeyFromText("CMaJ"));
    EXPECT_EQ(mixxx::track::io::key::C_MAJOR,
              KeyUtils::guessKeyFromText("cmAj"));
    EXPECT_EQ(mixxx::track::io::key::D_FLAT_MAJOR,
              KeyUtils::guessKeyFromText("C#mAJ"));
    EXPECT_EQ(mixxx::track::io::key::D_FLAT_MAJOR,
              KeyUtils::guessKeyFromText("Dbmaj"));
    EXPECT_EQ(mixxx::track::io::key::C_MAJOR,
              KeyUtils::guessKeyFromText("C#bb#maJ"));

    // Test going across the edges.
    EXPECT_EQ(mixxx::track::io::key::B_MAJOR,
              KeyUtils::guessKeyFromText("Cb"));
    EXPECT_EQ(mixxx::track::io::key::C_MAJOR,
              KeyUtils::guessKeyFromText("B#"));
    EXPECT_EQ(mixxx::track::io::key::B_MINOR,
              KeyUtils::guessKeyFromText("cb"));
    EXPECT_EQ(mixxx::track::io::key::C_MINOR,
              KeyUtils::guessKeyFromText("b#"));

    // Rapid Evolution test cases
    EXPECT_EQ(mixxx::track::io::key::A_MINOR,
            KeyUtils::guessKeyFromText("Am"));
    EXPECT_EQ(mixxx::track::io::key::A_MINOR,
            KeyUtils::guessKeyFromText("08A"));
    EXPECT_EQ(mixxx::track::io::key::B_FLAT_MINOR,
            KeyUtils::guessKeyFromText("A#m"));
    EXPECT_EQ(mixxx::track::io::key::B_FLAT_MINOR,
            KeyUtils::guessKeyFromText("Bbm"));
    EXPECT_EQ(mixxx::track::io::key::A_MAJOR,
            KeyUtils::guessKeyFromText("11B"));
    EXPECT_EQ(mixxx::track::io::key::A_FLAT_MAJOR, // ionian
            KeyUtils::guessKeyFromText("G#+50"));
    EXPECT_EQ(mixxx::track::io::key::A_FLAT_MAJOR, // ionian
            KeyUtils::guessKeyFromText("Ab +50"));
    EXPECT_EQ(mixxx::track::io::key::A_FLAT_MAJOR, // ionian
            KeyUtils::guessKeyFromText("Ab +50cents"));
    EXPECT_EQ(mixxx::track::io::key::A_FLAT_MAJOR, // ionian
            KeyUtils::guessKeyFromText("04B +50cents"));
    EXPECT_EQ(mixxx::track::io::key::A_FLAT_MAJOR, // ionian
            KeyUtils::guessKeyFromText("G#-50"));
    EXPECT_EQ(mixxx::track::io::key::A_FLAT_MAJOR, // ionian
            KeyUtils::guessKeyFromText("Ab -50"));
    EXPECT_EQ(mixxx::track::io::key::A_FLAT_MAJOR, // ionian
            KeyUtils::guessKeyFromText("Ab -50cents"));
    EXPECT_EQ(mixxx::track::io::key::A_FLAT_MAJOR, // ionian
            KeyUtils::guessKeyFromText("04B -50cents"));
    // Mixxx does not allow this but Rapid Evolution
    // EXPECT_EQ(mixxx::track::io::key::A_FLAT_MAJOR, // ionian
    //      KeyUtils::guessKeyFromText("    4b    -50   cents    "));
    // EXPECT_EQ(mixxx::track::io::key::A_FLAT_MAJOR, // ionian
    //          KeyUtils::guessKeyFromText("    g  #    -    50   cents    "));
    // EXPECT_EQ(mixxx::track::io::key::A_FLAT_MAJOR, // ionian
    //          KeyUtils::guessKeyFromText("    g  #    +    50   cents    "));
    EXPECT_EQ(mixxx::track::io::key::INVALID, // ionian
            KeyUtils::guessKeyFromText(" "));
    EXPECT_EQ(mixxx::track::io::key::INVALID, // ionian
            KeyUtils::guessKeyFromText(""));
    EXPECT_EQ(mixxx::track::io::key::INVALID, // ionian
            KeyUtils::guessKeyFromText("xyz"));
}

TEST_F(KeyUtilsTest, ScaleModeNotation) {
    EXPECT_EQ(mixxx::track::io::key::C_MAJOR,
            KeyUtils::guessKeyFromText("C ionian"));
    EXPECT_EQ(mixxx::track::io::key::A_MINOR,
            KeyUtils::guessKeyFromText("A aeolian"));
    EXPECT_EQ(mixxx::track::io::key::C_MAJOR,
            KeyUtils::guessKeyFromText("F lydian"));
    EXPECT_EQ(mixxx::track::io::key::C_MAJOR,
            KeyUtils::guessKeyFromText("G mixolydian"));
    EXPECT_EQ(mixxx::track::io::key::A_MINOR,
            KeyUtils::guessKeyFromText("D dorian"));
    EXPECT_EQ(mixxx::track::io::key::A_MINOR,
            KeyUtils::guessKeyFromText("E phrygian"));
    EXPECT_EQ(mixxx::track::io::key::A_MINOR,
            KeyUtils::guessKeyFromText("B locrian"));

    EXPECT_EQ(mixxx::track::io::key::F_SHARP_MINOR,
            KeyUtils::guessKeyFromText("11A"));
    EXPECT_EQ(mixxx::track::io::key::A_MAJOR,
            KeyUtils::guessKeyFromText("11B"));
    EXPECT_EQ(mixxx::track::io::key::A_MAJOR,
            KeyUtils::guessKeyFromText("11I"));
    EXPECT_EQ(mixxx::track::io::key::A_MAJOR,
            KeyUtils::guessKeyFromText("11L"));
    EXPECT_EQ(mixxx::track::io::key::A_MAJOR,
            KeyUtils::guessKeyFromText("11M"));
    EXPECT_EQ(mixxx::track::io::key::F_SHARP_MINOR,
            KeyUtils::guessKeyFromText("11D"));
    EXPECT_EQ(mixxx::track::io::key::F_SHARP_MINOR,
            KeyUtils::guessKeyFromText("11P"));
    EXPECT_EQ(mixxx::track::io::key::F_SHARP_MINOR,
            KeyUtils::guessKeyFromText("11C"));

    // Redundant Mode
    EXPECT_EQ(mixxx::track::io::key::INVALID,
            KeyUtils::guessKeyFromText("Cm ionian"));
}

mixxx::track::io::key::ChromaticKey incrementKey(
    mixxx::track::io::key::ChromaticKey key, int steps=1) {
    return static_cast<mixxx::track::io::key::ChromaticKey>(
        static_cast<int>(key) + steps);
}

TEST_F(KeyUtilsTest, ShortestStepsToKey_EqualKeyZeroSteps) {
    mixxx::track::io::key::ChromaticKey key = mixxx::track::io::key::INVALID;
    while (true) {
        EXPECT_EQ(0, KeyUtils::shortestStepsToKey(key, key));
        if (key == mixxx::track::io::key::B_MINOR) {
            break;
        }
        key = incrementKey(key);
    }
}

TEST_F(KeyUtilsTest, ShortestStepsToKey_SameTonicZeroSteps) {
    mixxx::track::io::key::ChromaticKey minor = mixxx::track::io::key::C_MINOR;
    mixxx::track::io::key::ChromaticKey major = mixxx::track::io::key::C_MAJOR;

    while (true) {
        EXPECT_EQ(0, KeyUtils::shortestStepsToKey(minor, major));
        if (minor == mixxx::track::io::key::B_MINOR) {
            break;
        }
        major = incrementKey(major);
        minor = incrementKey(minor);
    }
}

TEST_F(KeyUtilsTest, ShortestStepsToKey) {
    mixxx::track::io::key::ChromaticKey start_key_minor =
            mixxx::track::io::key::C_MINOR;
    mixxx::track::io::key::ChromaticKey start_key_major =
            mixxx::track::io::key::C_MAJOR;

    for (int i = 0; i < 12; ++i) {
        for (int j = 0; j < 12; ++j) {
            mixxx::track::io::key::ChromaticKey minor_key = KeyUtils::scaleKeySteps(start_key_minor, j);
            mixxx::track::io::key::ChromaticKey major_key = KeyUtils::scaleKeySteps(start_key_major, j);
            // When we are 6 steps away, 6 and -6 are equidistant.
            if (j == 6) {
                EXPECT_EQ(6, abs(KeyUtils::shortestStepsToKey(start_key_minor, minor_key)));
                EXPECT_EQ(6, abs(KeyUtils::shortestStepsToKey(start_key_minor, major_key)));
                EXPECT_EQ(6, abs(KeyUtils::shortestStepsToKey(start_key_major, minor_key)));
                EXPECT_EQ(6, abs(KeyUtils::shortestStepsToKey(start_key_major, major_key)));
            } else {
                int expected = (j < 6) ? j : j - 12;
                EXPECT_EQ(expected, KeyUtils::shortestStepsToKey(start_key_minor, minor_key));
                EXPECT_EQ(expected, KeyUtils::shortestStepsToKey(start_key_minor, major_key));
                EXPECT_EQ(expected, KeyUtils::shortestStepsToKey(start_key_major, minor_key));
                EXPECT_EQ(expected, KeyUtils::shortestStepsToKey(start_key_major, major_key));
            }
        }
        start_key_minor = KeyUtils::scaleKeySteps(start_key_minor, 1);
        start_key_major = KeyUtils::scaleKeySteps(start_key_major, 1);
    }
}

TEST_F(KeyUtilsTest, GetCompatibleKeys) {
    // The relative major/minor, the perfect 4th major/minor and the
    // perfect 5th major/minor are all compatible. This is easily
    // checked with the Circle of Fifths.

    // Test keys on the boundary between 1 and 12 to check that wrap-around
    // works.
    mixxx::track::io::key::ChromaticKey key =
            mixxx::track::io::key::A_MINOR;
    QList<mixxx::track::io::key::ChromaticKey> compatible =
            KeyUtils::getCompatibleKeys(key);
    EXPECT_THAT(compatible,
            UnorderedElementsAre(mixxx::track::io::key::C_MAJOR,
                    mixxx::track::io::key::F_MAJOR,
                    mixxx::track::io::key::G_MAJOR,
                    mixxx::track::io::key::D_MINOR,
                    mixxx::track::io::key::E_MINOR,
                    mixxx::track::io::key::A_MINOR));

    key = mixxx::track::io::key::F_MAJOR;
    compatible = KeyUtils::getCompatibleKeys(key);
    EXPECT_THAT(compatible,
            UnorderedElementsAre(mixxx::track::io::key::C_MAJOR,
                    mixxx::track::io::key::F_MAJOR,
                    mixxx::track::io::key::B_FLAT_MAJOR,
                    mixxx::track::io::key::D_MINOR,
                    mixxx::track::io::key::A_MINOR,
                    mixxx::track::io::key::G_MINOR));
}

// Expected base (minor/"A") colors per Camelot wheel number, from the
// table in docs/decisions/0002-camelot-key-coloring.md. Index 0 is unused
// (wheel numbers are 1-12) so the array can be indexed directly.
constexpr QRgb kExpectedCamelotWheelColor[13] = {
        0x000000, // unused
        0xE53935, // 1 - red
        0xFB8C00, // 2 - orange
        0xFDD835, // 3 - yellow
        0xC0CA33, // 4 - lime
        0x43A047, // 5 - green
        0x00ACC1, // 6 - teal
        0x1E88E5, // 7 - blue
        0x3949AB, // 8 - indigo
        0x8E24AA, // 9 - purple
        0xD81B60, // 10 - pink
        0x6D4C41, // 11 - brown
        0x546E7A, // 12 - blue-grey
};

TEST_F(KeyUtilsTest, KeyToCamelotColor_InvalidKeyReturnsInvalidColor) {
    EXPECT_FALSE(KeyUtils::keyToCamelotColor(mixxx::track::io::key::INVALID).isValid());
}

TEST_F(KeyUtilsTest, KeyToCamelotColor_MinorKeyMatchesAdrTable) {
    // Per KeyUtils::keyToString(..., KeyNotation::Lancelot) (verified by the
    // existing LancelotNotation test above): A_MINOR == "8A",
    // D_MINOR == "7A", C_SHARP_MINOR == "12A".
    EXPECT_EQ(QColor(kExpectedCamelotWheelColor[8]),
            KeyUtils::keyToCamelotColor(mixxx::track::io::key::A_MINOR));
    EXPECT_EQ(QColor(kExpectedCamelotWheelColor[7]),
            KeyUtils::keyToCamelotColor(mixxx::track::io::key::D_MINOR));
    EXPECT_EQ(QColor(kExpectedCamelotWheelColor[12]),
            KeyUtils::keyToCamelotColor(mixxx::track::io::key::C_SHARP_MINOR));
}

TEST_F(KeyUtilsTest, KeyToCamelotColor_MajorIsLighterTintOfRelativeMinor) {
    // A_MINOR == "8A", its relative major C_MAJOR == "8B" (same wheel
    // number): same base hue, lighter per keyToCamelotColor's documented
    // .lighter(135).
    const QColor minorColor = KeyUtils::keyToCamelotColor(mixxx::track::io::key::A_MINOR);
    const QColor majorColor = KeyUtils::keyToCamelotColor(mixxx::track::io::key::C_MAJOR);
    EXPECT_EQ(QColor(kExpectedCamelotWheelColor[8]).lighter(135), majorColor);
    // Major and minor of the same wheel number must be distinguishable.
    EXPECT_NE(minorColor, majorColor);
}

TEST_F(KeyUtilsTest, KeyToCamelotColor_AdjacentWheelNumbersAreDistinct) {
    // Spot-check that neighboring wheel positions (per the ADR table) are
    // visually distinct colors, not coincidentally identical.
    // A_MINOR=="8A", E_MINOR=="9A", B_MINOR=="10A", G_MINOR=="6A", D_MINOR=="7A".
    EXPECT_NE(KeyUtils::keyToCamelotColor(mixxx::track::io::key::A_MINOR),  // 8A
            KeyUtils::keyToCamelotColor(mixxx::track::io::key::E_MINOR));  // 9A
    EXPECT_NE(KeyUtils::keyToCamelotColor(mixxx::track::io::key::E_MINOR), // 9A
            KeyUtils::keyToCamelotColor(mixxx::track::io::key::B_MINOR)); // 10A
    EXPECT_NE(KeyUtils::keyToCamelotColor(mixxx::track::io::key::G_MINOR),      // 6A
            KeyUtils::keyToCamelotColor(mixxx::track::io::key::D_MINOR));      // 7A
}

TEST_F(KeyUtilsTest, KeyToCamelotColor_AllTwelveWheelPositionsAreDistinct) {
    // Walk all 12 minor keys around the wheel in fifths (A_MINOR == "8A",
    // per the LancelotNotation test above) and verify each of the 12 base
    // colors is unique, per the ADR's "distributed around the hue wheel"
    // intent.
    mixxx::track::io::key::ChromaticKey key = mixxx::track::io::key::A_MINOR;
    QSet<QRgb> seenColors;
    for (int i = 0; i < 12; ++i) {
        const QColor color = KeyUtils::keyToCamelotColor(key);
        EXPECT_TRUE(color.isValid());
        EXPECT_FALSE(seenColors.contains(color.rgb()))
                << "Duplicate Camelot wheel color at step " << i;
        seenColors.insert(color.rgb());
        key = KeyUtils::scaleKeySteps(key, 7); // +7 semitones == next wheel number
    }
    EXPECT_EQ(12, seenColors.size());
}
