#pragma once

#include <QMap>
#include <QString>
#include <optional>

// DJ App Automix autopilot (docs/plan-ui-integrare.md §6 Etapa 5, cut down for
// a quick live test): pure, Qt-event-loop-free logic, unit tested in
// src/test/djappautopilot_test.cpp. The driver (djappautopilot.h) is the only
// caller; it supplies live ControlObject/PlayerInfo/brain.db values and
// executes the Decision by tapping the existing MIX trigger controls
// ([Channel1],automix_transition_to_2 / [Channel2],automix_transition_to_1),
// the same effect as pressing the button.
//
// Scope: watches whichever deck is playing while the automix engine is idle
// (not armed/running -- a transition already in progress, by Dan or by a
// previous autopilot trigger, is left alone). When that deck's position
// reaches its track's mix-out point for the active recipe's length
// (brain.db mix_points.mix_out_sec, keyed by transition length in beats), it
// either loads the best suggestion onto the other deck (if nothing is there)
// or, if a track is already loaded there (Dan's own pick, or a previous
// autopilot pick), just taps MIX.
namespace djapp::autopilot {

// mix_points.mix_out_sec is stored as JSON, e.g. {"8": 12.3, "16": 34.5}:
// transition length in beats -> seconds position (on the track's own
// timeline) where MIX should start for that length. Malformed/missing JSON
// gives an empty map (never guessed).
QMap<int, double> parseMixOutMap(const QString& json);

// The value whose key (transition length in beats) is nearest lengthBeats;
// nullopt if `byLength` is empty. Ties favor the smaller key (same rule as
// brain/store/mixpoints.py MixPointsRow.mix_in_for).
std::optional<double> nearestMixOut(const QMap<int, double>& byLength, double lengthBeats);

// How long before a track's mix-out point Dan gets to see candidates (change
// 2, live feedback 2026-10-09: "vreau să știu ce urmează mai din timp, nu în
// ultimul moment"; raised from 45s to 120s on further feedback the same
// night - "cu cât îl calculăm mai devreme cu atât mai bine"). The actual
// load/trigger timing is unchanged (still at/after mix_out_sec); only when
// the picker opens moves earlier.
constexpr double kLookaheadSec = 120.0;

// The up-to-3-candidate picker (change 3): how many rows to pull from brain's
// allowed and risky suggestion lists.
constexpr int kMaxCandidates = 3;

enum class Action {
    None, // nothing to do this tick
    ShowCandidates, // inside the lookahead window, other deck empty: offer up to kMaxCandidates picks, load nothing yet
    PickAndLoad, // past mix-out, other deck empty: pick a suggestion, load it, then trigger MIX
    Trigger, // past mix-out, other deck already has a track: just tap MIX
};

// How many of each list to take for the picker / for the deadline auto-pick
// (allowed first, risky only fills what's left) -- change 3's composition
// rule. Pure function of the counts brain.db's query already returned, so
// it is testable without a live suggestion query.
struct CandidatePlan {
    int allowedCount = 0;
    int riskyCount = 0;
};
CandidatePlan planCandidates(
        int allowedAvailable, int riskyAvailable, int maxCandidates = kMaxCandidates);

// True exactly when the only thing on offer is a risky pick (change 1: Dan
// never ends up with nothing loaded, but a risky-only pick must be flagged,
// not used silently). False both when an allowed candidate exists and when
// there is truly nothing at all (that dead end is `candidates.isEmpty()`,
// checked by the caller).
bool isRiskyFallback(int allowedAvailable, int riskyAvailable);

struct Inputs {
    bool enabled = false;
    bool engineIdle = true; // [AutomixTransition],state == 0 (idle: not armed, not running)
    bool deck1Playing = false;
    bool deck2Playing = false;
    bool deck1Loaded = false; // has a track ([ChannelN],track_loaded)
    bool deck2Loaded = false;
    double deck1PositionSec = 0.0;
    double deck2PositionSec = 0.0;
    std::optional<double> deck1MixOutSec; // from mix_points, for the active recipe length
    std::optional<double> deck2MixOutSec;
    // A deck that just finished being the outgoing side of a transition: its
    // old (now-played) track is still sitting there (autopilot never ejects
    // a track by itself), but the autopilot may treat it as "empty" for the
    // purpose of picking a fresh next track. 0 = no override. Cleared by the
    // driver once a load (by Dan or by the autopilot) happens on that deck.
    int overrideEmptyDeck = 0;
};

struct Decision {
    Action action = Action::None;
    int playingDeckNumber = 0; // the deck past its mix-out point (0 = none)
    int otherDeckNumber = 0; // the deck to load/trigger into (0 = none)
};

// Pure decision for one tick. See the enum/struct comments above for the
// rules; this function has no notion of time or state beyond what Inputs
// carries, so a caller (or a test) can replay any sequence of ticks.
Decision decide(const Inputs& in);

enum class StatusKind {
    Off,
    NoPlayingDeck, // on, but neither deck is playing
    BothPlaying, // on, but both decks are playing (ambiguous, wait)
    NoMixData, // the playing deck's track has no mix_points row yet
    Watching, // on, watching a deck, waiting to reach the lookahead window
    Choosing, // inside the lookahead window, candidates shown, waiting for a click or the deadline
    Picking, // past mix-out (or querying for the picker), no candidate at all -- the true dead end
    Picked, // a candidate was loaded onto the other deck (by Dan's click, or the deadline default)
    Armed, // [AutomixTransition],state == 1 (MIX armed, by Dan or by autopilot)
    Running, // [AutomixTransition],state == 2 (transition in progress)
};

struct Status {
    StatusKind kind = StatusKind::Off;
    int deckNumber = 0;
    double mixOutSec = 0.0;
    QString trackText;
    // Picked only: the loaded candidate was a risky fallback (change 1), not
    // an ideal match -- flagged, not silently used.
    bool risky = false;
};

// "urmăresc deck 1, iese la 3:12" etc. (Romanian, matches the other DJ App
// view strings' tone).
QString statusText(const Status& status);

} // namespace djapp::autopilot
