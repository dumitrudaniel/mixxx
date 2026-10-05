#include "mixer/automixtransitionmath.h"

#include <gtest/gtest.h>

class AutomixTransitionMathTest : public testing::Test {
};

TEST_F(AutomixTransitionMathTest, TransitionDurationSeconds_128Bpm) {
    // 2 bars * 4 beats/bar = 8 beats. At 128 BPM, one beat = 60/128 s.
    const double expected = 8.0 * (60.0 / 128.0);
    EXPECT_DOUBLE_EQ(expected, AutomixTransitionMath::transitionDurationSeconds(128.0));
}

TEST_F(AutomixTransitionMathTest, TransitionDurationSeconds_120Bpm) {
    // Round number: 8 beats at 120 BPM (0.5s/beat) = 4 seconds exactly.
    EXPECT_DOUBLE_EQ(4.0, AutomixTransitionMath::transitionDurationSeconds(120.0));
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

// --- EQ gain (low/mid/high, all identical) -- continuous linear fade,
// replacing the old instant-swap-at-midpoint behavior (2026-10-05). ---

TEST_F(AutomixTransitionMathTest, OutgoingEqGainForProgress_FadesUnityToCut) {
    EXPECT_DOUBLE_EQ(1.0, AutomixTransitionMath::outgoingEqGainForProgress(0.0));
    EXPECT_DOUBLE_EQ(0.75, AutomixTransitionMath::outgoingEqGainForProgress(0.25));
    EXPECT_DOUBLE_EQ(0.5, AutomixTransitionMath::outgoingEqGainForProgress(0.5));
    EXPECT_DOUBLE_EQ(0.25, AutomixTransitionMath::outgoingEqGainForProgress(0.75));
    EXPECT_DOUBLE_EQ(0.0, AutomixTransitionMath::outgoingEqGainForProgress(1.0));
}

TEST_F(AutomixTransitionMathTest, IncomingEqGainForProgress_FadesCutToUnity) {
    EXPECT_DOUBLE_EQ(0.0, AutomixTransitionMath::incomingEqGainForProgress(0.0));
    EXPECT_DOUBLE_EQ(0.25, AutomixTransitionMath::incomingEqGainForProgress(0.25));
    EXPECT_DOUBLE_EQ(0.5, AutomixTransitionMath::incomingEqGainForProgress(0.5));
    EXPECT_DOUBLE_EQ(0.75, AutomixTransitionMath::incomingEqGainForProgress(0.75));
    EXPECT_DOUBLE_EQ(1.0, AutomixTransitionMath::incomingEqGainForProgress(1.0));
}

TEST_F(AutomixTransitionMathTest, EqGainForProgress_AlwaysSumsToUnity) {
    // Continuous crossfade: at every point the two gains sum to exactly 1.0
    // (unlike the old instant-swap MVP, which kept both at their endpoints
    // until the midpoint).
    for (double progress = 0.0; progress <= 1.0; progress += 0.1) {
        const double outgoing = AutomixTransitionMath::outgoingEqGainForProgress(progress);
        const double incoming = AutomixTransitionMath::incomingEqGainForProgress(progress);
        EXPECT_NEAR(1.0, outgoing + incoming, 1e-9);
    }
}

TEST_F(AutomixTransitionMathTest, EqGainForProgress_ClampsOutOfRangeProgress) {
    EXPECT_DOUBLE_EQ(1.0, AutomixTransitionMath::outgoingEqGainForProgress(-0.5));
    EXPECT_DOUBLE_EQ(0.0, AutomixTransitionMath::outgoingEqGainForProgress(1.5));
    EXPECT_DOUBLE_EQ(0.0, AutomixTransitionMath::incomingEqGainForProgress(-0.5));
    EXPECT_DOUBLE_EQ(1.0, AutomixTransitionMath::incomingEqGainForProgress(1.5));
}

// --- Filter ("Filter" knob / super1) sweep (added 2026-10-05). ---

TEST_F(AutomixTransitionMathTest, OutgoingFilterForProgress_SweepsNeutralToHighPassEnd) {
    EXPECT_DOUBLE_EQ(0.5, AutomixTransitionMath::outgoingFilterForProgress(0.0));
    EXPECT_DOUBLE_EQ(0.75, AutomixTransitionMath::outgoingFilterForProgress(0.5));
    EXPECT_DOUBLE_EQ(1.0, AutomixTransitionMath::outgoingFilterForProgress(1.0));
}

TEST_F(AutomixTransitionMathTest, IncomingFilterForProgress_SweepsMildLowPassToNeutral) {
    EXPECT_DOUBLE_EQ(0.3, AutomixTransitionMath::incomingFilterForProgress(0.0));
    EXPECT_DOUBLE_EQ(0.4, AutomixTransitionMath::incomingFilterForProgress(0.5));
    EXPECT_DOUBLE_EQ(0.5, AutomixTransitionMath::incomingFilterForProgress(1.0));
}

TEST_F(AutomixTransitionMathTest, FilterForProgress_ClampsOutOfRangeProgress) {
    EXPECT_DOUBLE_EQ(0.5, AutomixTransitionMath::outgoingFilterForProgress(-0.5));
    EXPECT_DOUBLE_EQ(1.0, AutomixTransitionMath::outgoingFilterForProgress(1.5));
    EXPECT_DOUBLE_EQ(0.3, AutomixTransitionMath::incomingFilterForProgress(-0.5));
    EXPECT_DOUBLE_EQ(0.5, AutomixTransitionMath::incomingFilterForProgress(1.5));
}

TEST_F(AutomixTransitionMathTest, FilterForProgress_StaysWithinUnitRange) {
    // Both decks' filter values must stay within the CO's own [0, 1] range
    // throughout the transition -- no overshoot past full HPF/LPF.
    for (double progress = 0.0; progress <= 1.0; progress += 0.1) {
        const double outgoing = AutomixTransitionMath::outgoingFilterForProgress(progress);
        const double incoming = AutomixTransitionMath::incomingFilterForProgress(progress);
        EXPECT_GE(outgoing, 0.0);
        EXPECT_LE(outgoing, 1.0);
        EXPECT_GE(incoming, 0.0);
        EXPECT_LE(incoming, 1.0);
    }
}
