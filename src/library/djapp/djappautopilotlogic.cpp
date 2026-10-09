#include "library/djapp/djappautopilotlogic.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <algorithm>
#include <cmath>
#include <limits>

namespace djapp::autopilot {

QMap<int, double> parseMixOutMap(const QString& json) {
    QMap<int, double> result;
    if (json.isEmpty()) {
        return result;
    }
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return result;
    }
    const QJsonObject obj = doc.object();
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
        bool ok = false;
        const int beats = it.key().toInt(&ok);
        if (ok && it.value().isDouble()) {
            result.insert(beats, it.value().toDouble());
        }
    }
    return result;
}

std::optional<double> nearestMixOut(const QMap<int, double>& byLength, double lengthBeats) {
    if (byLength.isEmpty()) {
        return std::nullopt;
    }
    int bestKey = byLength.firstKey();
    double bestDistance = std::numeric_limits<double>::max();
    for (auto it = byLength.constBegin(); it != byLength.constEnd(); ++it) {
        const double distance = std::abs(static_cast<double>(it.key()) - lengthBeats);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestKey = it.key();
        }
    }
    return byLength.value(bestKey);
}

Decision decide(const Inputs& in) {
    Decision out;
    if (!in.enabled || !in.engineIdle) {
        return out;
    }
    if (in.deck1Playing == in.deck2Playing) {
        // Neither playing, or (ambiguously) both: wait.
        return out;
    }
    const int playingDeck = in.deck1Playing ? 1 : 2;
    const double positionSec = playingDeck == 1 ? in.deck1PositionSec : in.deck2PositionSec;
    const std::optional<double> mixOutSec =
            playingDeck == 1 ? in.deck1MixOutSec : in.deck2MixOutSec;
    if (!mixOutSec.has_value()) {
        return out;
    }
    const double deadline = mixOutSec.value();
    const double lookaheadStart = deadline - kLookaheadSec;
    if (positionSec < lookaheadStart) {
        // Too early even to start showing candidates: Watching.
        return out;
    }

    const int otherDeck = playingDeck == 1 ? 2 : 1;
    const bool otherLoaded = (playingDeck == 1 ? in.deck2Loaded : in.deck1Loaded) &&
            in.overrideEmptyDeck != otherDeck;

    out.playingDeckNumber = playingDeck;
    out.otherDeckNumber = otherDeck;

    if (positionSec < deadline) {
        // Lookahead window open (change 2): offer candidates early, but only
        // while the other deck is still free -- Dan's own pick (or an
        // earlier click/auto-pick) always takes priority, nothing to choose
        // once it's spoken for.
        out.action = otherLoaded ? Action::None : Action::ShowCandidates;
        return out;
    }

    // Deadline reached: load the default pick if Dan hasn't chosen one
    // (change 4), or trigger the transition if the other deck already has a
    // track (Dan's pick, a clicked candidate, or the autopilot's own
    // fallback load).
    out.action = otherLoaded ? Action::Trigger : Action::PickAndLoad;
    return out;
}

CandidatePlan planCandidates(int allowedAvailable, int riskyAvailable, int maxCandidates) {
    CandidatePlan plan;
    plan.allowedCount = std::clamp(allowedAvailable, 0, maxCandidates);
    const int remaining = maxCandidates - plan.allowedCount;
    plan.riskyCount = std::clamp(riskyAvailable, 0, remaining);
    return plan;
}

bool isRiskyFallback(int allowedAvailable, int riskyAvailable) {
    return allowedAvailable <= 0 && riskyAvailable > 0;
}

namespace {
QString minutesSeconds(double seconds) {
    const int total = static_cast<int>(std::lround(std::max(0.0, seconds)));
    return QStringLiteral("%1:%2")
            .arg(total / 60)
            .arg(total % 60, 2, 10, QLatin1Char('0'));
}
} // namespace

QString statusText(const Status& status) {
    switch (status.kind) {
    case StatusKind::Off:
        return QStringLiteral("oprit");
    case StatusKind::NoPlayingDeck:
        return QStringLiteral("pornit · aștept o piesă care cântă");
    case StatusKind::BothPlaying:
        return QStringLiteral("pornit · ambele deck-uri cântă, aștept");
    case StatusKind::NoMixData:
        return QStringLiteral("urmăresc deck %1 · fără puncte de tranziție (mix_points)")
                .arg(status.deckNumber);
    case StatusKind::Watching:
        return QStringLiteral("urmăresc deck %1 · iese la %2")
                .arg(status.deckNumber)
                .arg(minutesSeconds(status.mixOutSec));
    case StatusKind::Choosing:
        return QStringLiteral("deck %1 iese la %2 · alege o variantă mai jos")
                .arg(status.deckNumber)
                .arg(minutesSeconds(status.mixOutSec));
    case StatusKind::Picking:
        return QStringLiteral("deck %1 a ieșit · nicio sugestie disponibilă")
                .arg(status.deckNumber);
    case StatusKind::Picked:
        return (status.risky ? QStringLiteral("⚠ fără opțiune ideală, aleg cea mai apropiată "
                                                "(riscant) · ")
                              : QString()) +
                QStringLiteral("am ales %1 · pregătesc tranziția").arg(status.trackText);
    case StatusKind::Armed:
        return QStringLiteral("tranziție armată · pornește pe bara următoare");
    case StatusKind::Running:
        return QStringLiteral("în tranziție");
    }
    return QStringLiteral("oprit");
}

} // namespace djapp::autopilot
