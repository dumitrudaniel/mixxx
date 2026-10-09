#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <vector>

#include "mixer/automixtransitionmath.h"

// Transition recipes as data (Etapa 0, docs/plan-automix-v2.md 3.1-3.3 and
// docs/decisions/0017). A recipe is a set of automation lanes; each lane
// moves one control of one deck through points placed on the transition's
// beat timeline. The controller (mixer/automixtransitioncontroller.h) only
// executes lanes; every musical decision lives in the recipe file
// <settings dir>/automix_recipes.json, which is re-read on every MIX press
// so curves can be tuned by ear without rebuilding Mixxx.
//
// Pure data + parsing, no ControlObject dependency (unit tested in
// src/test/automixrecipe_test.cpp). The same JSON is read by the Python
// offline simulator (brain/tools/automix_sim.py).

enum class AutomixDeckRole {
    Outgoing,
    Incoming,
};

enum class AutomixParam {
    EqLow,
    EqMid,
    EqHigh,
    Filter,
    Volume,
    // Mixxx 2.6 stems, file (NI) order: [ChannelN_Stem1..4],volume = drums,
    // bass, other, vocals (docs/decisions/0020). No-ops on non-stem tracks.
    StemDrums,
    StemBass,
    StemOther,
    StemVocals,
};

// "Astept fraza" (Dan's practice, docs/decisions/0020): the outgoing voice
// finishes the phrase it is singing when the transition starts, then fades in
// fadeBeats; the incoming voice stays muted until then and fades in after.
// The outgoing phrase is cut anyway after maxWaitBeats. Needs stem tracks and
// a vocal map from brain.db; otherwise the recipe runs on the EQs alone.
struct AutomixVocalGuard {
    bool enabled = true;
    double maxWaitBeats = 32.0;
    double fadeBeats = 2.0;
};

// "Doar tobe peste" (Dan, docs/decisions/0022): when the two keys clash, only
// the incoming drums play until half the recipe; there the harmonic stems
// swap - bass and other over fadeBeats, vocals over vocalsFadeBeats. Needs
// stem tracks with a key; otherwise the recipe runs on the EQs alone.
struct AutomixKeyGuard {
    bool enabled = true;
    // Bass + other (Dan, 2026-10-09: "volumele de stems se misca foarte
    // brusc" - slowed down from the original 2 beats).
    double fadeBeats = 8.0;
    // Vocals only, independent of the above - Dan: "doar cel de voce as da
    // voie sa se miste asa rapid", kept at the original fast speed.
    double vocalsFadeBeats = 2.0;
};

// Live tempo during and after a transition (Dan's decision #6,
// docs/decisions/0027, planned by mixer/automixtempo.h).
//  - MeetReturn (default): both decks glide together to the midpoint tempo
//    (a + b) / 2 over the first meetBeats of the transition (at most half the
//    recipe), stay there, then the incoming deck glides back to its own tempo
//    over returnBeats of its beats after the transition (0 = it stays at the
//    midpoint).
//  - MatchIncoming: the incoming deck takes the outgoing tempo at arm time and
//    keeps it (the one-shot match from docs/decisions/0008).
//  - Off: no tempo change at all (Fade curat).
// JSON: "tempo": {"mode": "meet_return" | "match_incoming" | "off",
// "meet_bars": 4, "return_bars": 8}. Legacy "tempo_match": false = mode off.
enum class AutomixTempoMode {
    MeetReturn,
    MatchIncoming,
    Off,
};

struct AutomixTempo {
    AutomixTempoMode mode = AutomixTempoMode::MeetReturn;
    double meetBeats = 16.0;
    double returnBeats = 32.0;
};

struct AutomixPoint {
    double beat = 0.0;
    // "current" in JSON: the value the control held when the transition
    // started, so a lane ramps from wherever the knob was left instead of
    // jumping to a fixed value.
    bool useStartValue = false;
    double value = 0.0;
    // Shape of the segment that ENDS at this point.
    AutomixTransitionMath::Shape shape = AutomixTransitionMath::Shape::Linear;
};

struct AutomixLane {
    AutomixDeckRole deck = AutomixDeckRole::Outgoing;
    AutomixParam param = AutomixParam::EqLow;
    // Sorted by beat (non-decreasing), first point at beat 0. Two points on
    // the same beat make an instant jump; the later one wins from that beat on.
    std::vector<AutomixPoint> points;

    // Lane value at `beat` (transition beats, 0 = start), given the control's
    // value at transition start. Holds the first value before the first
    // point and the last value after the last point.
    double valueAt(double beat, double startValue) const;
};

struct AutomixPreset {
    AutomixParam param = AutomixParam::EqLow;
    double value = 0.0;
};

struct AutomixRecipe {
    QString id;
    QString name;
    double lengthBeats = 0.0;
    // One-shot rate_ratio match of the incoming deck at arm time: on unless
    // tempo.mode is Off (MeetReturn starts from the matched tempo too).
    bool tempoMatch = true;
    AutomixTempo tempo;
    // If the incoming deck is stopped when MIX is armed, it is prepared with
    // `prepareIncoming` and started at `incomingPlayAtBeat` (Mixxx aligns its
    // phase on play when quantize is on).
    bool autoPlayIncoming = true;
    double incomingPlayAtBeat = 0.0;
    std::vector<AutomixPreset> prepareIncoming;
    // Enabled lanes only; lanes with "enabled": false are validated, then dropped.
    std::vector<AutomixLane> lanes;
    AutomixVocalGuard vocalGuard;
    AutomixKeyGuard keyGuard;
};

class AutomixRecipeBook {
  public:
    // Recipe ids behind the selector button's states, in skin order
    // (res/skins/SeratoLike/mixer/automix_controls.xml).
    static const QStringList& selectorIds();
    static constexpr int kDefaultSelectorIndex = 2; // standard8

    // Parses a recipe file. On failure returns false, describes the problem
    // in *pError and leaves *pBook untouched.
    static bool parse(const QByteArray& json, AutomixRecipeBook* pBook, QString* pError);

    // The default recipe file, written to the settings dir on first use.
    static QByteArray builtinJson();
    static const AutomixRecipeBook& builtin();

    const AutomixRecipe* find(const QString& id) const;
    // Recipe for a selector slot; a slot missing from this book falls back to
    // the built-in recipe with the same id. Out-of-range index -> default slot.
    const AutomixRecipe* forSelectorIndex(int index) const;

    // Granularity of the armed start, in bars (1 = next bar).
    double armQuantumBars() const {
        return m_armQuantumBars;
    }
    // Glide from a manually set value back onto the curve after "Reia auto".
    double resumeGlideBeats() const {
        return m_resumeGlideBeats;
    }
    int size() const {
        return static_cast<int>(m_recipes.size());
    }

  private:
    std::vector<AutomixRecipe> m_recipes;
    double m_armQuantumBars = 1.0;
    double m_resumeGlideBeats = 1.0;
};
