#include "mixer/automixrecipe.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <algorithm>
#include <set>
#include <utility>

namespace {

using Shape = AutomixTransitionMath::Shape;

// Default recipe file. Positions are in bars of 4 beats. Starting values,
// not verdicts: tuned by ear through the settings-dir copy of this file.
// Split into several literals because MSVC caps a single string literal at
// ~16 KB.
constexpr const char* kBuiltinJsonHead = R"JSON({
  "version": 1,
  "notes": "Retete automix DJ App. EQ: castig liniar, 1 = unitate, 0 = kill, \"-12dB\" = 0.25. Filtru: 0.5 neutru, spre 1 HPF, spre 0 LPF. Volum: 1 = fader la maxim. Punct: bar (sau beat), value (numar, current, kill, unity, neutral sau \"-6dB\"), shape = forma segmentului care se termina in punct (linear, sin, cos, smoothstep, hold). current = valoarea knob-ului la pornirea tranzitiei. Tempo: meet_return = ambele piese se intalnesc la tempo-ul de mijloc (a+b)/2 in primele meet_bars ale tranzitiei (cel mult jumatate din ea), apoi piesa noua revine la tempo-ul ei in return_bars; match_incoming = doar piesa noua ia tempo-ul celei vechi si ramane acolo; off = fara potrivire. Fisierul se reciteste la fiecare apasare MIX; sterge-l ca sa revii la valorile implicite.",
  "settings": {
    "arm_quantum_bars": 1,
    "resume_glide_beats": 1
  },
  "recipes": [
)JSON";

constexpr const char* kBuiltinJsonUrgenta2 = R"JSON(    {
      "id": "urgenta2",
      "name": "Urgenta 2",
      "notes": "2 bare: aceeasi reteta ca Standard 8, comprimata. Salvare rapida. Piesa veche iese complet pana la timpul 5.5 (11/16 din reteta).",
      "length_bars": 2,
      "tempo": { "mode": "meet_return", "meet_bars": 4, "return_bars": 8 },
      "prepare_incoming": { "eq_low": "kill", "eq_mid": "kill", "eq_high": "kill" },
      "lanes": [
        { "deck": "incoming", "param": "eq_mid", "points": [
          { "bar": 0, "value": "current" }, { "bar": 1, "value": "unity", "shape": "sin" } ] },
        { "deck": "incoming", "param": "eq_high", "points": [
          { "bar": 0, "value": "current" }, { "bar": 1, "value": "-12dB", "shape": "sin" },
          { "bar": 1.25, "value": "unity", "shape": "sin" } ] },
        { "deck": "incoming", "param": "eq_low", "points": [
          { "bar": 0, "value": "current" }, { "bar": 1, "value": "current" },
          { "bar": 1.25, "value": "unity", "shape": "sin" } ] },
        { "deck": "outgoing", "param": "eq_low", "points": [
          { "bar": 0, "value": "current" }, { "bar": 1, "value": "current" },
          { "bar": 1.25, "value": "kill", "shape": "cos" } ] },
        { "deck": "outgoing", "param": "eq_high", "points": [
          { "bar": 0, "value": "current" }, { "bar": 1, "value": "current" },
          { "bar": 1.25, "value": "kill", "shape": "cos" } ] },
        { "deck": "outgoing", "param": "eq_mid", "points": [
          { "bar": 0, "value": "current" }, { "bar": 1, "value": "current" },
          { "bar": 1.375, "value": "kill", "shape": "smoothstep" } ] }
      ]
    },
)JSON";

constexpr const char* kBuiltinJsonScurt4 = R"JSON(    {
      "id": "scurt4",
      "name": "Scurt 4",
      "notes": "4 bare: grila nesigura, semba/kompa live, mod scoala. Piesa veche iese complet pana la bara 2.75 (timpul 11, 11/16 din reteta). Basul se schimba liniar de la bara 0, plin cel tarziu la bara 2 (Dan, 2026-10-09: pe o reteta asa scurta vrea basul nou intrat full cat mai din timp, nu sincronizat cu 11/16 ca la Standard 8/Lung 16).",
      "length_bars": 4,
      "tempo": { "mode": "meet_return", "meet_bars": 4, "return_bars": 8 },
      "prepare_incoming": { "eq_low": "kill", "eq_mid": "kill", "eq_high": "kill" },
      "lanes": [
        { "deck": "incoming", "param": "eq_mid", "points": [
          { "bar": 0, "value": "current" }, { "bar": 2, "value": "unity", "shape": "sin" } ] },
        { "deck": "incoming", "param": "eq_high", "points": [
          { "bar": 0, "value": "current" }, { "bar": 2, "value": "-12dB", "shape": "sin" },
          { "bar": 2.5, "value": "unity", "shape": "sin" } ] },
        { "deck": "incoming", "param": "eq_low", "points": [
          { "bar": 0, "value": "current" }, { "bar": 2, "value": "unity", "shape": "linear" } ] },
        { "deck": "outgoing", "param": "eq_low", "points": [
          { "bar": 0, "value": "current" }, { "bar": 2, "value": "kill", "shape": "linear" } ] },
        { "deck": "outgoing", "param": "eq_high", "points": [
          { "bar": 0, "value": "current" }, { "bar": 2, "value": "current" },
          { "bar": 2.5, "value": "kill", "shape": "cos" } ] },
        { "deck": "outgoing", "param": "eq_mid", "points": [
          { "bar": 0, "value": "current" }, { "bar": 2, "value": "current" },
          { "bar": 2.75, "value": "kill", "shape": "smoothstep" } ] }
      ]
    },
)JSON";

constexpr const char* kBuiltinJsonStandard8 = R"JSON(    {
      "id": "standard8",
      "name": "Standard 8",
      "notes": "Implicit. Faza 1 (bare 0-4): mediile piesei noi urca, inaltele ei doar pana la -12 dB; basul ei ramane taiat pana la bara 3.5. Faza 2 (bare 3.5-5.5): bas schimbat liniar pe ultimele 2 bare, pana exact cand piesa veche e complet afara (Dan, 2026-10-09: sincronizat cu regula 11/16 - basul ajunge full exact cand restul piesei vechi tace). Faza 3 (bare 4-5.5): mediile piesei vechi coboara la kill (smoothstep); din timpul 22 (ultima treime) se aude doar piesa noua (Dan, 2026-10-08). Filtrul si intrarea mai usoara in volum sunt oprite (enabled false); pune true ca sa le incerci.",
      "length_bars": 8,
      "tempo": { "mode": "meet_return", "meet_bars": 4, "return_bars": 8 },
      "prepare_incoming": { "eq_low": "kill", "eq_mid": "kill", "eq_high": "kill" },
      "lanes": [
        { "deck": "incoming", "param": "eq_mid", "points": [
          { "bar": 0, "value": "current" }, { "bar": 4, "value": "unity", "shape": "sin" } ] },
        { "deck": "incoming", "param": "eq_high", "points": [
          { "bar": 0, "value": "current" }, { "bar": 4, "value": "-12dB", "shape": "sin" },
          { "bar": 5, "value": "unity", "shape": "sin" } ] },
        { "deck": "incoming", "param": "eq_low", "points": [
          { "bar": 0, "value": "current" }, { "bar": 3.5, "value": "current" },
          { "bar": 5.5, "value": "unity", "shape": "linear" } ] },
        { "deck": "outgoing", "param": "eq_low", "points": [
          { "bar": 0, "value": "current" }, { "bar": 3.5, "value": "current" },
          { "bar": 5.5, "value": "kill", "shape": "linear" } ] },
        { "deck": "outgoing", "param": "eq_high", "points": [
          { "bar": 0, "value": "current" }, { "bar": 4, "value": "current" },
          { "bar": 5, "value": "kill", "shape": "cos" } ] },
        { "deck": "outgoing", "param": "eq_mid", "points": [
          { "bar": 0, "value": "current" }, { "bar": 4, "value": "current" },
          { "bar": 5.5, "value": "kill", "shape": "smoothstep" } ] },
        { "deck": "outgoing", "param": "filter", "enabled": false, "points": [
          { "bar": 0, "value": "current" }, { "bar": 4, "value": "current" },
          { "bar": 5.5, "value": 0.75, "shape": "smoothstep" } ] },
        { "deck": "incoming", "param": "volume", "enabled": false, "points": [
          { "bar": 0, "value": "-2dB" }, { "bar": 2, "value": "current", "shape": "sin" } ] }
      ]
    },
)JSON";

constexpr const char* kBuiltinJsonLung16 = R"JSON(    {
      "id": "lung16",
      "name": "Lung 16",
      "notes": "16 bare: urban kiz, tarraxo, intro/outro lungi. Basul se schimba liniar pe ultimele 2 bare, pana exact cand piesa veche e complet afara (Dan, 2026-10-09: sincronizat cu regula 11/16, ca la Standard 8). Piesa veche iese complet pana la bara 11 (timpul 44, 11/16 din reteta).",
      "length_bars": 16,
      "tempo": { "mode": "meet_return", "meet_bars": 4, "return_bars": 8 },
      "prepare_incoming": { "eq_low": "kill", "eq_mid": "kill", "eq_high": "kill" },
      "lanes": [
        { "deck": "incoming", "param": "eq_mid", "points": [
          { "bar": 0, "value": "current" }, { "bar": 8, "value": "unity", "shape": "sin" } ] },
        { "deck": "incoming", "param": "eq_high", "points": [
          { "bar": 0, "value": "current" }, { "bar": 8, "value": "-12dB", "shape": "sin" },
          { "bar": 9, "value": "unity", "shape": "sin" } ] },
        { "deck": "incoming", "param": "eq_low", "points": [
          { "bar": 0, "value": "current" }, { "bar": 9, "value": "current" },
          { "bar": 11, "value": "unity", "shape": "linear" } ] },
        { "deck": "outgoing", "param": "eq_low", "points": [
          { "bar": 0, "value": "current" }, { "bar": 9, "value": "current" },
          { "bar": 11, "value": "kill", "shape": "linear" } ] },
        { "deck": "outgoing", "param": "eq_high", "points": [
          { "bar": 0, "value": "current" }, { "bar": 8, "value": "current" },
          { "bar": 9, "value": "kill", "shape": "cos" } ] },
        { "deck": "outgoing", "param": "eq_mid", "points": [
          { "bar": 0, "value": "current" }, { "bar": 8, "value": "current" },
          { "bar": 11, "value": "kill", "shape": "smoothstep" } ] }
      ]
    },
)JSON";

constexpr const char* kBuiltinJsonFadeCurat = R"JSON(    {
      "id": "fade_curat",
      "name": "Fade curat",
      "notes": "Fara beatmatch: tempo peste buget sau piese incompatibile. Piesa veche se stinge in 2 bare; piesa noua porneste la bara 1.5. Regula 11/16 nu se aplica: suprapunerea e doar ultima jumatate de bara, scoaterea mai devreme ar lasa liniste.",
      "length_bars": 2,
      "tempo": { "mode": "off" },
      "incoming_play_at_bar": 1.5,
      "prepare_incoming": { "eq_low": "unity", "eq_mid": "unity", "eq_high": "unity" },
      "lanes": [
        { "deck": "outgoing", "param": "eq_low", "points": [
          { "bar": 0, "value": "current" }, { "bar": 2, "value": "kill", "shape": "cos" } ] },
        { "deck": "outgoing", "param": "eq_mid", "points": [
          { "bar": 0, "value": "current" }, { "bar": 2, "value": "kill", "shape": "cos" } ] },
        { "deck": "outgoing", "param": "eq_high", "points": [
          { "bar": 0, "value": "current" }, { "bar": 2, "value": "kill", "shape": "cos" } ] },
        { "deck": "incoming", "param": "eq_low", "points": [
          { "bar": 0, "value": "current" }, { "bar": 1.5, "value": "current" },
          { "bar": 2, "value": "unity", "shape": "sin" } ] },
        { "deck": "incoming", "param": "eq_mid", "points": [
          { "bar": 0, "value": "current" }, { "bar": 1.5, "value": "current" },
          { "bar": 2, "value": "unity", "shape": "sin" } ] },
        { "deck": "incoming", "param": "eq_high", "points": [
          { "bar": 0, "value": "current" }, { "bar": 1.5, "value": "current" },
          { "bar": 2, "value": "unity", "shape": "sin" } ] }
      ]
    }
  ]
}
)JSON";

bool parseParam(const QString& text, AutomixParam* pParam) {
    if (text == QLatin1String("eq_low")) {
        *pParam = AutomixParam::EqLow;
    } else if (text == QLatin1String("eq_mid")) {
        *pParam = AutomixParam::EqMid;
    } else if (text == QLatin1String("eq_high")) {
        *pParam = AutomixParam::EqHigh;
    } else if (text == QLatin1String("filter")) {
        *pParam = AutomixParam::Filter;
    } else if (text == QLatin1String("volume")) {
        *pParam = AutomixParam::Volume;
    } else {
        return false;
    }
    return true;
}

// "tempo": absent = meet_return with defaults (Dan's decision #6), unless the
// legacy "tempo_match": false asks for no tempo change. Both present must agree.
// Same rules as the 2.6 fork and brain/automix/recipes.py; the 2.5 engine only
// reads tempoMatch (one-shot match unless mode off).
bool parseTempo(const QJsonValue& json,
        const QJsonValue& legacyTempoMatch,
        AutomixTempo* pTempo,
        QString* pError) {
    *pTempo = AutomixTempo();
    if (!legacyTempoMatch.isUndefined() && !legacyTempoMatch.isBool()) {
        *pError = QStringLiteral("tempo_match must be true or false");
        return false;
    }
    const bool legacyOff = legacyTempoMatch.isBool() && !legacyTempoMatch.toBool();
    if (json.isUndefined() || json.isNull()) {
        if (legacyOff) {
            pTempo->mode = AutomixTempoMode::Off;
        }
        return true;
    }
    if (!json.isObject()) {
        *pError = QStringLiteral("tempo must be an object");
        return false;
    }
    const QJsonObject object = json.toObject();
    const QString mode = object.value(QStringLiteral("mode")).toString(
            QStringLiteral("meet_return"));
    if (mode == QLatin1String("meet_return")) {
        pTempo->mode = AutomixTempoMode::MeetReturn;
    } else if (mode == QLatin1String("match_incoming")) {
        pTempo->mode = AutomixTempoMode::MatchIncoming;
    } else if (mode == QLatin1String("off")) {
        pTempo->mode = AutomixTempoMode::Off;
    } else {
        *pError = QStringLiteral("tempo: unknown mode \"%1\"").arg(mode);
        return false;
    }
    if (legacyTempoMatch.isBool() &&
            legacyTempoMatch.toBool() == (pTempo->mode == AutomixTempoMode::Off)) {
        *pError = QStringLiteral("tempo_match contradicts tempo.mode \"%1\"").arg(mode);
        return false;
    }
    const double meetBars = object.value(QStringLiteral("meet_bars")).toDouble(4.0);
    const double returnBars = object.value(QStringLiteral("return_bars")).toDouble(8.0);
    if (meetBars <= 0.0 || returnBars < 0.0) {
        *pError = QStringLiteral("tempo: meet_bars must be > 0 and return_bars >= 0");
        return false;
    }
    pTempo->meetBeats = meetBars * AutomixTransitionMath::kBeatsPerBar;
    pTempo->returnBeats = returnBars * AutomixTransitionMath::kBeatsPerBar;
    return true;
}

bool parseShape(const QString& text, Shape* pShape) {
    if (text == QLatin1String("linear")) {
        *pShape = Shape::Linear;
    } else if (text == QLatin1String("sin")) {
        *pShape = Shape::Sin;
    } else if (text == QLatin1String("cos")) {
        *pShape = Shape::Cos;
    } else if (text == QLatin1String("smoothstep")) {
        *pShape = Shape::Smoothstep;
    } else if (text == QLatin1String("hold")) {
        *pShape = Shape::Hold;
    } else {
        return false;
    }
    return true;
}

double maxValueForParam(AutomixParam param) {
    switch (param) {
    case AutomixParam::EqLow:
    case AutomixParam::EqMid:
    case AutomixParam::EqHigh:
        return 4.0; // Mixxx EQ knob range is [0, 4] (+12 dB).
    case AutomixParam::Filter:
    case AutomixParam::Volume:
        return 1.0;
    }
    return 1.0;
}

// Parses a point/preset value. "current" is only meaningful inside lanes,
// so callers that cannot use it pass pUseStartValue == nullptr.
bool parseValue(const QJsonValue& json,
        AutomixParam param,
        double* pValue,
        bool* pUseStartValue,
        QString* pError) {
    double value = 0.0;
    if (pUseStartValue) {
        *pUseStartValue = false;
    }
    if (json.isDouble()) {
        value = json.toDouble();
    } else if (json.isString()) {
        const QString text = json.toString().trimmed().toLower();
        if (text == QLatin1String("current")) {
            if (!pUseStartValue) {
                *pError = QStringLiteral("\"current\" is only allowed in lane points");
                return false;
            }
            *pUseStartValue = true;
            *pValue = 0.0;
            return true;
        } else if (text == QLatin1String("kill")) {
            value = 0.0;
        } else if (text == QLatin1String("unity")) {
            value = 1.0;
        } else if (text == QLatin1String("neutral")) {
            value = 0.5;
        } else {
            static const QRegularExpression kDbPattern(
                    QStringLiteral("^([-+]?[0-9]*\\.?[0-9]+)\\s*db$"));
            const QRegularExpressionMatch match = kDbPattern.match(text);
            if (!match.hasMatch()) {
                *pError = QStringLiteral("unknown value \"%1\"").arg(json.toString());
                return false;
            }
            value = AutomixTransitionMath::dbToGain(match.captured(1).toDouble());
        }
    } else {
        *pError = QStringLiteral("value must be a number or a string");
        return false;
    }
    if (value < 0.0 || value > maxValueForParam(param)) {
        *pError = QStringLiteral("value %1 out of range [0, %2]")
                          .arg(value)
                          .arg(maxValueForParam(param));
        return false;
    }
    *pValue = value;
    return true;
}

bool parseLane(const QJsonObject& json,
        double lengthBeats,
        AutomixLane* pLane,
        bool* pEnabled,
        QString* pError) {
    const QString deck = json.value(QStringLiteral("deck")).toString();
    if (deck == QLatin1String("outgoing")) {
        pLane->deck = AutomixDeckRole::Outgoing;
    } else if (deck == QLatin1String("incoming")) {
        pLane->deck = AutomixDeckRole::Incoming;
    } else {
        *pError = QStringLiteral("deck must be \"outgoing\" or \"incoming\"");
        return false;
    }
    if (!parseParam(json.value(QStringLiteral("param")).toString(), &pLane->param)) {
        *pError = QStringLiteral("unknown param \"%1\"")
                          .arg(json.value(QStringLiteral("param")).toString());
        return false;
    }
    *pEnabled = json.value(QStringLiteral("enabled")).toBool(true);

    const QJsonArray points = json.value(QStringLiteral("points")).toArray();
    if (points.isEmpty()) {
        *pError = QStringLiteral("lane has no points");
        return false;
    }
    pLane->points.clear();
    for (const QJsonValue& pointValue : points) {
        const QJsonObject pointJson = pointValue.toObject();
        AutomixPoint point;
        if (pointJson.contains(QStringLiteral("bar"))) {
            point.beat = pointJson.value(QStringLiteral("bar")).toDouble() *
                    AutomixTransitionMath::kBeatsPerBar;
        } else if (pointJson.contains(QStringLiteral("beat"))) {
            point.beat = pointJson.value(QStringLiteral("beat")).toDouble();
        } else {
            *pError = QStringLiteral("point needs \"bar\" or \"beat\"");
            return false;
        }
        if (point.beat < 0.0 || point.beat > lengthBeats + 1e-9) {
            *pError = QStringLiteral("point at beat %1 is outside the recipe length")
                              .arg(point.beat);
            return false;
        }
        if (!pLane->points.empty() && point.beat < pLane->points.back().beat) {
            *pError = QStringLiteral("points must be in time order");
            return false;
        }
        if (!parseValue(pointJson.value(QStringLiteral("value")),
                    pLane->param,
                    &point.value,
                    &point.useStartValue,
                    pError)) {
            return false;
        }
        const QString shape = pointJson.value(QStringLiteral("shape"))
                                      .toString(QStringLiteral("linear"));
        if (!parseShape(shape, &point.shape)) {
            *pError = QStringLiteral("unknown shape \"%1\"").arg(shape);
            return false;
        }
        pLane->points.push_back(point);
    }
    if (pLane->points.front().beat > 0.0) {
        AutomixPoint start;
        start.beat = 0.0;
        start.useStartValue = true;
        pLane->points.insert(pLane->points.begin(), start);
    }
    return true;
}

bool parseRecipe(const QJsonObject& json, AutomixRecipe* pRecipe, QString* pError) {
    pRecipe->id = json.value(QStringLiteral("id")).toString();
    if (pRecipe->id.isEmpty()) {
        *pError = QStringLiteral("recipe without id");
        return false;
    }
    pRecipe->name = json.value(QStringLiteral("name")).toString(pRecipe->id);
    const double lengthBars = json.value(QStringLiteral("length_bars")).toDouble(0.0);
    if (lengthBars <= 0.0) {
        *pError = QStringLiteral("length_bars must be > 0");
        return false;
    }
    pRecipe->lengthBeats = lengthBars * AutomixTransitionMath::kBeatsPerBar;
    if (!parseTempo(json.value(QStringLiteral("tempo")),
                json.value(QStringLiteral("tempo_match")),
                &pRecipe->tempo,
                pError)) {
        return false;
    }
    pRecipe->tempoMatch = pRecipe->tempo.mode != AutomixTempoMode::Off;
    pRecipe->autoPlayIncoming = json.value(QStringLiteral("auto_play_incoming")).toBool(true);
    pRecipe->incomingPlayAtBeat =
            json.value(QStringLiteral("incoming_play_at_bar")).toDouble(0.0) *
            AutomixTransitionMath::kBeatsPerBar;
    if (pRecipe->incomingPlayAtBeat < 0.0 ||
            pRecipe->incomingPlayAtBeat > pRecipe->lengthBeats) {
        *pError = QStringLiteral("incoming_play_at_bar is outside the recipe length");
        return false;
    }

    pRecipe->prepareIncoming.clear();
    const QJsonObject prepare = json.value(QStringLiteral("prepare_incoming")).toObject();
    for (auto it = prepare.constBegin(); it != prepare.constEnd(); ++it) {
        AutomixPreset preset;
        if (!parseParam(it.key(), &preset.param)) {
            *pError = QStringLiteral("prepare_incoming: unknown param \"%1\"").arg(it.key());
            return false;
        }
        if (!parseValue(it.value(), preset.param, &preset.value, nullptr, pError)) {
            *pError = QStringLiteral("prepare_incoming.%1: %2").arg(it.key(), *pError);
            return false;
        }
        pRecipe->prepareIncoming.push_back(preset);
    }

    pRecipe->lanes.clear();
    std::set<std::pair<int, int>> seen;
    const QJsonArray lanes = json.value(QStringLiteral("lanes")).toArray();
    for (int i = 0; i < lanes.size(); ++i) {
        AutomixLane lane;
        bool enabled = true;
        if (!parseLane(lanes.at(i).toObject(), pRecipe->lengthBeats, &lane, &enabled, pError)) {
            *pError = QStringLiteral("lane %1: %2").arg(i).arg(*pError);
            return false;
        }
        if (!enabled) {
            continue;
        }
        const auto key = std::make_pair(static_cast<int>(lane.deck), static_cast<int>(lane.param));
        if (!seen.insert(key).second) {
            *pError = QStringLiteral("lane %1: a deck/param pair can only have one enabled lane")
                              .arg(i);
            return false;
        }
        pRecipe->lanes.push_back(std::move(lane));
    }
    return true;
}

} // namespace

double AutomixLane::valueAt(double beat, double startValue) const {
    if (points.empty()) {
        return startValue;
    }
    const auto resolve = [startValue](const AutomixPoint& point) {
        return point.useStartValue ? startValue : point.value;
    };
    if (beat <= points.front().beat) {
        return resolve(points.front());
    }
    for (std::size_t i = 1; i < points.size(); ++i) {
        const AutomixPoint& to = points[i];
        if (beat < to.beat) {
            const AutomixPoint& from = points[i - 1];
            const double t = (beat - from.beat) / (to.beat - from.beat);
            return AutomixTransitionMath::interpolate(resolve(from), resolve(to), t, to.shape);
        }
    }
    return resolve(points.back());
}

// static
const QStringList& AutomixRecipeBook::selectorIds() {
    static const QStringList kIds = {
            QStringLiteral("urgenta2"),
            QStringLiteral("scurt4"),
            QStringLiteral("standard8"),
            QStringLiteral("lung16"),
            QStringLiteral("fade_curat"),
    };
    return kIds;
}

// static
bool AutomixRecipeBook::parse(const QByteArray& json, AutomixRecipeBook* pBook, QString* pError) {
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (doc.isNull()) {
        *pError = QStringLiteral("JSON error at offset %1: %2")
                          .arg(parseError.offset)
                          .arg(parseError.errorString());
        return false;
    }
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("version")).toInt(0) != 1) {
        *pError = QStringLiteral("unsupported version (expected 1)");
        return false;
    }

    AutomixRecipeBook book;
    const QJsonObject settings = root.value(QStringLiteral("settings")).toObject();
    book.m_armQuantumBars = settings.value(QStringLiteral("arm_quantum_bars")).toDouble(1.0);
    if (book.m_armQuantumBars <= 0.0) {
        *pError = QStringLiteral("settings.arm_quantum_bars must be > 0");
        return false;
    }
    book.m_resumeGlideBeats = settings.value(QStringLiteral("resume_glide_beats")).toDouble(1.0);
    if (book.m_resumeGlideBeats < 0.0) {
        *pError = QStringLiteral("settings.resume_glide_beats must be >= 0");
        return false;
    }

    const QJsonArray recipes = root.value(QStringLiteral("recipes")).toArray();
    for (int i = 0; i < recipes.size(); ++i) {
        AutomixRecipe recipe;
        if (!parseRecipe(recipes.at(i).toObject(), &recipe, pError)) {
            *pError = QStringLiteral("recipe %1 (%2): %3")
                              .arg(i)
                              .arg(recipes.at(i).toObject().value(QStringLiteral("id")).toString(),
                                      *pError);
            return false;
        }
        if (book.find(recipe.id)) {
            *pError = QStringLiteral("duplicate recipe id \"%1\"").arg(recipe.id);
            return false;
        }
        book.m_recipes.push_back(std::move(recipe));
    }
    *pBook = std::move(book);
    return true;
}

// static
QByteArray AutomixRecipeBook::builtinJson() {
    QByteArray json;
    json.append(kBuiltinJsonHead);
    json.append(kBuiltinJsonUrgenta2);
    json.append(kBuiltinJsonScurt4);
    json.append(kBuiltinJsonStandard8);
    json.append(kBuiltinJsonLung16);
    json.append(kBuiltinJsonFadeCurat);
    return json;
}

// static
const AutomixRecipeBook& AutomixRecipeBook::builtin() {
    static const AutomixRecipeBook kBook = [] {
        AutomixRecipeBook book;
        QString error;
        const bool ok = parse(builtinJson(), &book, &error);
        // Covered by AutomixRecipeTest.BuiltinParses; an empty book only
        // means MIX refuses to start, never a crash.
        Q_UNUSED(ok);
        return book;
    }();
    return kBook;
}

const AutomixRecipe* AutomixRecipeBook::find(const QString& id) const {
    const auto it = std::find_if(m_recipes.cbegin(),
            m_recipes.cend(),
            [&id](const AutomixRecipe& recipe) { return recipe.id == id; });
    return it == m_recipes.cend() ? nullptr : &*it;
}

const AutomixRecipe* AutomixRecipeBook::forSelectorIndex(int index) const {
    const QStringList& ids = selectorIds();
    if (index < 0 || index >= ids.size()) {
        index = kDefaultSelectorIndex;
    }
    const QString& id = ids.at(index);
    if (const AutomixRecipe* pRecipe = find(id)) {
        return pRecipe;
    }
    if (this != &builtin()) {
        return builtin().find(id);
    }
    return nullptr;
}
