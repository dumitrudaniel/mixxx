#pragma once

#include <optional>

#include "audio/frame.h"

namespace mixxx {
class Beats;
} // namespace mixxx

// Pure, engine-independent math for the automix transition engine
// (Faza 1.5, rebuilt as Etapa 0 of docs/plan-automix-v2.md, 2026-10-06).
//
// Deliberately free of any ControlObject/QObject dependency so it can be unit
// tested without a live Mixxx engine (see src/test/automixtransitionmath_test.cpp).
//
// What lives here:
//  - Curve shapes used by recipe lanes (see mixer/automixrecipe.h).
//  - Quantized start: which grid beat an armed transition starts on.
//  - The transition clock: how many beats to advance per tick, robust to
//    seeks/loops and to the outgoing deck stopping.
//  - The one-shot tempo match (unchanged since the 2026-10-05 "snap to grid"
//    fix, docs/decisions/0008): a single rate_ratio write, never Sync.
//
// The old Faza 1.5 curves (crossfader sweep, bass swap fraction, filter sweep,
// mid scoop) are gone: the transition is now data (recipes), and the
// crossfader is never touched (Dan mixes from the EQs only, see
// docs/decisions/0017).
class AutomixTransitionMath {
  public:
    static constexpr double kBeatsPerBar = 4.0;

    // Shape of one lane segment, applied to normalized time t in [0, 1].
    //  - Linear:     t
    //  - Sin:        sin(t*pi/2)      fast start, soft landing (ease-out)
    //  - Cos:        1 - cos(t*pi/2)  soft start, fast end (ease-in)
    //  - Smoothstep: 3t^2 - 2t^3      soft at both ends
    //  - Hold:       0 until t reaches 1, then 1 (value jumps at the segment end)
    //
    // Equal-power swap between two decks: the band going OUT uses Cos from
    // 1 to 0 (= cos(t*pi/2)) and the band coming IN uses Sin from 0 to 1
    // (= sin(t*pi/2)); the sum of squares stays 1 for the whole segment.
    enum class Shape {
        Linear,
        Sin,
        Cos,
        Smoothstep,
        Hold,
    };

    // Clamps t to [0, 1], then applies the shape. Always returns 0 at t<=0
    // and 1 at t>=1.
    static double applyShape(Shape shape, double t);

    // from + (to - from) * applyShape(shape, t).
    static double interpolate(double from, double to, double t, Shape shape);

    // 10^(db/20). The Mixxx EQ knob value is a linear amplitude gain (1.0 =
    // unity, 0.25 = -12 dB, 0.0 = full kill; see knobValueToBiquadGainDb() in
    // effects/backends/builtin/biquadfullkilleqeffect.cpp), as is the channel
    // volume value.
    static double dbToGain(double db);

    // Grid beat on which an armed transition starts.
    //
    // `beat` is the outgoing deck's current position in beats from its grid
    // anchor (Mixxx's first downbeat, index 0), fractional. `quantumBeats`
    // is the start granularity (4 = next bar, 16 = next 4-bar block).
    // A press up to `graceBeats` AFTER a boundary still starts on that
    // boundary (a human pressing "on the one" is often a few ms late);
    // otherwise the next boundary is used. Works for negative beats (before
    // the anchor). Returns `beat` unchanged if quantumBeats <= 0.
    static double nextStartBeat(double beat, double quantumBeats, double graceBeats);

    // Fractional beat index of `position` on `beats`, counted from
    // cfirstmarker() (Mixxx's grid anchor, "first downbeat" = beat 0); the
    // fraction comes from the previous and the next beat. This is the beat
    // the transition clock runs on, and the vocal guard converts vocal
    // phrases with it. nullopt if the position is invalid or off the grid.
    static std::optional<double> gridBeatAt(
            const mixxx::Beats& beats, mixxx::audio::FramePos position);

    // How far the transition clock advances this tick, in beats.
    //
    // `rawDelta` is the change of the outgoing deck's grid position since the
    // previous tick; `expectedDelta` is what wall-clock time and the current
    // effective BPM predict. While the outgoing deck plays normally, the grid
    // position is the truth (so the swap lands on the grid downbeat even if
    // the tempo moves). A backwards or oversized jump (loop wrap, seek, cue)
    // is not musical time passing, and a stopped deck has no grid motion at
    // all; in both cases the clock keeps running on expectedDelta so a
    // transition can never freeze or rewind halfway.
    static double clockAdvance(double rawDelta, double expectedDelta, bool outgoingPlaying);

    // One-shot tempo match (2026-10-05, see docs/decisions/0008 addendum --
    // "snap to grid destroys manual beatmatching" bugfix). New
    // [ChannelN],rate_ratio for the INCOMING deck so its effective BPM equals
    // the outgoing deck's effective BPM:
    //
    //   newRatio = incomingRateRatio * (outgoingBpm / incomingBpm)
    //
    // Only ever changes playback speed, never beat position, so it cannot
    // fight manual alignment. Returns -1.0 (do not write) if any input is
    // <= 0.0.
    static double tempoMatchedIncomingRateRatio(
            double outgoingBpm, double incomingBpm, double incomingRateRatio);
};
