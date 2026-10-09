#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>
#include <optional>

#include "control/controlproxy.h"
#include "control/controlpushbutton.h"
#include "library/djapp/djappautopilotlogic.h"
#include "preferences/usersettings.h"
#include "track/track_decl.h"
#include "util/class.h"

class Library;

// A picker row (change 3): display-only, no TrackPointer -- clicking a row
// in the widget calls DJAppAutopilot::chooseCandidate(index), the autopilot
// keeps the actual track.
struct DJAppAutopilotCandidate {
    QString label;
    double score = 0.0;
    double stepBpm = 0.0;
    QString keyVerdict;
    bool allowed = false; // false = risky (change 1/3: flagged, not hidden)
    // brain.db's pair_scores.recipe_hint for this specific pair (Dan,
    // 2026-10-09): "fade_curat" when the tempo step is too big or either
    // grid is unreliable for a beatmatched mix, otherwise the set's own
    // recipe id. Shown so Dan can see when a transition will NOT use his
    // currently selected recipe.
    QString recipeHint;
};

// DJ App automix autopilot (docs/plan-ui-integrare.md §6 Etapa 5, cut down
// for a quick live test, 2026-10-09). OFF by default. When ON, it fills the
// gap Dan would otherwise fill by hand: when the deck that is playing
// reaches its track's recommended mix-out point (brain.db mix_points, for
// the active recipe's length), it loads the best suggestion
// (DJAppSuggestions::query) onto the other deck if nothing is there yet,
// then taps the existing MIX trigger -- the same ControlObject a button
// press sets (mixer/automixtransitioncontroller.h), so every refusal rule
// (crossfader centered, playing, has grid, ...) still applies: the engine
// itself decides, this class only presses the button for Dan.
//
// Never touches: crossfader, faders, sync, the now-free deck after a
// transition (its old track is left loaded; see the `overrideEmptyDeck`
// comment in djappautopilotlogic.h). Never writes brain.db or mixxxdb.sqlite
// (read-only through BrainDbReader / Mixxx's own library tables, like every
// other DJ App reader).
//
// Pure decision logic lives in djappautopilotlogic.h/.cpp (unit tested);
// this class only supplies live values and executes the result on a 500 ms
// timer (no need for the automix engine's 20 ms tick: brain.db reads happen
// when a deck's track changes, not on every tick).
class DJAppAutopilot : public QObject {
    Q_OBJECT
  public:
    DJAppAutopilot(UserSettingsPointer pConfig, Library* pLibrary, QObject* pParent);
    ~DJAppAutopilot() override;

    bool isEnabled() const;

  public slots:
    // "PORNEȘTE AUTOMIX" / "OPREȘTE AUTOMIX" toggle button.
    void setEnabled(bool enabled);
    // Dan clicked candidate `index` in the picker (change 3): load it onto
    // the free deck right away, without waiting for the deadline.
    void chooseCandidate(int index);

  signals:
    void statusTextChanged(const QString& text);
    void enabledChanged(bool enabled);
    // Forwarded by DJAppFeature exactly like DlgDJAppSuggestions::loadTrackToPlayer.
    void loadTrackToPlayer(TrackPointer pTrack, const QString& group, bool play);
    // Up to kMaxCandidates rows for the picker (change 3); empty clears it
    // (a click landed, the deadline's default pick landed, or the cycle
    // moved on to a different track).
    void candidatesChanged(const QList<DJAppAutopilotCandidate>& candidates);

  private slots:
    void tick();

  private:
    struct DeckProxies {
        explicit DeckProxies(const QString& group);
        ControlProxy play;
        ControlProxy trackLoaded;
        ControlProxy playposition;
        ControlProxy duration;
    };

    // Re-reads brain.db's mix_points for a deck's track only when that
    // deck's track (or the active recipe's length) changed since the last
    // tick -- never inside the 500 ms poll itself.
    void refreshMixOutCache(int deckNumber);
    double activeRecipeLengthBeats() const;
    std::optional<double> readMixOutSec(const QString& location, double lengthBeats) const;

    // A resolved candidate (change 1/3/4): the actual track plus the same
    // display fields as DJAppAutopilotCandidate.
    struct Candidate {
        TrackPointer track;
        QString label;
        double score = 0.0;
        double stepBpm = 0.0;
        QString keyVerdict;
        bool allowed = false;
        QString recipeHint; // brain.db's pair_scores.recipe_hint for this pair
    };
    static QList<DJAppAutopilotCandidate> toDisplayList(const QList<Candidate>& candidates);

    // Queries brain.db for up to kMaxCandidates candidates after
    // playingDeckNumber's track, allowed first then risky filling the rest
    // (djapp::autopilot::planCandidates). Empty = the true dead end (change
    // 1's "nicio sugestie disponibilă" case); *pRiskyFallback is set when the
    // list is non-empty but contains no allowed candidate at all.
    QList<Candidate> queryCandidates(
            int playingDeckNumber, int otherDeckNumber, bool* pRiskyFallback);
    // Lookahead window (change 2/3): query once per (track, other deck) and
    // show the picker; a no-op on later ticks while the same candidates are
    // still showing.
    void showCandidates(int playingDeckNumber, int otherDeckNumber);
    // Loads `candidate` onto otherDeckNumber right away -- the shared path
    // for both a Dan click and the deadline's default pick.
    // By value: chooseCandidate() passes m_candidates.at(index), and this
    // clears m_candidates (resetCandidates()) before it is done reading the
    // candidate's fields -- a reference would dangle at that point.
    void loadCandidate(int playingDeckNumber, int otherDeckNumber, Candidate candidate);

    void pickAndLoad(int playingDeckNumber, int otherDeckNumber);
    void triggerMix(int playingDeckNumber);
    void setStatus(const djapp::autopilot::Status& status);
    // Clears whatever the picker/auto-pick last showed (new watch cycle,
    // off, or a transition just completed).
    void resetCandidates();

    QString brainDbPath() const;

    UserSettingsPointer m_pConfig;
    Library* m_pLibrary;

    ControlPushButton m_enabled;
    ControlProxy m_engineState; // [AutomixTransition],state
    ControlProxy m_recipeSelector; // [AutomixTransition],recipe
    ControlProxy m_triggerToDeck2; // [Channel1],automix_transition_to_2
    ControlProxy m_triggerToDeck1; // [Channel2],automix_transition_to_1
    // Dan, 2026-10-09: "garda de ton si voce nu par sa faca ceva" - these
    // were only ever logged (qInfo), invisible while DJing. Read during
    // StatusKind::Running so the status line says whether each guard is
    // actually protecting this transition.
    ControlProxy m_vocalGuardActiveStatus; // [AutomixTransition],vocal_guard_active
    ControlProxy m_keyGuardActiveStatus; // [AutomixTransition],key_guard_active

    DeckProxies m_deck1;
    DeckProxies m_deck2;

    QTimer m_tickTimer;

    QString m_cachedLocation[2];
    double m_cachedRecipeLengthBeats[2] = {-1.0, -1.0};
    std::optional<double> m_cachedMixOutSec[2];

    int m_lastEngineState = 0;
    int m_pendingFromDeck = 0;
    int m_overrideEmptyDeck = 0;
    int m_awaitingLoadDeck = 0;
    bool m_lastEnabled = false;

    // Picker cache (change 2/3): avoids re-querying brain.db every 500 ms
    // tick while the same lookahead window is open.
    QList<Candidate> m_candidates;
    QString m_candidatesSourceLocation;
    int m_candidatesPlayingDeck = 0;
    int m_candidatesOtherDeck = 0;
    bool m_candidatesQueried = false;

    // What the status line shows for StatusKind::Picked (change 1's risky
    // flag, surfaced in the UI too), set by loadCandidate(), cleared by
    // resetCandidates().
    QString m_pickedLabel;
    bool m_pickedRisky = false;
    // The picked candidate's recipe_hint (Dan, 2026-10-09): triggerMix()
    // flips [AutomixTransition],recipe to this for the one press, then
    // restores Dan's own selection right after - same as him manually
    // picking a different recipe row before pressing MIX himself.
    QString m_pickedRecipeHint;

    DISALLOW_COPY_AND_ASSIGN(DJAppAutopilot);
};
