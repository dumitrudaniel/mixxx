#pragma once

// Pure, engine-independent math for AutomixTransitionController (Faza 1.5).
//
// Deliberately free of any ControlObject/QObject dependency so it can be unit
// tested without a live Mixxx engine (see src/test/automixtransitionmath_test.cpp).
//
// Scope (Faza 1.5 MVP, approved by Dan 2026-10-04; duration revised to 2 bars
// 2026-10-05 per Dan's live-test feedback -- 16 bars felt far too long):
//  - Fixed 2-bar transition duration, computed from the outgoing deck's BPM.
//  - Crossfader: full linear sweep from one deck to the other over the transition.
//  - EQ: low-frequency ("bass") swap at the exact midpoint of the transition,
//    not a continuous 3-band sweep and not a filter sweep (both out of scope for v1).
class AutomixTransitionMath {
  public:
    // Transition length in bars/beats. Revised 16->2 bars 2026-10-05 per
    // Dan's live-test feedback (8 beats = 2 bars of 4 beats).
    static constexpr double kTransitionBars = 2.0;
    static constexpr double kBeatsPerBar = 4.0;

    // Gain values used for the instantaneous low-band swap. These assume the
    // default Mixxx EQ effect's gain parameter is unity (no boost/cut) at 1.0
    // and fully cut at 0.0 -- confirmed by reading the EQ effect's knob wiring
    // in res/skins/SeratoLike/mixer/eq_knob_left.xml (parameter1 == Low), but
    // the *neutral==1.0 / cut==0.0* assumption about the effect's own gain
    // curve is a documented judgment call (see docs/decisions/0008).
    static constexpr double kBassUnityGain = 1.0;
    static constexpr double kBassCutGain = 0.0;

    // Returns the transition duration in seconds for a 2-bar transition at
    // the given outgoing-deck BPM. Returns -1.0 if bpm is not usable (<= 0),
    // which callers must treat as "refuse to start the transition" (e.g. the
    // outgoing deck has no track loaded/analyzed yet).
    static double transitionDurationSeconds(double outgoingBpm);

    // Maps elapsed/duration time into a clamped [0, 1] progress value.
    // Returns 0.0 if durationSeconds <= 0 (guards against division by zero).
    static double progressForElapsed(double elapsedSeconds, double durationSeconds);

    // Crossfader position (Mixxx convention: -1.0 = full deck 1, +1.0 = full
    // deck 2) for a given progress in [0, 1]. `fromDeck1ToDeck2` selects the
    // sweep direction.
    static double crossfaderForProgress(double progress, bool fromDeck1ToDeck2);

    // Low-band ("bass") gain for the outgoing/incoming deck at a given
    // progress. The swap is instantaneous at the midpoint (progress == 0.5),
    // per PLAN.md's "bass swap la jumatatea blend-ului" -- not a continuous
    // crossfade of the low band.
    static double outgoingBassGainForProgress(double progress);
    static double incomingBassGainForProgress(double progress);
};
