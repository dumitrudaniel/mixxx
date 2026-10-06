#include "mixer/automixtransitionmath.h"

#include <algorithm>
#include <cmath>

namespace {
// Avoid relying on M_PI (not guaranteed defined on MSVC without
// _USE_MATH_DEFINES).
constexpr double kPi = 3.14159265358979323846;

// Grid motion smaller than this backwards is read jitter, not a jump.
constexpr double kBackwardJitterBeats = 0.05;
// Grid motion this far beyond what wall-clock time predicts is a seek, not
// playback catching up after a late engine callback or a stalled GUI tick.
constexpr double kForwardJumpSlackBeats = 0.5;
} // namespace

// static
double AutomixTransitionMath::applyShape(Shape shape, double t) {
    const double x = std::clamp(t, 0.0, 1.0);
    switch (shape) {
    case Shape::Linear:
        return x;
    case Shape::Sin:
        return std::sin(x * kPi / 2.0);
    case Shape::Cos:
        return 1.0 - std::cos(x * kPi / 2.0);
    case Shape::Smoothstep:
        return x * x * (3.0 - 2.0 * x);
    case Shape::Hold:
        return x >= 1.0 ? 1.0 : 0.0;
    }
    return x;
}

// static
double AutomixTransitionMath::interpolate(double from, double to, double t, Shape shape) {
    return from + (to - from) * applyShape(shape, t);
}

// static
double AutomixTransitionMath::dbToGain(double db) {
    return std::pow(10.0, db / 20.0);
}

// static
double AutomixTransitionMath::nextStartBeat(
        double beat, double quantumBeats, double graceBeats) {
    if (quantumBeats <= 0.0) {
        return beat;
    }
    const double boundary = std::floor(beat / quantumBeats) * quantumBeats;
    if (beat - boundary <= graceBeats) {
        return boundary;
    }
    return boundary + quantumBeats;
}

// static
double AutomixTransitionMath::clockAdvance(
        double rawDelta, double expectedDelta, bool outgoingPlaying) {
    const double expected = std::max(expectedDelta, 0.0);
    if (!outgoingPlaying) {
        return expected;
    }
    if (rawDelta < -kBackwardJitterBeats || rawDelta > expected + kForwardJumpSlackBeats) {
        return expected;
    }
    return std::max(rawDelta, 0.0);
}

// static
double AutomixTransitionMath::tempoMatchedIncomingRateRatio(
        double outgoingBpm, double incomingBpm, double incomingRateRatio) {
    if (outgoingBpm <= 0.0 || incomingBpm <= 0.0 || incomingRateRatio <= 0.0) {
        return -1.0;
    }
    return incomingRateRatio * (outgoingBpm / incomingBpm);
}
