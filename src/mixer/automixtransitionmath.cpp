#include "mixer/automixtransitionmath.h"

#include <algorithm>
#include <cmath>

namespace {
// Avoid relying on M_PI (not guaranteed defined on MSVC without
// _USE_MATH_DEFINES) for the mid-scoop's sine hump.
constexpr double kPi = 3.14159265358979323846;
} // namespace

// static
double AutomixTransitionMath::transitionDurationSeconds(double outgoingBpm) {
    if (outgoingBpm <= 0.0) {
        return -1.0;
    }
    const double totalBeats = kTransitionBars * kBeatsPerBar;
    return totalBeats * (60.0 / outgoingBpm);
}

// static
double AutomixTransitionMath::progressForElapsed(double elapsedSeconds, double durationSeconds) {
    if (durationSeconds <= 0.0) {
        return 0.0;
    }
    const double progress = elapsedSeconds / durationSeconds;
    return std::clamp(progress, 0.0, 1.0);
}

// static
double AutomixTransitionMath::crossfaderForProgress(double progress, bool fromDeck1ToDeck2) {
    const double clamped = std::clamp(progress, 0.0, 1.0);
    // -1.0 .. +1.0 linear sweep.
    const double sweep = -1.0 + clamped * 2.0;
    return fromDeck1ToDeck2 ? sweep : -sweep;
}

// static
double AutomixTransitionMath::outgoingBassGainForProgress(double progress) {
    const double clamped = std::clamp(progress, 0.0, 1.0);
    if (clamped >= kBassSwapFraction) {
        return kEqCutGain;
    }
    const double windowFraction = clamped / kBassSwapFraction;
    return kEqUnityGain + windowFraction * (kEqCutGain - kEqUnityGain);
}

// static
double AutomixTransitionMath::incomingBassGainForProgress(double progress) {
    const double clamped = std::clamp(progress, 0.0, 1.0);
    if (clamped >= kBassSwapFraction) {
        return kEqUnityGain;
    }
    const double windowFraction = clamped / kBassSwapFraction;
    return kEqCutGain + windowFraction * (kEqUnityGain - kEqCutGain);
}

// static
double AutomixTransitionMath::outgoingFilterForProgress(double progress) {
    const double eased = easeInOut(progress); // clamps internally.
    return kFilterNeutral + eased * (kFilterOutgoingEnd - kFilterNeutral);
}

// static
double AutomixTransitionMath::incomingFilterForProgress(double progress) {
    const double eased = easeInOut(progress); // clamps internally.
    return kFilterIncomingStart + eased * (kFilterNeutral - kFilterIncomingStart);
}

// static
double AutomixTransitionMath::easeInOut(double t) {
    const double clamped = std::clamp(t, 0.0, 1.0);
    return clamped * clamped * (3.0 - 2.0 * clamped);
}

// static
double AutomixTransitionMath::midScoopGainForProgress(double progress) {
    const double clamped = std::clamp(progress, 0.0, 1.0);
    return kEqUnityGain - kMidScoopDepth * std::sin(kPi * clamped);
}

// static
double AutomixTransitionMath::tempoMatchedIncomingRateRatio(
        double outgoingBpm, double incomingBpm, double incomingRateRatio) {
    if (outgoingBpm <= 0.0 || incomingBpm <= 0.0 || incomingRateRatio <= 0.0) {
        return -1.0;
    }
    return incomingRateRatio * (outgoingBpm / incomingBpm);
}
