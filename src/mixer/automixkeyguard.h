#pragma once

#include "proto/keys.pb.h"

struct AutomixVocalGuardPlan;

// DJ App key guard, "doar tobe peste" (Dan, 2026-10-08, docs/decisions/0022).
// C++ port of brain/automix/keyguard.py, used live by the automix executor
// (mixer/automixtransitioncontroller.h) on Mixxx 2.6 stem tracks:
//
//  1. Keys that fit (same, neighbours on the Camelot wheel, relative
//     major/minor): nothing happens.
//  2. Keys that clash: the incoming harmonic stems (bass, other, vocals) stay
//     muted and the outgoing ones keep playing until they swap, crossing over
//     in fadeBeats, equal-power like the band swaps. Drums are never touched.
//     - Bass: at half the recipe (swapBeat), where the EQ lanes swap bass
//       and highs. The recipe keeps the incoming bass killed until then.
//     - Melody (other + vocals): at the same beat, unless the vocal guard is
//       active and the outgoing voice hands over before it (revision
//       2026-10-08, Preto Show -> Gata Morena: "the melody comes in too
//       late"). Then the melody swaps where the outgoing voice starts to fade,
//       snapped up to the next bar line, and never before one bar of incoming
//       drums alone (kLeadInBeats after the incoming starts playing). In
//       between the outgoing bass plays under the incoming melody.
//  3. With the vocal guard on as well, the vocals stem takes the smaller of
//     the two gains: the old voice still fades at its phrase end, the new one
//     never comes in before the melody swap.
//
// Positions are in TRANSITION beats (0 = the bar the transition starts on).
// Pure math, unit tested in src/test/automixkeyguard_test.cpp.

// Which key guard swap a harmonic stem follows.
enum class AutomixKeyGuardStem {
    Bass,
    // "other" and "vocals".
    Melody,
};

struct AutomixKeyGuardPlan {
    // Bass: outgoing full until here, then cos fade over fadeBeats; incoming
    // muted until here, then sin fade over fadeBeats.
    double swapBeat = 0.0;
    // Other + vocals: same curves from here. Equal to swapBeat unless the
    // outgoing voice handed over earlier (see AutomixKeyGuardPlanner::plan).
    double melodySwapBeat = 0.0;
    double fadeBeats = 0.0;

    double swapBeatOf(AutomixKeyGuardStem stem) const {
        return stem == AutomixKeyGuardStem::Bass ? swapBeat : melodySwapBeat;
    }
    double outgoingGain(AutomixKeyGuardStem stem, double beat) const;
    double incomingGain(AutomixKeyGuardStem stem, double beat) const;
    // The melody swaps before the bass.
    bool earlyMelody() const {
        return melodySwapBeat < swapBeat;
    }
    // Transition beat from which every gain is final (the bass swap is the
    // later one).
    double endBeat() const {
        return swapBeat + fadeBeats;
    }
};

class AutomixKeyGuardPlanner {
  public:
    // At least one bar of incoming drums alone before the melody swaps
    // ("first only the new drums").
    static constexpr double kLeadInBeats = 4.0;
    // The melody swaps on a bar line (multiple of 4 transition beats; beat 0
    // is a downbeat). A handover at most this far past a bar line counts as
    // that bar line (grid conversion noise: 8.1 is still bar 2).
    static constexpr double kBarSnapToleranceBeats = 0.25;

    // True when both keys are known and do not fit: not the same Open Key /
    // Camelot number (same or relative key), and not one step apart in the
    // same mode. Two steps ("energy boost") and every diagonal count as a
    // clash. Mirrors keys_clash() + key_compatibility() in brain.
    static bool keysClash(mixxx::track::io::key::ChromaticKey outgoing,
            mixxx::track::io::key::ChromaticKey incoming);

    // Bass swap at half the recipe: Standard 8 -> 16, like the vocal guard's
    // latest handover. Melody swap: see melodySwapBeat(); pVocalPlan is the
    // vocal guard's plan when that guard is active, nullptr otherwise.
    static AutomixKeyGuardPlan plan(double lengthBeats,
            double fadeBeats,
            const AutomixVocalGuardPlan* pVocalPlan = nullptr,
            double incomingPlayAtBeat = 0.0);

    // max(outgoing voice fade start, incomingPlayAtBeat + kLeadInBeats),
    // snapped up to a bar line, capped at swapBeat. swapBeat itself without a
    // vocal plan (vocal guard off, an instrumental, no stems or maps).
    static double melodySwapBeat(double swapBeat,
            const AutomixVocalGuardPlan* pVocalPlan,
            double incomingPlayAtBeat);
};
