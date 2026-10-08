#include "mixer/automixtempo.h"

#include <algorithm>
#include <cmath>

#include "mixer/automixtransitionmath.h"

namespace {
using Shape = AutomixTransitionMath::Shape;

void setWhyNot(QString* pWhyNot, const QString& reason) {
    if (pWhyNot) {
        *pWhyNot = reason;
    }
}
} // namespace

double AutomixTempoPlan::transitionFactor(double transitionBeat) const {
    if (meetEndBeat <= 0.0) {
        return transitionBeat >= 0.0 ? meetFactor : 1.0;
    }
    return AutomixTransitionMath::interpolate(
            1.0, meetFactor, transitionBeat / meetEndBeat, Shape::Smoothstep);
}

double AutomixTempoPlan::returnFactorAt(double returnBeat) const {
    if (returnBeats <= 0.0) {
        return meetFactor;
    }
    return AutomixTransitionMath::interpolate(
            meetFactor, returnFactor, returnBeat / returnBeats, Shape::Smoothstep);
}

// static
std::optional<AutomixTempoPlan> AutomixTempoPlanner::plan(const AutomixTempo& tempo,
        double recipeLengthBeats,
        double outgoingBpm,
        double incomingBpm,
        double incomingRateRatio,
        QString* pWhyNot) {
    if (tempo.mode == AutomixTempoMode::Off) {
        setWhyNot(pWhyNot, QStringLiteral("recipe has tempo off"));
        return std::nullopt;
    }
    if (tempo.mode == AutomixTempoMode::MatchIncoming) {
        setWhyNot(pWhyNot,
                QStringLiteral("recipe has tempo match_incoming (one-shot match only)"));
        return std::nullopt;
    }
    if (outgoingBpm <= 0.0 || incomingBpm <= 0.0 || incomingRateRatio <= 0.0) {
        setWhyNot(pWhyNot, QStringLiteral("a deck has no BPM"));
        return std::nullopt;
    }
    AutomixTempoPlan plan;
    plan.outgoingBpm = outgoingBpm;
    plan.incomingBpm = incomingBpm / incomingRateRatio;
    plan.meetBpm = (plan.outgoingBpm + plan.incomingBpm) / 2.0;
    const double step = std::abs(plan.outgoingBpm - plan.incomingBpm) / plan.meetBpm;
    if (step > kMaxMeetStep) {
        setWhyNot(pWhyNot,
                QStringLiteral("step %1 -> %2 BPM is %3 %, over %4 %: incoming "
                               "matched only")
                        .arg(QString::number(plan.outgoingBpm, 'f', 2),
                                QString::number(plan.incomingBpm, 'f', 2),
                                QString::number(step * 100.0, 'f', 1),
                                QString::number(kMaxMeetStep * 100.0, 'f', 0)));
        return std::nullopt;
    }
    // Done by the bass swap at half the recipe at the latest: the swap and the
    // old track's exit happen at a steady tempo.
    plan.meetEndBeat = std::max(0.0, std::min(tempo.meetBeats, recipeLengthBeats / 2.0));
    plan.meetFactor = plan.meetBpm / plan.outgoingBpm;
    plan.returnBeats = std::max(0.0, tempo.returnBeats);
    plan.returnFactor = plan.hasReturn() ? 1.0 / incomingRateRatio : plan.meetFactor;
    return plan;
}
