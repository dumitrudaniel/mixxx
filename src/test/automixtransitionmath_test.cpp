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

// Crossfader front-loaded 2026-10-06 (Dan's "fully in effect by the
// midpoint" proposal): the full -1..+1 sweep now completes by
// kCrossfadeSwapFraction (0.5) of progress, not progress==1.0, then holds.
// easeInOut(0.5)==0.5 exactly, so the quarter-progress point (half of the
// front-loaded window) still lands exactly at the crossfader's own center.

TEST_F(AutomixTransitionMathTest, CrossfaderForProgress_Deck1ToDeck2Sweep) {
    EXPECT_DOUBLE_EQ(-1.0, AutomixTransitionMath::crossfaderForProgress(0.0, true));
    EXPECT_DOUBLE_EQ(0.0, AutomixTransitionMath::crossfaderForProgress(0.25, true));
    EXPECT_DOUBLE_EQ(1.0, AutomixTransitionMath::crossfaderForProgress(0.5, true));
    EXPECT_DOUBLE_EQ(1.0, AutomixTransitionMath::crossfaderForProgress(1.0, true));
}

TEST_F(AutomixTransitionMathTest, CrossfaderForProgress_Deck2ToDeck1SweepIsReversed) {
    EXPECT_DOUBLE_EQ(1.0, AutomixTransitionMath::crossfaderForProgress(0.0, false));
    EXPECT_DOUBLE_EQ(0.0, AutomixTransitionMath::crossfaderForProgress(0.25, false));
    EXPECT_DOUBLE_EQ(-1.0, AutomixTransitionMath::crossfaderForProgress(0.5, false));
    EXPECT_DOUBLE_EQ(-1.0, AutomixTransitionMath::crossfaderForProgress(1.0, false));
}

TEST_F(AutomixTransitionMathTest, CrossfaderForProgress_HoldsAtEndpointPastSwapFraction) {
    // Past the front-loaded window, the crossfader must stay pinned at the
    // endpoint, not overshoot or reverse.
    EXPECT_DOUBLE_EQ(1.0, AutomixTransitionMath::crossfaderForProgress(0.7, true));
    EXPECT_DOUBLE_EQ(1.0, AutomixTransitionMath::crossfaderForProgress(1.0, true));
}

// --- Mid/high EQ gain fade REMOVED 2026-10-05 (double-attenuation addendum):
// it rode a second full attenuation-equivalent fade on top of the
// (constant-power) crossfader, compounding multiplicatively and producing a
// real volume dip at the midpoint that read as a "jump" on recovery. Mid/
// high bands are now left untouched (unity gain) for the whole transition --
// the crossfader alone carries their presence. The OutgoingEqGainForProgress/
// IncomingEqGainForProgress tests that used to live here, and the function
// pair itself, are gone, not just unused. ---

// --- Low-band-only bass swap curve (re-added 2026-10-05, second addendum
// same day): front-loaded, completes by kBassSwapFraction (0.6) of
// progress, then holds -- unlike the mid/high curve above, which spans the
// whole [0,1] range. ---

TEST_F(AutomixTransitionMathTest, OutgoingBassGainForProgress_FrontLoadedThenHolds) {
    EXPECT_DOUBLE_EQ(1.0, AutomixTransitionMath::outgoingBassGainForProgress(0.0));
    // Halfway through the 0.6 swap window (progress 0.3): halfway faded.
    EXPECT_DOUBLE_EQ(0.5, AutomixTransitionMath::outgoingBassGainForProgress(0.3));
    // Swap complete exactly at kBassSwapFraction.
    EXPECT_DOUBLE_EQ(0.0, AutomixTransitionMath::outgoingBassGainForProgress(0.6));
    // Holds at cut for the remainder of the transition.
    EXPECT_DOUBLE_EQ(0.0, AutomixTransitionMath::outgoingBassGainForProgress(0.8));
    EXPECT_DOUBLE_EQ(0.0, AutomixTransitionMath::outgoingBassGainForProgress(1.0));
}

TEST_F(AutomixTransitionMathTest, IncomingBassGainForProgress_FrontLoadedThenHolds) {
    EXPECT_DOUBLE_EQ(0.0, AutomixTransitionMath::incomingBassGainForProgress(0.0));
    EXPECT_DOUBLE_EQ(0.5, AutomixTransitionMath::incomingBassGainForProgress(0.3));
    EXPECT_DOUBLE_EQ(1.0, AutomixTransitionMath::incomingBassGainForProgress(0.6));
    // Holds at unity for the remainder of the transition.
    EXPECT_DOUBLE_EQ(1.0, AutomixTransitionMath::incomingBassGainForProgress(0.8));
    EXPECT_DOUBLE_EQ(1.0, AutomixTransitionMath::incomingBassGainForProgress(1.0));
}

TEST_F(AutomixTransitionMathTest, BassGainForProgress_AlwaysSumsToUnity) {
    for (double progress = 0.0; progress <= 1.0; progress += 0.1) {
        const double outgoing = AutomixTransitionMath::outgoingBassGainForProgress(progress);
        const double incoming = AutomixTransitionMath::incomingBassGainForProgress(progress);
        EXPECT_NEAR(1.0, outgoing + incoming, 1e-9);
    }
}

TEST_F(AutomixTransitionMathTest, BassGainForProgress_ClampsOutOfRangeProgress) {
    EXPECT_DOUBLE_EQ(1.0, AutomixTransitionMath::outgoingBassGainForProgress(-0.5));
    EXPECT_DOUBLE_EQ(0.0, AutomixTransitionMath::outgoingBassGainForProgress(1.5));
    EXPECT_DOUBLE_EQ(0.0, AutomixTransitionMath::incomingBassGainForProgress(-0.5));
    EXPECT_DOUBLE_EQ(1.0, AutomixTransitionMath::incomingBassGainForProgress(1.5));
}

// --- easeInOut() smoothstep helper (added 2026-10-05, filter-sweep-feels-
// robotic fix). Same endpoints/midpoint as linear, different shape between
// them. ---

TEST_F(AutomixTransitionMathTest, EaseInOut_EndpointsAndMidpointMatchLinear) {
    EXPECT_DOUBLE_EQ(0.0, AutomixTransitionMath::easeInOut(0.0));
    EXPECT_DOUBLE_EQ(0.5, AutomixTransitionMath::easeInOut(0.5));
    EXPECT_DOUBLE_EQ(1.0, AutomixTransitionMath::easeInOut(1.0));
}

TEST_F(AutomixTransitionMathTest, EaseInOut_SlowerThanLinearNearEndpoints) {
    // Smoothstep accelerates away from 0 and decelerates into 1, so at a
    // quarter of the way through it should have covered LESS ground than
    // linear (0.25), and symmetrically more than linear at the 3/4 mark.
    EXPECT_LT(AutomixTransitionMath::easeInOut(0.25), 0.25);
    EXPECT_GT(AutomixTransitionMath::easeInOut(0.75), 0.75);
}

TEST_F(AutomixTransitionMathTest, EaseInOut_IsMonotonicNonDecreasing) {
    double previous = AutomixTransitionMath::easeInOut(0.0);
    for (double t = 0.05; t <= 1.0; t += 0.05) {
        const double current = AutomixTransitionMath::easeInOut(t);
        EXPECT_GE(current, previous);
        previous = current;
    }
}

TEST_F(AutomixTransitionMathTest, EaseInOut_ClampsOutOfRangeInput) {
    EXPECT_DOUBLE_EQ(0.0, AutomixTransitionMath::easeInOut(-0.5));
    EXPECT_DOUBLE_EQ(1.0, AutomixTransitionMath::easeInOut(1.5));
}

// --- Filter ("Filter" knob / super1) sweep (added 2026-10-05). Now eased
// (see easeInOut() above) rather than linear -- endpoints/midpoint values
// below are unchanged because easeInOut(0)==0, easeInOut(0.5)==0.5,
// easeInOut(1)==1, same as plain linear progress at exactly those three
// points. ---

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

// --- EXPERIMENTAL symmetric mid-band scoop (added 2026-10-05, see
// docs/decisions/0008 addendum -- pending Dan's ear-judgment, not a
// confirmed-good feature like the ones above). Both decks get the SAME
// value at a given progress (no outgoing/incoming distinction), peaking
// (lowest gain) at progress=0.5, unity at both ends. ---

TEST_F(AutomixTransitionMathTest, MidScoopGainForProgress_UnityAtBothEnds) {
    EXPECT_NEAR(1.0, AutomixTransitionMath::midScoopGainForProgress(0.0), 1e-9);
    EXPECT_NEAR(1.0, AutomixTransitionMath::midScoopGainForProgress(1.0), 1e-9);
}

TEST_F(AutomixTransitionMathTest, MidScoopGainForProgress_DipsAtMidpointByConfiguredDepth) {
    const double expected = 1.0 - AutomixTransitionMath::kMidScoopDepth;
    EXPECT_NEAR(expected, AutomixTransitionMath::midScoopGainForProgress(0.5), 1e-9);
}

TEST_F(AutomixTransitionMathTest, MidScoopGainForProgress_NeverExceedsConfiguredDepth) {
    // At no progress should the dip go deeper than kMidScoopDepth below
    // unity -- it's a mild hump, not anywhere close to a full cut.
    for (double progress = 0.0; progress <= 1.0; progress += 0.05) {
        const double value = AutomixTransitionMath::midScoopGainForProgress(progress);
        EXPECT_GE(value, 1.0 - AutomixTransitionMath::kMidScoopDepth - 1e-9);
        EXPECT_LE(value, 1.0 + 1e-9);
    }
}

TEST_F(AutomixTransitionMathTest, MidScoopGainForProgress_SymmetricAroundMidpoint) {
    // The hump shape must be symmetric: equal progress-distance from either
    // end produces the same gain value.
    EXPECT_NEAR(AutomixTransitionMath::midScoopGainForProgress(0.2),
            AutomixTransitionMath::midScoopGainForProgress(0.8),
            1e-9);
    EXPECT_NEAR(AutomixTransitionMath::midScoopGainForProgress(0.35),
            AutomixTransitionMath::midScoopGainForProgress(0.65),
            1e-9);
}

TEST_F(AutomixTransitionMathTest, MidScoopGainForProgress_ClampsOutOfRangeProgress) {
    EXPECT_NEAR(1.0, AutomixTransitionMath::midScoopGainForProgress(-0.5), 1e-9);
    EXPECT_NEAR(1.0, AutomixTransitionMath::midScoopGainForProgress(1.5), 1e-9);
}

// --- One-shot tempo match via rate_ratio (added 2026-10-05, "snap to grid
// destroys manual beatmatching" bugfix -- replaces the continuous
// sync_leader/sync_enabled approach, which forced ongoing phase correction
// as an inseparable side effect of tempo matching). Pure proportion math:
// newRatio = incomingRateRatio * (outgoingBpm / incomingBpm). ---

TEST_F(AutomixTransitionMathTest, TempoMatchedIncomingRateRatio_MatchesOutgoingBpm) {
    // Incoming deck at 130 BPM with no existing pitch adjustment (rate_ratio
    // 1.0); outgoing deck at 128 BPM. New ratio should make
    // incomingLocalBpm * newRatio == outgoingBpm, i.e. 130 * newRatio == 128.
    const double newRatio = AutomixTransitionMath::tempoMatchedIncomingRateRatio(
            /*outgoingBpm=*/128.0, /*incomingBpm=*/130.0, /*incomingRateRatio=*/1.0);
    EXPECT_NEAR(128.0, 130.0 * newRatio, 1e-9);
}

TEST_F(AutomixTransitionMathTest, TempoMatchedIncomingRateRatio_RespectsExistingPitchAdjustment) {
    // Incoming deck already pitched up by +4% (rate_ratio 1.04) and
    // currently reading 130 BPM effective. The new ratio must still land the
    // incoming deck's EFFECTIVE bpm on the outgoing deck's bpm, regardless of
    // whatever rate_ratio it started at.
    const double newRatio = AutomixTransitionMath::tempoMatchedIncomingRateRatio(
            /*outgoingBpm=*/120.0, /*incomingBpm=*/130.0, /*incomingRateRatio=*/1.04);
    // incomingLocalBpm = incomingBpm / incomingRateRatio.
    const double incomingLocalBpm = 130.0 / 1.04;
    EXPECT_NEAR(120.0, incomingLocalBpm * newRatio, 1e-9);
}

TEST_F(AutomixTransitionMathTest, TempoMatchedIncomingRateRatio_NoOpWhenAlreadyMatched) {
    // Both decks already at the same effective bpm with unity rate_ratio:
    // the new ratio should be (very close to) 1.0, not drift the deck.
    const double newRatio = AutomixTransitionMath::tempoMatchedIncomingRateRatio(
            128.0, 128.0, 1.0);
    EXPECT_NEAR(1.0, newRatio, 1e-9);
}

TEST_F(AutomixTransitionMathTest, TempoMatchedIncomingRateRatio_InvalidOutgoingBpmReturnsNegative) {
    EXPECT_LT(AutomixTransitionMath::tempoMatchedIncomingRateRatio(0.0, 130.0, 1.0), 0.0);
    EXPECT_LT(AutomixTransitionMath::tempoMatchedIncomingRateRatio(-5.0, 130.0, 1.0), 0.0);
}

TEST_F(AutomixTransitionMathTest, TempoMatchedIncomingRateRatio_InvalidIncomingBpmReturnsNegative) {
    EXPECT_LT(AutomixTransitionMath::tempoMatchedIncomingRateRatio(128.0, 0.0, 1.0), 0.0);
    EXPECT_LT(AutomixTransitionMath::tempoMatchedIncomingRateRatio(128.0, -5.0, 1.0), 0.0);
}

TEST_F(AutomixTransitionMathTest, TempoMatchedIncomingRateRatio_InvalidIncomingRateRatioReturnsNegative) {
    EXPECT_LT(AutomixTransitionMath::tempoMatchedIncomingRateRatio(128.0, 130.0, 0.0), 0.0);
    EXPECT_LT(AutomixTransitionMath::tempoMatchedIncomingRateRatio(128.0, 130.0, -1.0), 0.0);
}
