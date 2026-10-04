#include "mixer/automixtransitionmath.h"

#include <gtest/gtest.h>

class AutomixTransitionMathTest : public testing::Test {
};

TEST_F(AutomixTransitionMathTest, TransitionDurationSeconds_128Bpm) {
    // 16 bars * 4 beats/bar = 64 beats. At 128 BPM, one beat = 60/128 s.
    const double expected = 64.0 * (60.0 / 128.0);
    EXPECT_DOUBLE_EQ(expected, AutomixTransitionMath::transitionDurationSeconds(128.0));
}

TEST_F(AutomixTransitionMathTest, TransitionDurationSeconds_120Bpm) {
    // Round number: 64 beats at 120 BPM (0.5s/beat) = 32 seconds exactly.
    EXPECT_DOUBLE_EQ(32.0, AutomixTransitionMath::transitionDurationSeconds(120.0));
}

TEST_F(AutomixTransitionMathTest, TransitionDurationSeconds_InvalidBpmReturnsNegative) {
    EXPECT_LT(AutomixTransitionMath::transitionDurationSeconds(0.0), 0.0);
    EXPECT_LT(AutomixTransitionMath::transitionDurationSeconds(-5.0), 0.0);
}

TEST_F(AutomixTransitionMathTest, ProgressForElapsed_Midpoint) {
    EXPECT_DOUBLE_EQ(0.5, AutomixTransitionMath::progressForElapsed(16.0, 32.0));
}

TEST_F(AutomixTransitionMathTest, ProgressForElapsed_ClampsToOne) {
    EXPECT_DOUBLE_EQ(1.0, AutomixTransitionMath::progressForElapsed(99.0, 32.0));
}

TEST_F(AutomixTransitionMathTest, ProgressForElapsed_ClampsToZero) {
    EXPECT_DOUBLE_EQ(0.0, AutomixTransitionMath::progressForElapsed(-5.0, 32.0));
}

TEST_F(AutomixTransitionMathTest, ProgressForElapsed_ZeroDurationIsZero) {
    EXPECT_DOUBLE_EQ(0.0, AutomixTransitionMath::progressForElapsed(1.0, 0.0));
}

TEST_F(AutomixTransitionMathTest, CrossfaderForProgress_Deck1ToDeck2Sweep) {
    EXPECT_DOUBLE_EQ(-1.0, AutomixTransitionMath::crossfaderForProgress(0.0, true));
    EXPECT_DOUBLE_EQ(0.0, AutomixTransitionMath::crossfaderForProgress(0.5, true));
    EXPECT_DOUBLE_EQ(1.0, AutomixTransitionMath::crossfaderForProgress(1.0, true));
}

TEST_F(AutomixTransitionMathTest, CrossfaderForProgress_Deck2ToDeck1SweepIsReversed) {
    EXPECT_DOUBLE_EQ(1.0, AutomixTransitionMath::crossfaderForProgress(0.0, false));
    EXPECT_DOUBLE_EQ(0.0, AutomixTransitionMath::crossfaderForProgress(0.5, false));
    EXPECT_DOUBLE_EQ(-1.0, AutomixTransitionMath::crossfaderForProgress(1.0, false));
}

TEST_F(AutomixTransitionMathTest, BassSwap_HappensExactlyAtMidpoint) {
    // Just before the midpoint: outgoing deck still has bass, incoming is cut.
    EXPECT_DOUBLE_EQ(1.0, AutomixTransitionMath::outgoingBassGainForProgress(0.49));
    EXPECT_DOUBLE_EQ(0.0, AutomixTransitionMath::incomingBassGainForProgress(0.49));

    // At and after the midpoint: swapped.
    EXPECT_DOUBLE_EQ(0.0, AutomixTransitionMath::outgoingBassGainForProgress(0.5));
    EXPECT_DOUBLE_EQ(1.0, AutomixTransitionMath::incomingBassGainForProgress(0.5));
    EXPECT_DOUBLE_EQ(0.0, AutomixTransitionMath::outgoingBassGainForProgress(0.9));
    EXPECT_DOUBLE_EQ(1.0, AutomixTransitionMath::incomingBassGainForProgress(0.9));
}

TEST_F(AutomixTransitionMathTest, BassSwap_AlwaysComplementary) {
    for (double progress = 0.0; progress <= 1.0; progress += 0.1) {
        const double outgoing = AutomixTransitionMath::outgoingBassGainForProgress(progress);
        const double incoming = AutomixTransitionMath::incomingBassGainForProgress(progress);
        // Exactly one of the two decks has bass at any point in the transition.
        EXPECT_NE(outgoing, incoming);
    }
}
