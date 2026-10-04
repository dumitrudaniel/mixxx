#include "mixer/automixtransitionmath.h"

#include <algorithm>

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
    return progress < 0.5 ? kBassUnityGain : kBassCutGain;
}

// static
double AutomixTransitionMath::incomingBassGainForProgress(double progress) {
    return progress < 0.5 ? kBassCutGain : kBassUnityGain;
}
