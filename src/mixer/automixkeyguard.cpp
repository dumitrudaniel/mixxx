#include "mixer/automixkeyguard.h"

#include <algorithm>
#include <cmath>

#include "track/keyutils.h"

namespace {
constexpr double kHalfPi = 1.57079632679489661923;

// 0 before `start`, 1 after `start + length`, linear in between.
double fadeProgress(double beat, double start, double length) {
    if (length <= 0.0) {
        return beat >= start ? 1.0 : 0.0;
    }
    return std::clamp((beat - start) / length, 0.0, 1.0);
}
} // namespace

double AutomixKeyGuardPlan::outgoingGain(double beat) const {
    return std::cos(fadeProgress(beat, swapBeat, fadeBeats) * kHalfPi);
}

double AutomixKeyGuardPlan::incomingGain(double beat) const {
    return std::sin(fadeProgress(beat, swapBeat, fadeBeats) * kHalfPi);
}

// static
bool AutomixKeyGuardPlanner::keysClash(mixxx::track::io::key::ChromaticKey outgoing,
        mixxx::track::io::key::ChromaticKey incoming) {
    using mixxx::track::io::key::INVALID;
    if (outgoing == INVALID || incoming == INVALID) {
        return false;
    }
    // Open Key numbers run around the wheel like Camelot's (a fixed offset),
    // with a key and its relative major/minor sharing a number.
    const int a = KeyUtils::keyToOpenKeyNumber(outgoing);
    const int b = KeyUtils::keyToOpenKeyNumber(incoming);
    const int diff = ((a - b) % 12 + 12) % 12;
    const int steps = std::min(diff, 12 - diff);
    const bool sameMode = KeyUtils::keyIsMajor(outgoing) == KeyUtils::keyIsMajor(incoming);
    if (steps == 0) {
        return false;
    }
    return !(steps == 1 && sameMode);
}

// static
AutomixKeyGuardPlan AutomixKeyGuardPlanner::plan(double lengthBeats, double fadeBeats) {
    AutomixKeyGuardPlan plan;
    plan.swapBeat = std::max(0.0, lengthBeats / 2.0);
    plan.fadeBeats = std::max(0.0, fadeBeats);
    return plan;
}
