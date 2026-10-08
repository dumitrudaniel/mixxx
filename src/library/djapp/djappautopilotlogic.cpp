#include "library/djapp/djappautopilotlogic.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
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
    if (positionSec < mixOutSec.value()) {
        return out;
    }
    const int otherDeck = playingDeck == 1 ? 2 : 1;
    const bool otherLoaded = (playingDeck == 1 ? in.deck2Loaded : in.deck1Loaded) &&
            in.overrideEmptyDeck != otherDeck;

    out.playingDeckNumber = playingDeck;
    out.otherDeckNumber = otherDeck;
    out.action = otherLoaded ? Action::Trigger : Action::PickAndLoad;
    return out;
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
    case StatusKind::Picking:
        return QStringLiteral("deck %1 a ieșit · nicio sugestie disponibilă")
                .arg(status.deckNumber);
    case StatusKind::Picked:
        return QStringLiteral("am ales %1 · pregătesc tranziția").arg(status.trackText);
    case StatusKind::Armed:
        return QStringLiteral("tranziție armată · pornește pe bara următoare");
    case StatusKind::Running:
        return QStringLiteral("în tranziție");
    }
    return QStringLiteral("oprit");
}

} // namespace djapp::autopilot
