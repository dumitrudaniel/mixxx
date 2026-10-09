#include "library/djapp/djappautopilotlogic.h"

#include <gtest/gtest.h>

// DJ App automix autopilot (docs/plan-ui-integrare.md §6 Etapa 5, cut-down
// live test, 2026-10-09): pure decision logic only. The live driver
// (library/djapp/djappautopilot.h) wires this to ControlObjects, PlayerInfo
// and brain.db, and is exercised by hand (no live Mixxx instance in these
// tests).

using namespace djapp::autopilot;

namespace {

TEST(DJAppAutopilotLogicTest, ParsesMixOutJson) {
    const QMap<int, double> map = parseMixOutMap(
            QStringLiteral("{\"8\": 12.3, \"16\": 34.5, \"32\": 56.7}"));
    EXPECT_EQ(map.size(), 3);
    EXPECT_DOUBLE_EQ(map.value(8), 12.3);
    EXPECT_DOUBLE_EQ(map.value(16), 34.5);
    EXPECT_DOUBLE_EQ(map.value(32), 56.7);
}

TEST(DJAppAutopilotLogicTest, EmptyOrMalformedJsonGivesEmptyMap) {
    EXPECT_TRUE(parseMixOutMap(QString()).isEmpty());
    EXPECT_TRUE(parseMixOutMap(QStringLiteral("not json")).isEmpty());
    EXPECT_TRUE(parseMixOutMap(QStringLiteral("[1,2,3]")).isEmpty());
}

TEST(DJAppAutopilotLogicTest, NearestMixOutPicksClosestLength) {
    QMap<int, double> map;
    map.insert(8, 10.0);
    map.insert(16, 20.0);
    map.insert(32, 40.0);
    map.insert(64, 80.0);
    EXPECT_DOUBLE_EQ(nearestMixOut(map, 32.0).value(), 40.0);
    EXPECT_DOUBLE_EQ(nearestMixOut(map, 30.0).value(), 40.0); // closer to 32 than 16
    EXPECT_DOUBLE_EQ(nearestMixOut(map, 20.0).value(), 20.0);
    EXPECT_DOUBLE_EQ(nearestMixOut(map, 100.0).value(), 80.0); // clamps to the largest
    EXPECT_FALSE(nearestMixOut({}, 32.0).has_value());
}

Inputs baseInputs() {
    Inputs in;
    in.enabled = true;
    in.engineIdle = true;
    in.deck1Playing = true;
    in.deck2Playing = false;
    in.deck1Loaded = true;
    in.deck2Loaded = false;
    in.deck1PositionSec = 310.0;
    in.deck2PositionSec = 0.0;
    in.deck1MixOutSec = 300.0; // already past it
    in.deck2MixOutSec = std::nullopt;
    return in;
}

TEST(DJAppAutopilotLogicTest, OffDoesNothing) {
    Inputs in = baseInputs();
    in.enabled = false;
    EXPECT_EQ(decide(in).action, Action::None);
}

TEST(DJAppAutopilotLogicTest, WaitsWhileEngineNotIdle) {
    Inputs in = baseInputs();
    in.engineIdle = false; // armed or running, by Dan or a previous trigger
    EXPECT_EQ(decide(in).action, Action::None);
}

TEST(DJAppAutopilotLogicTest, WaitsWhenNeitherOrBothDecksPlay) {
    Inputs in = baseInputs();
    in.deck1Playing = false;
    in.deck2Playing = false;
    EXPECT_EQ(decide(in).action, Action::None);

    in.deck1Playing = true;
    in.deck2Playing = true;
    EXPECT_EQ(decide(in).action, Action::None);
}

TEST(DJAppAutopilotLogicTest, WaitsBeforeMixOutPoint) {
    Inputs in = baseInputs();
    in.deck1PositionSec = 10.0; // mix-out is at 300.0, window opens at 180.0
    EXPECT_EQ(decide(in).action, Action::None);
}

TEST(DJAppAutopilotLogicTest, WaitsWithoutMixOutData) {
    Inputs in = baseInputs();
    in.deck1MixOutSec = std::nullopt;
    EXPECT_EQ(decide(in).action, Action::None);
}

TEST(DJAppAutopilotLogicTest, PicksAndLoadsWhenOtherDeckIsEmpty) {
    Inputs in = baseInputs();
    const Decision d = decide(in);
    EXPECT_EQ(d.action, Action::PickAndLoad);
    EXPECT_EQ(d.playingDeckNumber, 1);
    EXPECT_EQ(d.otherDeckNumber, 2);
}

TEST(DJAppAutopilotLogicTest, JustTriggersWhenOtherDeckAlreadyHasATrack) {
    Inputs in = baseInputs();
    in.deck2Loaded = true; // Dan loaded one himself, or the autopilot already did
    const Decision d = decide(in);
    EXPECT_EQ(d.action, Action::Trigger);
    EXPECT_EQ(d.playingDeckNumber, 1);
    EXPECT_EQ(d.otherDeckNumber, 2);
}

TEST(DJAppAutopilotLogicTest, OverrideEmptyDeckTreatsAStaleTrackAsEmpty) {
    Inputs in = baseInputs();
    in.deck2Loaded = true; // the old track from a transition that just finished
    in.overrideEmptyDeck = 2;
    const Decision d = decide(in);
    EXPECT_EQ(d.action, Action::PickAndLoad);
    EXPECT_EQ(d.otherDeckNumber, 2);
}

TEST(DJAppAutopilotLogicTest, EntersChoosingInsideLookaheadWindow) {
    Inputs in = baseInputs();
    in.deck2Loaded = false; // other deck still free
    // mix-out is at 300.0, lookahead is 120s -> window opens at 180.0.
    in.deck1PositionSec = 200.0;
    const Decision d = decide(in);
    EXPECT_EQ(d.action, Action::ShowCandidates);
    EXPECT_EQ(d.playingDeckNumber, 1);
    EXPECT_EQ(d.otherDeckNumber, 2);
}

TEST(DJAppAutopilotLogicTest, StaysWatchingRightBeforeTheLookaheadWindow) {
    Inputs in = baseInputs();
    in.deck1PositionSec = 179.9; // window opens at 180.0
    EXPECT_EQ(decide(in).action, Action::None);
}

TEST(DJAppAutopilotLogicTest, ChoosingWindowOpensExactlyAtTheThreshold) {
    Inputs in = baseInputs();
    in.deck1PositionSec = 180.0; // mixOutSec(300) - kLookaheadSec(120)
    EXPECT_EQ(decide(in).action, Action::ShowCandidates);
}

TEST(DJAppAutopilotLogicTest, PickerStaysUpInsideTheWindowEvenAfterAPick) {
    // Dan: "nu vreau sa dispara lista, poate ma razgandesc" - a track on the
    // free deck (his own, a click, or the autopilot's own fallback) does not
    // close the picker before the deadline; he can still click a different
    // row to change his mind. showCandidates() itself (not pure decide())
    // avoids requerying brain.db for the same source+deck.
    Inputs in = baseInputs();
    in.deck1PositionSec = 200.0; // inside the window, before the deadline
    in.deck2Loaded = true; // Dan already put a track there himself
    const Decision d = decide(in);
    EXPECT_EQ(d.action, Action::ShowCandidates);
    EXPECT_EQ(d.otherDeckNumber, 2);
}

TEST(DJAppAutopilotLogicTest, OverrideEmptyDeckAlsoAppliesInsideTheWindow) {
    Inputs in = baseInputs();
    in.deck1PositionSec = 200.0;
    in.deck2Loaded = true; // stale track from a transition that just finished
    in.overrideEmptyDeck = 2;
    const Decision d = decide(in);
    EXPECT_EQ(d.action, Action::ShowCandidates);
    EXPECT_EQ(d.otherDeckNumber, 2);
}

TEST(DJAppAutopilotLogicTest, DeadlineReachedWithNoChoiceAutoPicksTop) {
    Inputs in = baseInputs();
    in.deck1PositionSec = 300.0; // exactly at mixOutSec: the deadline
    in.deck2Loaded = false; // Dan never clicked a candidate
    const Decision d = decide(in);
    EXPECT_EQ(d.action, Action::PickAndLoad);
    EXPECT_EQ(d.otherDeckNumber, 2);
}

TEST(DJAppAutopilotLogicTest, PlanCandidatesTakesAllowedFirstThenFillsWithRisky) {
    // Plenty of both: capped at maxCandidates, allowed preferred.
    CandidatePlan plan = planCandidates(5, 5, 3);
    EXPECT_EQ(plan.allowedCount, 3);
    EXPECT_EQ(plan.riskyCount, 0);

    // Fewer than 3 allowed: risky fills the remainder.
    plan = planCandidates(1, 5, 3);
    EXPECT_EQ(plan.allowedCount, 1);
    EXPECT_EQ(plan.riskyCount, 2);

    // No allowed at all: risky fallback fills every slot it can.
    plan = planCandidates(0, 2, 3);
    EXPECT_EQ(plan.allowedCount, 0);
    EXPECT_EQ(plan.riskyCount, 2);

    // Nothing at all: the true dead end.
    plan = planCandidates(0, 0, 3);
    EXPECT_EQ(plan.allowedCount, 0);
    EXPECT_EQ(plan.riskyCount, 0);
}

TEST(DJAppAutopilotLogicTest, IsRiskyFallbackOnlyWhenAllowedIsEmptyButRiskyIsNot) {
    EXPECT_FALSE(isRiskyFallback(3, 5)); // allowed available: not a fallback
    EXPECT_FALSE(isRiskyFallback(1, 0)); // allowed available, no risky either
    EXPECT_TRUE(isRiskyFallback(0, 1)); // Morango do Nordeste's real case tonight
    EXPECT_TRUE(isRiskyFallback(0, 15));
    EXPECT_FALSE(isRiskyFallback(0, 0)); // the true dead end, not a fallback
}

TEST(DJAppAutopilotLogicTest, SymmetricForDeck2Playing) {
    Inputs in;
    in.enabled = true;
    in.engineIdle = true;
    in.deck1Playing = false;
    in.deck2Playing = true;
    in.deck1Loaded = false;
    in.deck2Loaded = true;
    in.deck1PositionSec = 0.0;
    in.deck2PositionSec = 50.0;
    in.deck2MixOutSec = 40.0;
    const Decision d = decide(in);
    EXPECT_EQ(d.action, Action::PickAndLoad);
    EXPECT_EQ(d.playingDeckNumber, 2);
    EXPECT_EQ(d.otherDeckNumber, 1);
}

TEST(DJAppAutopilotLogicTest, StatusTextIsRomanianAndReadable) {
    EXPECT_EQ(statusText(Status{StatusKind::Off, 0, 0.0, QString()}), QStringLiteral("oprit"));

    const QString watching = statusText(Status{StatusKind::Watching, 1, 192.0, QString()});
    EXPECT_TRUE(watching.contains(QStringLiteral("deck 1")));
    EXPECT_TRUE(watching.contains(QStringLiteral("3:12")));

    const QString picked =
            statusText(Status{StatusKind::Picked, 1, 0.0, QStringLiteral("Livongh")});
    EXPECT_TRUE(picked.contains(QStringLiteral("Livongh")));

    // Change 1: a risky fallback pick is flagged, not silently used.
    const QString riskyPicked = statusText(
            Status{StatusKind::Picked, 1, 0.0, QStringLiteral("Morango do Nordeste"), true});
    EXPECT_TRUE(riskyPicked.contains(QStringLiteral("Morango do Nordeste")));
    EXPECT_TRUE(riskyPicked.contains(QStringLiteral("riscant")));

    const QString choosing = statusText(Status{StatusKind::Choosing, 2, 125.0, QString()});
    EXPECT_TRUE(choosing.contains(QStringLiteral("deck 2")));
    EXPECT_TRUE(choosing.contains(QStringLiteral("2:05")));
}

} // namespace
