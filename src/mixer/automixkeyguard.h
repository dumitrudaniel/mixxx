#pragma once

#include "proto/keys.pb.h"

// DJ App key guard, "doar tobe peste" (Dan, 2026-10-08, docs/decisions/0022).
// C++ port of brain/automix/keyguard.py, used live by the automix executor
// (mixer/automixtransitioncontroller.h) on Mixxx 2.6 stem tracks:
//
//  1. Keys that fit (same, neighbours on the Camelot wheel, relative
//     major/minor): nothing happens.
//  2. Keys that clash: the incoming harmonic stems (bass, other, vocals) stay
//     muted and the outgoing ones keep playing until the swap beat, half the
//     recipe (where the EQ lanes swap bass and highs); there they cross over
//     in fadeBeats, equal-power like the band swaps. Drums are never touched.
//  3. With the vocal guard on as well, the vocals stem takes the smaller of
//     the two gains: the old voice still fades at its phrase end, the new one
//     never comes in before the swap.
//
// Positions are in TRANSITION beats (0 = the bar the transition starts on).
// Pure math, unit tested in src/test/automixkeyguard_test.cpp.

struct AutomixKeyGuardPlan {
    // Harmonic stems: outgoing full until here, then cos fade over fadeBeats;
    // incoming muted until here, then sin fade over fadeBeats.
    double swapBeat = 0.0;
    double fadeBeats = 0.0;

    double outgoingGain(double beat) const;
    double incomingGain(double beat) const;
    // Transition beat from which both gains are final.
    double endBeat() const {
        return swapBeat + fadeBeats;
    }
};

class AutomixKeyGuardPlanner {
  public:
    // True when both keys are known and do not fit: not the same Open Key /
    // Camelot number (same or relative key), and not one step apart in the
    // same mode. Two steps ("energy boost") and every diagonal count as a
    // clash. Mirrors keys_clash() + key_compatibility() in brain.
    static bool keysClash(mixxx::track::io::key::ChromaticKey outgoing,
            mixxx::track::io::key::ChromaticKey incoming);

    // Swap at half the recipe: Standard 8 -> 16, like the vocal guard's
    // latest handover.
    static AutomixKeyGuardPlan plan(double lengthBeats, double fadeBeats);
};
