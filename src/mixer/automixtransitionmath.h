#pragma once

// Pure, engine-independent math for AutomixTransitionController (Faza 1.5).
//
// Deliberately free of any ControlObject/QObject dependency so it can be unit
// tested without a live Mixxx engine (see src/test/automixtransitionmath_test.cpp).
//
// Scope (Faza 1.5 MVP, approved by Dan 2026-10-04; duration revised to 2 bars
// 2026-10-05 per Dan's live-test feedback -- 16 bars felt far too long; 3-band
// continuous EQ + filter sweep added 2026-10-05, then the mid/high portion of
// that EQ fade REMOVED again later the same day after Dan reported a volume
// "jump" around the transition midpoint -- see docs/decisions/0008 addenda,
// specifically the 2026-10-05 "double-attenuation" addendum):
//  - Fixed 2-bar transition duration, computed from the outgoing deck's BPM.
//  - Crossfader: full linear sweep from one deck to the other over the
//    transition. This is the SOLE primary volume blend between the two decks
//    (constant-power curve, configured at the Mixxx [Mixer Profile] level --
//    see the crossfader-curve addendum). Nothing else in this class should
//    ride a second full attenuation-equivalent fade on top of it.
//  - EQ: ONLY the low band is automated, on its own faster, front-loaded
//    curve (complete by 60% of the transition -- see outgoing/
//    incomingBassGainForProgress) -- standard DJ technique: swap bass
//    quickly to avoid low-end mud between two full-bass tracks, a problem
//    that exists regardless of the decks' relative crossfader volume (both
//    basslines are present simultaneously for part of the transition no
//    matter what). Mid/high bands are deliberately left untouched (held at
//    unity gain) for the whole transition: letting the crossfader alone
//    carry their presence avoids the double-attenuation bug (crossfader
//    gain x EQ gain compounding multiplicatively, producing a real ~6dB dip
//    in combined loudness at progress=0.5 that read as a "jump" on the way
//    back up). Mid/high EQ automation was previously a continuous linear
//    fade identical in shape to the bass curve but spanning the whole
//    transition (outgoing/incomingEqGainForProgress, now removed); that
//    function pair is gone, not just unused.
//  - Filter: each deck's single "Filter" (quick-effect) knob sweeps over the
//    whole transition, in complementary directions per deck, through an
//    ease-in-out curve (see easeInOut() below -- added 2026-10-05 per Dan's
//    live-test feedback that the raw-linear sweep felt "robotic"; constant-
//    velocity motion reads as mechanical, eased motion reads as a hand
//    move). This is tonal/spectral shaping (cutoff frequency), not a volume
//    multiplier, so it is unaffected by the double-attenuation issue above.
//  - Mid scoop (EXPERIMENTAL, 2026-10-05, see docs/decisions/0008 addendum):
//    both decks' mid band dips TOGETHER by a mild amount, peaking at
//    progress=0.5 and returning to unity at both ends (midScoopGainForProgress
//    below). This is architecturally different from the removed mid/high
//    fade above -- it is symmetric (both decks move the same way, not
//    opposing each other), so it does not reproduce the double-attenuation
//    bug; it also peaks exactly where the constant-power crossfader is at
//    its *least* extreme (both decks near equal gain at progress=0.5), not
//    where either deck's crossfader gain is already low, so it never
//    compounds with an already-heavy attenuation. Depth is controlled by a
//    single constant (kMidScoopDepth) that can be set to 0.0 to disable the
//    effect entirely without removing the code path.
//  - Tempo matching (REWORKED 2026-10-05, see docs/decisions/0008 "snap to
//    grid" addendum): a single one-shot rate_ratio adjustment on the
//    incoming deck at transition start (tempoMatchedIncomingRateRatio()
//    below), NOT Mixxx's continuous sync engine. The previous approach
//    (sync_leader on outgoing + sync_enabled on incoming) matched tempo by
//    riding Mixxx's own Sync engine, which also forces ongoing beatgrid
//    phase correction as an inseparable side effect -- discarding any
//    manual beatmatching/pitch-bend Dan had already done before pressing
//    MIX. The one-shot rate_ratio write only ever changes playback speed;
//    it never touches beat position, so it cannot fight manual alignment.
class AutomixTransitionMath {
  public:
    // Transition length in bars/beats. Revised 16->2 bars 2026-10-05 per
    // Dan's live-test feedback (8 beats = 2 bars of 4 beats).
    static constexpr double kTransitionBars = 2.0;
    static constexpr double kBeatsPerBar = 4.0;

    // Gain values used for the low-band (bass) swap curve below. These
    // assume the default Mixxx EQ effect's gain parameter is unity (no
    // boost/cut) at 1.0 and fully cut at 0.0 -- confirmed by reading the EQ
    // knob wiring in res/skins/SeratoLike/mixer/eq_knob_left.xml (parameter1
    // == Low), but the *neutral==1.0 / cut==0.0* assumption about the
    // effect's own gain curve is a documented judgment call carried over
    // from the original MVP (see docs/decisions/0008). Mid/high bands are no
    // longer automated at all (left at unity gain, never written by this
    // class) -- see the double-attenuation addendum, 2026-10-05.
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

    // EXPERIMENTAL (2026-10-05, pending Dan's ear-judgment -- see
    // docs/decisions/0008 addendum). Maximum fractional gain reduction of the
    // symmetric mid-band scoop at its peak (progress=0.5), e.g. 0.2 == at
    // most a 20% gain cut at the very peak, NOT a full cut. Set to 0.0 to
    // disable the effect entirely -- the controller skips writing/monitoring
    // the mid-scoop CO altogether when this is 0.0, so disabling it also
    // means it never touches a manually-set mid knob.
    static constexpr double kMidScoopDepth = 0.2;

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
    // in [0, 1]. Eased (see easeInOut() below), not linear, as of
    // 2026-10-05 -- raw-linear motion on a timer read as mechanical in
    // Dan's live test. Outgoing deck sweeps from neutral toward the
    // high-pass end (sound "thins out"/fades into the distance). Incoming
    // deck starts at a mild low-pass position and sweeps back to neutral by
    // the end (sound "comes into focus"). Start/end values are unchanged
    // from the linear version -- only the shape of the curve between them
    // changed.
    static double outgoingFilterForProgress(double progress);
    static double incomingFilterForProgress(double progress);

    // Smoothstep ease-in-out: 3t^2 - 2t^3. Clamps t to [0, 1] first (so
    // out-of-range input behaves the same as every other function here --
    // clamp-then-compute). Starts and ends at the same values as linear
    // (easeInOut(0)==0, easeInOut(0.5)==0.5, easeInOut(1)==1) but
    // accelerates away from each endpoint and decelerates into the next,
    // instead of moving at constant velocity -- the standard reason
    // easing curves read as "natural/hand-performed" where linear
    // interpolation reads as "robotic". Used by the filter sweep above;
    // deliberately NOT applied to the crossfader (already constant-power
    // corrected by Mixxx's own engine, see docs/decisions/0008) or the bass
    // swap curve (front-loaded shape is a deliberate DJ-technique choice,
    // not meant to be smoothed into something slower).
    static double easeInOut(double t);

    // EXPERIMENTAL (2026-10-05, see docs/decisions/0008 addendum and the
    // kMidScoopDepth doc comment above). Symmetric mid-band gain multiplier
    // applied identically to BOTH decks regardless of crossfader position or
    // transition direction -- a hump shape that dips to its lowest point at
    // progress=0.5 and returns to unity (1.0) at progress=0 and progress=1.
    // This is NOT a per-deck fade like the removed mid/high EQ automation:
    // both decks move the same way at the same time, so it does not mirror
    // or duplicate the crossfader's relative-balance blend between the two
    // decks. Returns kEqUnityGain (no-op) for every progress when
    // kMidScoopDepth is 0.0.
    static double midScoopGainForProgress(double progress);

    // One-shot tempo match (2026-10-05, see docs/decisions/0008 addendum --
    // "snap to grid destroys manual beatmatching" bugfix). Computes the new
    // [ChannelN],rate_ratio value for the INCOMING deck that makes its
    // current effective BPM equal the outgoing deck's current effective BPM,
    // given the incoming deck's own current bpm/rate_ratio -- pure
    // proportion math, no engine/sync dependency:
    //
    //   newRatio = incomingRateRatio * (outgoingBpm / incomingBpm)
    //
    // This is deliberately the ONLY tempo-matching mechanism as of this
    // addendum: it replaces continuously-held sync_leader/sync_enabled
    // (Mixxx's sync engine), which forces ongoing beatgrid-phase correction
    // as a side effect of tempo matching -- exactly the behavior that
    // silently discarded Dan's manual beatmatching/pitch-bend the instant a
    // transition started. Setting rate_ratio only ever touches playback
    // speed; it never reads or writes beat position/phase, so it cannot
    // fight any manual alignment the deck already has. Applied once, at
    // transition start, then never touched again for the rest of the
    // transition -- not re-applied per tick like the crossfader/EQ/filter
    // curves above.
    //
    // Returns -1.0 (invalid/refuse-to-apply sentinel) if outgoingBpm,
    // incomingBpm, or incomingRateRatio is <= 0.0 (no usable track/tempo on
    // one side, or a corrupt rate_ratio reading) -- callers must treat a
    // negative return as "do not write rate_ratio this transition", not
    // clamp it to some fallback value.
    static double tempoMatchedIncomingRateRatio(
            double outgoingBpm, double incomingBpm, double incomingRateRatio);
};
