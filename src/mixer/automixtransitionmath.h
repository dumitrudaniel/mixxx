#pragma once

// Pure, engine-independent math for AutomixTransitionController (Faza 1.5).
//
// Deliberately free of any ControlObject/QObject dependency so it can be unit
// tested without a live Mixxx engine (see src/test/automixtransitionmath_test.cpp).
//
// Scope (Faza 1.5 MVP, approved by Dan 2026-10-04; duration revised to 2 bars
// 2026-10-05 per Dan's live-test feedback -- 16 bars felt far too long; 3-band
// continuous EQ + filter sweep added 2026-10-05, replacing the original
// instant-bass-swap-only MVP; bass-band staggering re-added 2026-10-05 (second
// addendum same day) after Dan reported the all-bands-identical-rate version
// felt mechanical -- see docs/decisions/0008 addenda):
//  - Fixed 2-bar transition duration, computed from the outgoing deck's BPM.
//  - Crossfader: full linear sweep from one deck to the other over the transition.
//  - EQ: the low band swaps on its OWN faster, front-loaded curve (complete by
//    60% of the transition -- see outgoing/incomingBassGainForProgress), while
//    mid/high fade continuously and linearly over the WHOLE transition (see
//    outgoing/incomingEqGainForProgress) -- standard DJ technique: swap bass
//    quickly to avoid low-end mud between two full-bass tracks, blend mid/high
//    gradually for a smooth, non-abrupt tonal transition.
//  - Filter: each deck's single "Filter" (quick-effect) knob sweeps linearly
//    over the whole transition, in complementary directions per deck.
class AutomixTransitionMath {
  public:
    // Transition length in bars/beats. Revised 16->2 bars 2026-10-05 per
    // Dan's live-test feedback (8 beats = 2 bars of 4 beats).
    static constexpr double kTransitionBars = 2.0;
    static constexpr double kBeatsPerBar = 4.0;

    // Gain values used for the EQ fade, identical for all three bands (low/
    // mid/high). These assume the default Mixxx EQ effect's gain parameter is
    // unity (no boost/cut) at 1.0 and fully cut at 0.0 -- confirmed by
    // reading the EQ knob wiring in res/skins/SeratoLike/mixer/eq_knob_left.xml
    // (parameter1/2/3 == Low/Mid/High respectively), but the *neutral==1.0 /
    // cut==0.0* assumption about the effect's own gain curve is a documented
    // judgment call carried over from the original MVP (see docs/decisions/0008).
    static constexpr double kEqUnityGain = 1.0;
    static constexpr double kEqCutGain = 0.0;

    // Low-band-specific stagger: real DJ mixing swaps bass fast/early (full
    // bass clash between two tracks is unpleasant even briefly), while
    // letting mid/high blend across the whole transition. The low band
    // completes its own swap by 60% of total progress, then holds; mid/high
    // keep using the full-duration curve above. Chosen as a clean, simple
    // piecewise-linear fraction -- not a cifră cerută explicit de Dan, doar
    // o alegere de judecată documentată; de ajustat dintr-o singură
    // constantă dacă bass swap-ul sună prea rapid/lent la testare live.
    static constexpr double kBassSwapFraction = 0.6;

    // Quick-filter ("Filter" knob) sweep range, via the
    // [QuickEffectRack1_[ChannelN]],super1 ControlPotmeter. Confirmed in
    // source (effects/backends/builtin/filtereffect.cpp +
    // effects/effectchain.cpp): range is [0.0, 1.0], 0.5 is neutral/no-filter
    // (both the LinkedLeft "lpf" and LinkedRight "hpf" parameters sit at
    // their own neutral point there), sweeping toward 1.0 engages the
    // high-pass filter (hpf corner frequency rises from its minimum/no-op
    // toward its maximum), sweeping toward 0.0 engages the low-pass filter
    // (lpf corner frequency falls from its maximum/no-op toward its minimum).
    static constexpr double kFilterNeutral = 0.5;
    static constexpr double kFilterOutgoingEnd = 1.0; // full high-pass end.
    static constexpr double kFilterIncomingStart = 0.3; // mild low-pass position.

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

    // EQ gain for the outgoing/incoming deck at a given progress in [0, 1].
    // Used for the MID and HIGH bands only as of 2026-10-05 (low band now has
    // its own faster curve below). Linear: outgoing fades unity(1.0) ->
    // cut(0.0), incoming fades cut(0.0) -> unity(1.0), over the WHOLE
    // transition, matching crossfaderForProgress's own linear curve.
    static double outgoingEqGainForProgress(double progress);
    static double incomingEqGainForProgress(double progress);

    // Low-band-only EQ gain, front-loaded: the swap completes by
    // kBassSwapFraction (60%) of progress, then holds at the end value for
    // the remainder of the transition. Outgoing fades unity(1.0) -> cut(0.0)
    // over [0, kBassSwapFraction], then stays at cut. Incoming fades
    // cut(0.0) -> unity(1.0) over the same window, then stays at unity.
    // Complementary throughout (sums to 1.0 at every progress), same as the
    // mid/high curve, just compressed into the first 60% of the duration.
    static double outgoingBassGainForProgress(double progress);
    static double incomingBassGainForProgress(double progress);

    // Quick-filter sweep for the outgoing/incoming deck at a given progress
    // in [0, 1]. Linear, same curve shape as the EQ/crossfader functions.
    // Outgoing deck sweeps from neutral toward the high-pass end (sound
    // "thins out"/fades into the distance). Incoming deck starts at a mild
    // low-pass position and sweeps back to neutral by the end (sound "comes
    // into focus").
    static double outgoingFilterForProgress(double progress);
    static double incomingFilterForProgress(double progress);
};
