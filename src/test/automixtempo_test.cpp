#include "mixer/automixtempo.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <iostream>
#include <utility>

// Live tempo "meet in the middle, then return" (docs/decisions/0027).

namespace {

AutomixTempo meetReturn(double meetBars = 4.0, double returnBars = 8.0) {
    AutomixTempo tempo;
    tempo.mode = AutomixTempoMode::MeetReturn;
    tempo.meetBeats = meetBars * 4.0;
    tempo.returnBeats = returnBars * 4.0;
    return tempo;
}

// Outgoing at 100 BPM, incoming track at 105 BPM already matched to it at arm
// time (rate 100/105), Standard 8.
AutomixTempoPlan standardPlan() {
    const auto plan = AutomixTempoPlanner::plan(meetReturn(), 32.0, 100.0, 100.0, 100.0 / 105.0);
    EXPECT_TRUE(plan.has_value());
    return plan.value_or(AutomixTempoPlan());
}

// Two decks on constant grids, driven like Mixxx drives them: the engine
// advances both decks once per audio buffer with the rate in effect at the
// buffer start; the GUI tick (every 20 ms) reads the outgoing position as of
// the last buffer (VisualPlayPosition) and writes both rate_ratio values. A
// write takes effect from the next buffer on. `incomingWriteLate` puts every
// incoming write one buffer later than the outgoing one: the worst case of
// an engine callback landing between the two writes of a tick, every tick.
struct PhaseRun {
    double maxErrorMs = 0.0;
    double outgoingBpmAtMeetEnd = 0.0;
    double incomingBpmAtMeetEnd = 0.0;
    double outgoingBpmAtEnd = 0.0;
    double incomingBpmAtEnd = 0.0;
    double incomingRateAtEnd = 0.0;
};

struct SimDeck {
    double fileBpm = 0.0;
    double rate = 1.0;
    double beat = 0.0;
    // Writes not yet seen by the engine: (first buffer that uses it, value).
    std::deque<std::pair<long, double>> pending;

    double bpm() const {
        return fileBpm * rate;
    }
    void write(double value, long fromBuffer) {
        pending.emplace_back(fromBuffer, value);
    }
    // The engine reads the control once per callback: the newest write that
    // has reached it.
    void applyWritesFor(long buffer) {
        while (!pending.empty() && pending.front().first <= buffer) {
            rate = pending.front().second;
            pending.pop_front();
        }
    }
};

PhaseRun simulatePhase(const AutomixTempoPlan& plan,
        double lengthBeats,
        double outgoingFileBpm,
        double outgoingRate,
        double incomingFileBpm,
        double incomingRate,
        bool incomingWriteLate) {
    constexpr double kBufferSec = 1024.0 / 44100.0;
    constexpr double kTickSec = 0.020;
    SimDeck outgoing{outgoingFileBpm, outgoingRate};
    SimDeck incoming{incomingFileBpm, incomingRate};
    PhaseRun run;
    long buffer = 0;
    double now = 0.0;
    double nextTick = 0.0;
    bool returning = false;
    double returnStartBeat = 0.0;
    bool meetEndSeen = false;
    // The transition starts with both decks in phase on beat 0.
    while (buffer < 200000) {
        for (SimDeck* pDeck : {&outgoing, &incoming}) {
            pDeck->applyWritesFor(buffer);
            pDeck->beat += pDeck->bpm() / 60.0 * kBufferSec;
        }
        ++buffer;
        now += kBufferSec;
        const double diff = incoming.beat - outgoing.beat;
        run.maxErrorMs = std::max(run.maxErrorMs, std::abs(diff) * 60000.0 / outgoing.bpm());
        if (!meetEndSeen && outgoing.beat >= plan.meetEndBeat + 1.0) {
            meetEndSeen = true;
            run.outgoingBpmAtMeetEnd = outgoing.bpm();
            run.incomingBpmAtMeetEnd = incoming.bpm();
        }

        while (nextTick <= now) {
            nextTick += kTickSec;
            double factor = 0.0;
            if (!returning && outgoing.beat >= lengthBeats) {
                returning = true;
                returnStartBeat = incoming.beat;
            }
            if (!returning) {
                factor = plan.transitionFactor(outgoing.beat);
            } else {
                const double returnBeat = incoming.beat - returnStartBeat;
                if (returnBeat >= plan.returnBeats + 4.0) {
                    run.outgoingBpmAtEnd = outgoing.bpm();
                    run.incomingBpmAtEnd = incoming.bpm();
                    run.incomingRateAtEnd = incoming.rate;
                    std::cout << "[ phase    ] max beatmatch error " << run.maxErrorMs
                              << " ms" << (incomingWriteLate ? " (late writes)" : "")
                              << std::endl;
                    return run;
                }
                factor = plan.returnFactorAt(returnBeat);
            }
            outgoing.write(outgoingRate * factor, buffer);
            incoming.write(incomingRate * factor, buffer + (incomingWriteLate ? 1 : 0));
        }
    }
    ADD_FAILURE() << "simulation did not finish";
    return run;
}

} // namespace

TEST(AutomixTempoTest, MeetsInTheMiddleThenReturnsToItsOwnTempo) {
    const AutomixTempoPlan plan = standardPlan();
    EXPECT_DOUBLE_EQ(100.0, plan.outgoingBpm);
    EXPECT_NEAR(105.0, plan.incomingBpm, 1e-9);
    EXPECT_NEAR(102.5, plan.meetBpm, 1e-9);
    // Both decks meet by the bass swap of Standard 8 (beat 16).
    EXPECT_DOUBLE_EQ(16.0, plan.meetEndBeat);
    EXPECT_NEAR(1.025, plan.meetFactor, 1e-12);
    // Incoming rate at the end = (100/105) * 1.05 = 1: its own tempo.
    EXPECT_NEAR(1.05, plan.returnFactor, 1e-12);
    EXPECT_TRUE(plan.hasReturn());
    EXPECT_DOUBLE_EQ(32.0, plan.returnBeats);
}

TEST(AutomixTempoTest, MeetRampIsSmoothAndEndsOnTheMeetTempo) {
    const AutomixTempoPlan plan = standardPlan();
    EXPECT_DOUBLE_EQ(1.0, plan.transitionFactor(-2.0));
    EXPECT_DOUBLE_EQ(1.0, plan.transitionFactor(0.0));
    EXPECT_NEAR(1.0125, plan.transitionFactor(8.0), 1e-12);
    EXPECT_NEAR(plan.meetFactor, plan.transitionFactor(16.0), 1e-12);
    EXPECT_NEAR(plan.meetFactor, plan.transitionFactor(30.0), 1e-12);
    // Smoothstep: no tempo corner where the ramp starts or ends.
    EXPECT_LT(plan.transitionFactor(0.1) - 1.0, 1e-4);
    EXPECT_LT(plan.meetFactor - plan.transitionFactor(15.9), 1e-4);
    double previous = 0.0;
    for (int i = 0; i <= 64; ++i) {
        const double factor = plan.transitionFactor(i * 0.25);
        EXPECT_GE(factor, previous) << i;
        previous = factor;
    }
}

TEST(AutomixTempoTest, ReturnRampGoesFromTheMeetToTheOwnTempo) {
    const AutomixTempoPlan plan = standardPlan();
    EXPECT_NEAR(plan.meetFactor, plan.returnFactorAt(0.0), 1e-12);
    EXPECT_NEAR((plan.meetFactor + plan.returnFactor) / 2.0, plan.returnFactorAt(16.0), 1e-12);
    EXPECT_NEAR(plan.returnFactor, plan.returnFactorAt(32.0), 1e-12);
    EXPECT_NEAR(plan.returnFactor, plan.returnFactorAt(99.0), 1e-12);
    // The incoming deck (rate 100/105 at the start) lands on rate 1.
    EXPECT_NEAR(1.0, 100.0 / 105.0 * plan.returnFactorAt(32.0), 1e-12);
}

TEST(AutomixTempoTest, MeetEndsByHalfTheRecipe) {
    // Urgenta 2, Scurt 4, Standard 8, Lung 16 with the default meet_bars 4.
    const std::pair<double, double> cases[] = {{8.0, 4.0}, {16.0, 8.0}, {32.0, 16.0}, {64.0, 16.0}};
    for (const auto& [lengthBeats, meetEnd] : cases) {
        const auto plan = AutomixTempoPlanner::plan(meetReturn(), lengthBeats, 100.0, 100.0, 0.98);
        ASSERT_TRUE(plan.has_value());
        EXPECT_DOUBLE_EQ(meetEnd, plan->meetEndBeat) << lengthBeats;
    }
    const auto shortMeet = AutomixTempoPlanner::plan(meetReturn(1.0), 32.0, 100.0, 100.0, 0.98);
    ASSERT_TRUE(shortMeet.has_value());
    EXPECT_DOUBLE_EQ(4.0, shortMeet->meetEndBeat);
}

TEST(AutomixTempoTest, DescendingStepSlowsBothDecks) {
    // Outgoing 105, incoming 100 matched to it (rate 1.05).
    const auto plan = AutomixTempoPlanner::plan(meetReturn(), 32.0, 105.0, 105.0, 1.05);
    ASSERT_TRUE(plan.has_value());
    EXPECT_NEAR(100.0, plan->incomingBpm, 1e-9);
    EXPECT_NEAR(102.5, plan->meetBpm, 1e-9);
    EXPECT_LT(plan->meetFactor, 1.0);
    EXPECT_NEAR(1.0, 1.05 * plan->returnFactor, 1e-12);
}

TEST(AutomixTempoTest, UnmatchedIncomingStillEndsAtItsOwnTempo) {
    // Dan left the incoming deck at rate 1 (105 BPM): the meet uses its own
    // tempo, the return lands it on rate 1 again.
    const auto plan = AutomixTempoPlanner::plan(meetReturn(), 32.0, 100.0, 105.0, 1.0);
    ASSERT_TRUE(plan.has_value());
    EXPECT_NEAR(102.5, plan->meetBpm, 1e-9);
    EXPECT_NEAR(1.0, plan->returnFactor, 1e-12);
}

TEST(AutomixTempoTest, ReturnBarsZeroStaysAtTheMeetTempo) {
    const auto plan = AutomixTempoPlanner::plan(meetReturn(4.0, 0.0), 32.0, 100.0, 100.0, 100.0 / 105.0);
    ASSERT_TRUE(plan.has_value());
    EXPECT_FALSE(plan->hasReturn());
    EXPECT_DOUBLE_EQ(plan->meetFactor, plan->returnFactor);
    EXPECT_DOUBLE_EQ(plan->meetFactor, plan->returnFactorAt(10.0));
}

TEST(AutomixTempoTest, OtherModesHaveNoRamp) {
    QString why;
    AutomixTempo tempo;
    tempo.mode = AutomixTempoMode::MatchIncoming;
    EXPECT_FALSE(AutomixTempoPlanner::plan(tempo, 32.0, 100.0, 100.0, 0.95, &why).has_value());
    EXPECT_TRUE(why.contains(QStringLiteral("match_incoming")));
    tempo.mode = AutomixTempoMode::Off;
    EXPECT_FALSE(AutomixTempoPlanner::plan(tempo, 32.0, 100.0, 100.0, 0.95, &why).has_value());
    EXPECT_TRUE(why.contains(QStringLiteral("off")));
}

TEST(AutomixTempoTest, GridErrorsAreNotMetInTheMiddle) {
    QString why;
    // 85 vs 170 (octave): never ramp both decks by a third.
    EXPECT_FALSE(AutomixTempoPlanner::plan(meetReturn(), 32.0, 85.0, 85.0, 0.5, &why).has_value());
    EXPECT_TRUE(why.contains(QStringLiteral("incoming matched only")));
    // 4:3 (89 vs 118.7).
    EXPECT_FALSE(AutomixTempoPlanner::plan(meetReturn(), 32.0, 89.0, 89.0, 89.0 / 118.7)
                         .has_value());
    // A 10 % step is still a tempo step.
    EXPECT_TRUE(AutomixTempoPlanner::plan(meetReturn(), 32.0, 100.0, 100.0, 100.0 / 110.0)
                        .has_value());
    // Missing BPM.
    EXPECT_FALSE(AutomixTempoPlanner::plan(meetReturn(), 32.0, 0.0, 100.0, 1.0).has_value());
    EXPECT_FALSE(AutomixTempoPlanner::plan(meetReturn(), 32.0, 100.0, 100.0, 0.0).has_value());
}

TEST(AutomixTempoTest, BothDecksStayPhaseLockedThroughEveryRamp) {
    const AutomixTempoPlan plan = standardPlan();
    const PhaseRun run = simulatePhase(plan, 32.0, 100.0, 1.0, 105.0, 100.0 / 105.0, false);
    // Same factor written to both decks in the same tick: no drift at all.
    EXPECT_LT(run.maxErrorMs, 0.01);
    // Met by the bass swap: both at 102.5 from transition beat 16 on.
    EXPECT_NEAR(102.5, run.outgoingBpmAtMeetEnd, 1e-9);
    EXPECT_NEAR(102.5, run.incomingBpmAtMeetEnd, 1e-9);
    // Back at the incoming track's own tempo, the old deck still locked to it.
    EXPECT_NEAR(105.0, run.incomingBpmAtEnd, 1e-9);
    EXPECT_NEAR(105.0, run.outgoingBpmAtEnd, 1e-9);
    EXPECT_NEAR(1.0, run.incomingRateAtEnd, 1e-12);
}

TEST(AutomixTempoTest, PhaseErrorStaysAroundOneMsWhenEveryWriteStraddlesACallback) {
    // Worst case: every incoming write lands one 23 ms buffer after the
    // outgoing one. The error is bounded by the total factor change times
    // one buffer (2 x 2.5 % x 23 ms), far inside the few-ms beatmatch.
    const AutomixTempoPlan plan = standardPlan();
    const PhaseRun run = simulatePhase(plan, 32.0, 100.0, 1.0, 105.0, 100.0 / 105.0, true);
    EXPECT_LT(run.maxErrorMs, 1.5);
    EXPECT_NEAR(105.0, run.incomingBpmAtEnd, 1e-9);
}

TEST(AutomixTempoTest, FastUrgentaRampStaysLocked) {
    // Urgenta 2: the whole 2.5 % meet within one bar.
    const auto plan = AutomixTempoPlanner::plan(meetReturn(), 8.0, 100.0, 100.0, 100.0 / 105.0);
    ASSERT_TRUE(plan.has_value());
    const PhaseRun run = simulatePhase(*plan, 8.0, 100.0, 1.0, 105.0, 100.0 / 105.0, true);
    EXPECT_LT(run.maxErrorMs, 1.5);
    EXPECT_NEAR(102.5, run.outgoingBpmAtMeetEnd, 1e-9);
    EXPECT_NEAR(102.5, run.incomingBpmAtMeetEnd, 1e-9);
}
