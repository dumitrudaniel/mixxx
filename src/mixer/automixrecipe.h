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

// "tempo" in the recipe file, parsed like the 2.6 fork and brain so the same
// file means the same thing everywhere (docs/decisions/0027). Mixxx 2.5 only
// does the one-shot match: meet_return runs here as match_incoming; off skips
// the match (Fade curat). Legacy "tempo_match": false = mode off.
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

struct AutomixRecipe {
    QString id;
    QString name;
    double lengthBeats = 0.0;
    // One-shot rate_ratio match of the incoming deck at arm time: on unless
    // tempo.mode is Off.
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
