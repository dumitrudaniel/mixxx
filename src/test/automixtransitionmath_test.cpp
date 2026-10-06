#include "mixer/automixtransitionmath.h"

#include <gtest/gtest.h>

#include <cmath>

using Shape = AutomixTransitionMath::Shape;

class AutomixTransitionMathTest : public testing::Test {
};

TEST_F(AutomixTransitionMathTest, ShapesStartAtZeroAndEndAtOne) {
    for (const Shape shape : {Shape::Linear, Shape::Sin, Shape::Cos, Shape::Smoothstep}) {
        EXPECT_DOUBLE_EQ(0.0, AutomixTransitionMath::applyShape(shape, 0.0));
        EXPECT_NEAR(1.0, AutomixTransitionMath::applyShape(shape, 1.0), 1e-12);
    }
}

TEST_F(AutomixTransitionMathTest, ShapesClampOutOfRangeTime) {
    for (const Shape shape :
            {Shape::Linear, Shape::Sin, Shape::Cos, Shape::Smoothstep, Shape::Hold}) {
        EXPECT_DOUBLE_EQ(0.0, AutomixTransitionMath::applyShape(shape, -3.0));
        EXPECT_NEAR(1.0, AutomixTransitionMath::applyShape(shape, 7.0), 1e-12);
    }
}

TEST_F(AutomixTransitionMathTest, SinIsFastStartCosIsSoftStart) {
    // Sin (ease-out) is ahead of linear in the first half, Cos (ease-in) behind.
    EXPECT_GT(AutomixTransitionMath::applyShape(Shape::Sin, 0.25), 0.25);
    EXPECT_LT(AutomixTransitionMath::applyShape(Shape::Cos, 0.25), 0.25);
    EXPECT_DOUBLE_EQ(0.5, AutomixTransitionMath::applyShape(Shape::Smoothstep, 0.5));
}

TEST_F(AutomixTransitionMathTest, HoldJumpsOnlyAtSegmentEnd) {
    EXPECT_DOUBLE_EQ(0.0, AutomixTransitionMath::applyShape(Shape::Hold, 0.999));
    EXPECT_DOUBLE_EQ(1.0, AutomixTransitionMath::applyShape(Shape::Hold, 1.0));
}

TEST_F(AutomixTransitionMathTest, EqualPowerSwapKeepsPowerConstant) {
    // Outgoing band 1 -> 0 with Cos, incoming band 0 -> 1 with Sin: the sum of
    // squared gains (power of two uncorrelated signals) stays 1 throughout.
    for (int i = 0; i <= 20; ++i) {
        const double t = i / 20.0;
        const double out = AutomixTransitionMath::interpolate(1.0, 0.0, t, Shape::Cos);
        const double in = AutomixTransitionMath::interpolate(0.0, 1.0, t, Shape::Sin);
        EXPECT_NEAR(1.0, out * out + in * in, 1e-12) << "t=" << t;
    }
}

TEST_F(AutomixTransitionMathTest, InterpolateBetweenArbitraryValues) {
    EXPECT_DOUBLE_EQ(0.25, AutomixTransitionMath::interpolate(0.25, 1.0, 0.0, Shape::Sin));
    EXPECT_DOUBLE_EQ(0.625, AutomixTransitionMath::interpolate(0.25, 1.0, 0.5, Shape::Linear));
    EXPECT_NEAR(1.0, AutomixTransitionMath::interpolate(0.25, 1.0, 1.0, Shape::Sin), 1e-12);
}

TEST_F(AutomixTransitionMathTest, DbToGain) {
    EXPECT_DOUBLE_EQ(1.0, AutomixTransitionMath::dbToGain(0.0));
    EXPECT_NEAR(0.2512, AutomixTransitionMath::dbToGain(-12.0), 1e-4);
    EXPECT_NEAR(0.5012, AutomixTransitionMath::dbToGain(-6.0), 1e-4);
}

TEST_F(AutomixTransitionMathTest, NextStartBeatGoesToNextBar) {
    // Beat 5.5 is inside bar 2 (beats 4..8): next bar starts at beat 8.
    EXPECT_DOUBLE_EQ(8.0, AutomixTransitionMath::nextStartBeat(5.5, 4.0, 0.1));
    // Quantum of 4 bars (16 beats).
    EXPECT_DOUBLE_EQ(16.0, AutomixTransitionMath::nextStartBeat(5.5, 16.0, 0.1));
}

TEST_F(AutomixTransitionMathTest, NextStartBeatGraceKeepsJustPassedBoundary) {
    // Pressed 0.05 beats after the downbeat: start on that downbeat.
    EXPECT_DOUBLE_EQ(8.0, AutomixTransitionMath::nextStartBeat(8.05, 4.0, 0.1));
    // 0.2 beats late is outside the grace window: wait for the next bar.
    EXPECT_DOUBLE_EQ(12.0, AutomixTransitionMath::nextStartBeat(8.2, 4.0, 0.1));
    // Exactly on the boundary.
    EXPECT_DOUBLE_EQ(8.0, AutomixTransitionMath::nextStartBeat(8.0, 4.0, 0.1));
}

TEST_F(AutomixTransitionMathTest, NextStartBeatBeforeGridAnchor) {
    // Negative beats (before Mixxx's first downbeat) still quantize to bars.
    EXPECT_DOUBLE_EQ(-4.0, AutomixTransitionMath::nextStartBeat(-6.5, 4.0, 0.1));
    EXPECT_DOUBLE_EQ(0.0, AutomixTransitionMath::nextStartBeat(-0.5, 4.0, 0.1));
}

TEST_F(AutomixTransitionMathTest, NextStartBeatInvalidQuantumReturnsBeat) {
    EXPECT_DOUBLE_EQ(5.5, AutomixTransitionMath::nextStartBeat(5.5, 0.0, 0.1));
}

TEST_F(AutomixTransitionMathTest, ClockFollowsGridWhilePlaying) {
    // Late engine callback: the grid moved more than wall time predicts, but
    // within slack -- the grid is the truth.
    EXPECT_DOUBLE_EQ(0.1, AutomixTransitionMath::clockAdvance(0.1, 0.035, true));
    // No engine callback since last tick: no motion this tick.
    EXPECT_DOUBLE_EQ(0.0, AutomixTransitionMath::clockAdvance(0.0, 0.035, true));
}

TEST_F(AutomixTransitionMathTest, ClockIgnoresLoopWrapAndSeek) {
    // Loop wrap: grid jumped back 4 beats -> keep wall-clock pace.
    EXPECT_DOUBLE_EQ(0.035, AutomixTransitionMath::clockAdvance(-4.0, 0.035, true));
    // Seek forward 32 beats -> keep wall-clock pace.
    EXPECT_DOUBLE_EQ(0.035, AutomixTransitionMath::clockAdvance(32.0, 0.035, true));
    // Tiny backwards jitter is clamped to zero, not treated as a jump.
    EXPECT_DOUBLE_EQ(0.0, AutomixTransitionMath::clockAdvance(-0.01, 0.035, true));
}

TEST_F(AutomixTransitionMathTest, ClockKeepsRunningWhenOutgoingStops) {
    EXPECT_DOUBLE_EQ(0.035, AutomixTransitionMath::clockAdvance(0.0, 0.035, false));
    EXPECT_DOUBLE_EQ(0.0, AutomixTransitionMath::clockAdvance(0.0, -1.0, false));
}

TEST_F(AutomixTransitionMathTest, TempoMatchedIncomingRateRatio_FromUnity) {
    // Incoming 120 BPM at unity rate; outgoing 128 BPM -> 128/120.
    EXPECT_DOUBLE_EQ(128.0 / 120.0,
            AutomixTransitionMath::tempoMatchedIncomingRateRatio(128.0, 120.0, 1.0));
}

TEST_F(AutomixTransitionMathTest, TempoMatchedIncomingRateRatio_PreservesExistingRatio) {
    // Effective incoming bpm 126.5 (from 1.1 ratio); target 128 -> scale the
    // existing ratio, not reset it.
    EXPECT_DOUBLE_EQ(1.1 * (128.0 / 126.5),
            AutomixTransitionMath::tempoMatchedIncomingRateRatio(128.0, 126.5, 1.1));
}

TEST_F(AutomixTransitionMathTest, TempoMatchedIncomingRateRatio_AlreadyMatchedIsNoOp) {
    EXPECT_DOUBLE_EQ(0.97, AutomixTransitionMath::tempoMatchedIncomingRateRatio(105.0, 105.0, 0.97));
}

TEST_F(AutomixTransitionMathTest, TempoMatchedIncomingRateRatio_InvalidInputsReturnNegative) {
    EXPECT_LT(AutomixTransitionMath::tempoMatchedIncomingRateRatio(0.0, 120.0, 1.0), 0.0);
    EXPECT_LT(AutomixTransitionMath::tempoMatchedIncomingRateRatio(128.0, 0.0, 1.0), 0.0);
    EXPECT_LT(AutomixTransitionMath::tempoMatchedIncomingRateRatio(128.0, 120.0, 0.0), 0.0);
    EXPECT_LT(AutomixTransitionMath::tempoMatchedIncomingRateRatio(-1.0, 120.0, 1.0), 0.0);
}
