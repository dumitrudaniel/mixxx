#include "mixer/automixkeyguard.h"

#include <gtest/gtest.h>

#include <cmath>

// Mirrors brain/tests/test_automix_keyguard.py.

namespace {

using namespace mixxx::track::io::key;

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
    EXPECT_DOUBLE_EQ(2.0, plan.fadeBeats);
    EXPECT_DOUBLE_EQ(18.0, plan.endBeat());
}

TEST(AutomixKeyGuardTest, EqualPowerCrossfade) {
    const AutomixKeyGuardPlan plan = AutomixKeyGuardPlanner::plan(32.0, 2.0);
    EXPECT_DOUBLE_EQ(1.0, plan.outgoingGain(0.0));
    EXPECT_DOUBLE_EQ(1.0, plan.outgoingGain(16.0));
    EXPECT_DOUBLE_EQ(0.0, plan.incomingGain(15.9));
    EXPECT_NEAR(0.0, plan.outgoingGain(18.0), 1e-12);
    EXPECT_DOUBLE_EQ(1.0, plan.incomingGain(30.0));
    for (double beat = 16.0; beat <= 18.0; beat += 0.25) {
        const double out = plan.outgoingGain(beat);
        const double in = plan.incomingGain(beat);
        EXPECT_NEAR(1.0, out * out + in * in, 1e-12);
    }
}

TEST(AutomixKeyGuardTest, ZeroFadeIsAHardSwap) {
    const AutomixKeyGuardPlan plan = AutomixKeyGuardPlanner::plan(16.0, 0.0);
    EXPECT_DOUBLE_EQ(1.0, plan.outgoingGain(7.99));
    EXPECT_NEAR(0.0, plan.outgoingGain(8.0), 1e-12);
    EXPECT_DOUBLE_EQ(0.0, plan.incomingGain(7.99));
    EXPECT_DOUBLE_EQ(1.0, plan.incomingGain(8.0));
}

} // namespace
