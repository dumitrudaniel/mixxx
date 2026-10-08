#include "mixer/automixkeyguard.h"

#include <gtest/gtest.h>

#include <cmath>

#include "mixer/automixvocalguard.h"

// Mirrors brain/tests/test_automix_keyguard.py.

namespace {

using namespace mixxx::track::io::key;

constexpr AutomixKeyGuardStem kBass = AutomixKeyGuardStem::Bass;
constexpr AutomixKeyGuardStem kMelody = AutomixKeyGuardStem::Melody;

// Vocal guard plan with the outgoing voice fading at `outgoingFadeStart`
// (phrase ended there naturally: the incoming voice follows after the fade).
AutomixVocalGuardPlan vocalPlan(double outgoingFadeStart, bool cut = false) {
    AutomixVocalGuardPlan plan;
    plan.outgoingFadeStart = outgoingFadeStart;
    plan.incomingFadeStart = cut ? outgoingFadeStart : outgoingFadeStart + 2.0;
    plan.fadeBeats = 2.0;
    plan.cutMidPhrase = cut;
    return plan;
}

TEST(AutomixKeyGuardTest, ClashingKeys) {
    // Li Que Mund -> Grace Evora: opposite sides of the wheel (11A / 5A).
    EXPECT_TRUE(AutomixKeyGuardPlanner::keysClash(F_SHARP_MINOR, C_MINOR));
    // Two steps ("energy boost"): the harmonies rub.
    EXPECT_TRUE(AutomixKeyGuardPlanner::keysClash(A_MINOR, B_MINOR));
    // Diagonal: one step and a mode change.
    EXPECT_TRUE(AutomixKeyGuardPlanner::keysClash(A_MINOR, G_MAJOR));
}

TEST(AutomixKeyGuardTest, FittingKeys) {
    // Nossa melodia -> Caso de Amor: neighbours (12A / 1A), across the wrap.
    EXPECT_FALSE(AutomixKeyGuardPlanner::keysClash(C_SHARP_MINOR, G_SHARP_MINOR));
    EXPECT_FALSE(AutomixKeyGuardPlanner::keysClash(A_MINOR, A_MINOR));
    EXPECT_FALSE(AutomixKeyGuardPlanner::keysClash(A_MINOR, C_MAJOR)); // relative
    EXPECT_FALSE(AutomixKeyGuardPlanner::keysClash(C_MAJOR, F_MAJOR)); // one step
    EXPECT_FALSE(AutomixKeyGuardPlanner::keysClash(B_MAJOR, F_SHARP_MAJOR));
}

TEST(AutomixKeyGuardTest, UnknownKeyNeverStartsTheGuard) {
    EXPECT_FALSE(AutomixKeyGuardPlanner::keysClash(INVALID, C_MINOR));
    EXPECT_FALSE(AutomixKeyGuardPlanner::keysClash(F_SHARP_MINOR, INVALID));
}

TEST(AutomixKeyGuardTest, SwapAtHalfTheRecipe) {
    const AutomixKeyGuardPlan plan = AutomixKeyGuardPlanner::plan(32.0, 2.0);
    EXPECT_DOUBLE_EQ(16.0, plan.swapBeat);
    // Without the vocal guard the melody swaps with the bass.
    EXPECT_DOUBLE_EQ(16.0, plan.melodySwapBeat);
    EXPECT_FALSE(plan.earlyMelody());
    EXPECT_DOUBLE_EQ(2.0, plan.fadeBeats);
    EXPECT_DOUBLE_EQ(18.0, plan.endBeat());
}

TEST(AutomixKeyGuardTest, EqualPowerCrossfade) {
    const AutomixKeyGuardPlan plan = AutomixKeyGuardPlanner::plan(32.0, 2.0);
    for (const AutomixKeyGuardStem stem : {kBass, kMelody}) {
        EXPECT_DOUBLE_EQ(1.0, plan.outgoingGain(stem, 0.0));
        EXPECT_DOUBLE_EQ(1.0, plan.outgoingGain(stem, 16.0));
        EXPECT_DOUBLE_EQ(0.0, plan.incomingGain(stem, 15.9));
        EXPECT_NEAR(0.0, plan.outgoingGain(stem, 18.0), 1e-12);
        EXPECT_DOUBLE_EQ(1.0, plan.incomingGain(stem, 30.0));
        for (double beat = 16.0; beat <= 18.0; beat += 0.25) {
            const double out = plan.outgoingGain(stem, beat);
            const double in = plan.incomingGain(stem, beat);
            EXPECT_NEAR(1.0, out * out + in * in, 1e-12);
        }
    }
}

TEST(AutomixKeyGuardTest, ZeroFadeIsAHardSwap) {
    const AutomixKeyGuardPlan plan = AutomixKeyGuardPlanner::plan(16.0, 0.0);
    EXPECT_DOUBLE_EQ(1.0, plan.outgoingGain(kBass, 7.99));
    EXPECT_NEAR(0.0, plan.outgoingGain(kBass, 8.0), 1e-12);
    EXPECT_DOUBLE_EQ(0.0, plan.incomingGain(kBass, 7.99));
    EXPECT_DOUBLE_EQ(1.0, plan.incomingGain(kBass, 8.0));
}

TEST(AutomixKeyGuardTest, MelodySwapsAtTheVocalHandover) {
    // Preto Show -> Gata Morena (party mix v4): the old voice ends at beat 8.
    const AutomixVocalGuardPlan vocal = vocalPlan(8.0);
    const AutomixKeyGuardPlan plan = AutomixKeyGuardPlanner::plan(32.0, 2.0, &vocal, 0.0);
    EXPECT_DOUBLE_EQ(8.0, plan.melodySwapBeat);
    EXPECT_DOUBLE_EQ(16.0, plan.swapBeat);
    EXPECT_TRUE(plan.earlyMelody());
    EXPECT_DOUBLE_EQ(18.0, plan.endBeat());
    // Melody: old out and new in over beats 8..10, equal-power.
    EXPECT_DOUBLE_EQ(0.0, plan.incomingGain(kMelody, 7.99));
    EXPECT_NEAR(1.0, plan.incomingGain(kMelody, 10.0), 1e-12);
    EXPECT_NEAR(0.0, plan.outgoingGain(kMelody, 10.0), 1e-12);
    const double out = plan.outgoingGain(kMelody, 9.0);
    const double in = plan.incomingGain(kMelody, 9.0);
    EXPECT_NEAR(1.0, out * out + in * in, 1e-12);
    // Bass: still at half the recipe (the EQ keeps the incoming bass killed
    // until then); the old bass plays under the new melody.
    EXPECT_DOUBLE_EQ(1.0, plan.outgoingGain(kBass, 12.0));
    EXPECT_DOUBLE_EQ(0.0, plan.incomingGain(kBass, 12.0));
    EXPECT_NEAR(1.0, plan.incomingGain(kBass, 18.0), 1e-12);
}

TEST(AutomixKeyGuardTest, MelodySwapSnapsUpToTheBar) {
    // Handover mid-bar: the old instruments finish the bar, the new melody
    // comes in on the next downbeat.
    const AutomixVocalGuardPlan midBar = vocalPlan(9.0);
    EXPECT_DOUBLE_EQ(12.0, AutomixKeyGuardPlanner::plan(32.0, 2.0, &midBar).melodySwapBeat);
    // Grid conversion noise just past a bar line stays on that bar.
    const AutomixVocalGuardPlan noisy = vocalPlan(8.1);
    EXPECT_DOUBLE_EQ(8.0, AutomixKeyGuardPlanner::plan(32.0, 2.0, &noisy).melodySwapBeat);
    // Snapping past the bass swap means no early melody.
    const AutomixVocalGuardPlan late = vocalPlan(13.0);
    const AutomixKeyGuardPlan plan = AutomixKeyGuardPlanner::plan(32.0, 2.0, &late);
    EXPECT_DOUBLE_EQ(16.0, plan.melodySwapBeat);
    EXPECT_FALSE(plan.earlyMelody());
}

TEST(AutomixKeyGuardTest, AtLeastOneBarOfDrumsFirst) {
    // The old voice is not singing at the start (fades at 0): the new melody
    // still waits one bar after the incoming starts playing.
    const AutomixVocalGuardPlan silent = vocalPlan(0.0);
    EXPECT_DOUBLE_EQ(4.0, AutomixKeyGuardPlanner::plan(32.0, 2.0, &silent, 0.0).melodySwapBeat);
    // Incoming started at beat 6: one bar later is 10, the next bar line 12.
    EXPECT_DOUBLE_EQ(12.0, AutomixKeyGuardPlanner::plan(32.0, 2.0, &silent, 6.0).melodySwapBeat);
    // Urgenta 2: the bass swap (beat 4) comes first, nothing changes.
    EXPECT_FALSE(AutomixKeyGuardPlanner::plan(8.0, 2.0, &silent, 0.0).earlyMelody());
}

TEST(AutomixKeyGuardTest, VoiceToTheSwapKeepsEverythingAtTheSwap) {
    // The old voice sings up to (or past) half the recipe: the vocal guard
    // cuts it there, everything swaps at 16 as before.
    const AutomixVocalGuardPlan cut = vocalPlan(16.0, true);
    const AutomixKeyGuardPlan plan = AutomixKeyGuardPlanner::plan(32.0, 2.0, &cut);
    EXPECT_DOUBLE_EQ(16.0, plan.melodySwapBeat);
    EXPECT_FALSE(plan.earlyMelody());
    // No vocal plan (guard off, an instrumental, no stems or maps): at 16.
    EXPECT_FALSE(AutomixKeyGuardPlanner::plan(32.0, 2.0, nullptr, 0.0).earlyMelody());
}

} // namespace
