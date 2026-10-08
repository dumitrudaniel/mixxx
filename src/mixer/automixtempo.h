#pragma once

#include <QString>
#include <optional>

#include "mixer/automixrecipe.h"

// DJ App live tempo: "meet in the middle, then return" (Dan's decision #6 in
// docs/plan-ui-integrare.md, docs/decisions/0027). The offline renders
// (brain/automix/mixrender.py, docs/decisions/0021) ramp the old track to the
// midpoint over the 4 bars BEFORE the transition; live, MIX is pressed shortly
// before the start bar, so that look-ahead does not exist. The live schedule:
//
//  1. Arm: the incoming deck takes the outgoing tempo a (the one-shot match,
//     docs/decisions/0008), so its quantized start lands in phase.
//  2. Transition beats 0 -> meetEndBeat (meet_bars, at most half the recipe:
//     Standard 8 -> beats 0..16, before the bass swap): BOTH decks glide
//     together from a to m = (a + b) / 2. Each deck's rate is its rate at the
//     transition start times the same factor, so their tempos stay equal at
//     every write and the phase lock made at the start is kept.
//  3. Until the end of the transition both play at m.
//  4. After the transition (the old track is out by 11/16 of it, beat 22 of
//     Standard 8, docs/decisions/0023) the incoming deck glides back to its own
//     tempo b (rate 1) over return_bars of ITS beats. An outgoing deck that is
//     still playing its track follows with the same factor, so the two stay
//     locked even if Dan kept the old track audible.
//
// Positions: transition beats (outgoing grid, 0 = the start bar) for steps
// 2-3, incoming beats since the return started for step 4. Both ramps are
// smoothstep (no tempo corner at either end). Pure math, unit tested in
// src/test/automixtempo_test.cpp, including a phase simulation of both decks.
struct AutomixTempoPlan {
    // a: the outgoing deck's effective tempo when the transition starts.
    double outgoingBpm = 0.0;
    // b: the incoming track's own tempo (its effective tempo at rate 1).
    double incomingBpm = 0.0;
    // m = (a + b) / 2.
    double meetBpm = 0.0;
    // Transition beat where both decks reach m (the ramp starts at beat 0).
    double meetEndBeat = 0.0;
    // Rate multiplier of both decks (relative to their rate at the transition
    // start) once they meet: m / a.
    double meetFactor = 1.0;
    // Multiplier at the end of the return: the incoming deck is back at rate 1
    // (1 / its rate at the transition start). Equal to meetFactor without a
    // return.
    double returnFactor = 1.0;
    // Incoming beats of the glide back; 0 = no return (stays at m).
    double returnBeats = 0.0;

    // Multiplier during the transition, at `transitionBeat`.
    double transitionFactor(double transitionBeat) const;
    // Multiplier during the return, `returnBeat` incoming beats after it began.
    double returnFactorAt(double returnBeat) const;
    bool hasReturn() const {
        return returnBeats > 0.0;
    }
};

class AutomixTempoPlanner {
  public:
    // Largest |a - b| / m the meet accepts (both decks move at most 6 %).
    // Beyond it the step is not a tempo step but a grid error (octave, 4:3) or
    // far over the planner's 5.2 BPM: the incoming deck keeps the plain
    // one-shot match instead, as before.
    static constexpr double kMaxMeetStep = 0.12;

    // The schedule for one transition, from both decks' state when it starts.
    // nullopt when no automated ramp runs (mode match_incoming or off, missing
    // BPM, step too large); *pWhyNot then says why, for mixxx.log.
    static std::optional<AutomixTempoPlan> plan(const AutomixTempo& tempo,
            double recipeLengthBeats,
            double outgoingBpm,
            double incomingBpm,
            double incomingRateRatio,
            QString* pWhyNot = nullptr);
};
