#pragma once

#include <QDateTime>
#include <QElapsedTimer>
#include <QObject>
#include <QSharedPointer>
#include <QTimer>
#include <optional>
#include <vector>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "control/controlpushbutton.h"
#include "mixer/automixkeyguard.h"
#include "mixer/automixrecipe.h"
#include "mixer/automixtempo.h"
#include "mixer/automixvocalguard.h"
#include "preferences/usersettings.h"
#include "track/track_decl.h"
#include "util/class.h"

class VisualPlayPosition;

// Automix transition engine between deck 1 and deck 2 (Faza 1.5, rebuilt as
// Etapa 0 of docs/plan-automix-v2.md, see docs/decisions/0017).
//
// Dan picks the tracks, cue points and loops; MIX does the transition the
// way he mixes by hand: from the EQs (and optionally the filter), with the
// crossfader centered and both channel faders up. The controller is a
// generic executor: everything musical comes from a recipe
// (mixer/automixrecipe.h) re-read from <settings dir>/automix_recipes.json
// on every MIX press.
//
// Flow:
//  1. MIX press -> armed. Refused (button shows why for 2 s) if the
//     crossfader is not centered, the outgoing deck is not playing, has no
//     beatgrid, or the other deck has no track. Arming exits active loops on
//     both decks, applies the one-shot tempo match, and, if the incoming deck
//     is stopped, presets its EQs (inaudible: it is not playing yet).
//     Pressing again while armed disarms.
//  2. The transition starts on the next bar of the outgoing deck's Mixxx
//     beatgrid (settings.arm_quantum_bars; holding MIX forces the very next
//     bar). A stopped incoming deck is started then (Mixxx aligns its phase
//     on play when quantize is on), so its cue point lands on that bar.
//  3. Lanes run on a beat clock driven by the outgoing deck's grid position,
//     not wall time, so phase changes land on grid downbeats even if the
//     tempo moves (AutomixTransitionMath::clockAdvance()).
//  4. Touching an automated knob hands THAT knob back (per-lane manual
//     takeover); the others keep going. "Reia auto" glides every manual lane
//     back onto its curve.
//  5. Vocal guard (mixer/automixvocalguard.h): with two stem tracks that both
//     have a real voice in brain.db's vocal maps, the vocals stem volume of
//     each deck is an extra lane (multiplying the recipe's stem_vocals lane,
//     if any): the old voice finishes its phrase, then the new one comes in.
//     Switched per transition by [AutomixTransition],vocal_guard.
//  6. Key guard (mixer/automixkeyguard.h): with two stem tracks whose keys
//     clash, the bass, other and vocals stems are extra lanes too: only the
//     incoming drums play until half the recipe, then the harmonic stems
//     swap. On the vocals stem the stricter of the two guards wins.
//     Switched per transition by [AutomixTransition],key_guard.
//  7. Tempo (mixer/automixtempo.h, docs/decisions/0027): with the recipe's
//     tempo mode meet_return, both decks' rate_ratio are lanes too. They
//     glide together to the midpoint tempo over the first bars, so they stay
//     phase-locked, and after the transition the incoming deck glides back to
//     its own tempo (the return outlives the transition: the MIX engine is
//     idle again and can be re-armed, which ends the return). Touching a
//     deck's rate fader hands that deck's tempo back to Dan; "Reia auto"
//     glides it back onto the curve, like any other lane.
//
// Never touched: crossfader, channel faders (unless a recipe enables a
// volume lane), sync (no tempo lanes while a deck has sync on). Hardcoded to [Channel1]/[Channel2] because the
// SeratoLike skin is 2-deck (docs/decisions/0003).
class AutomixTransitionController : public QObject {
    Q_OBJECT
  public:
    // Values of [ChannelN],automix_state, which drives the MIX button's look.
    enum class ButtonState {
        Idle = 0,
        Armed = 1,
        Running = 2,
        RefusedCrossfader = 3,
        RefusedNotPlaying = 4,
        RefusedNoGrid = 5,
        RefusedNoTrack = 6,
    };

    AutomixTransitionController(UserSettingsPointer pConfig, QObject* pParent);
    ~AutomixTransitionController() override;

  private slots:
    void slotTriggerToDeck2(double value);
    void slotTriggerToDeck1(double value);
    void slotLongPress();
    void slotResume(double value);
    void slotClearRefusal();
    void slotTick();

  private:
    enum class State {
        Idle,
        Armed,
        Running,
    };

    struct DeckControls {
        explicit DeckControls(const QString& group);

        ControlProxy* control(AutomixParam param);

        QString group;
        ControlProxy play;
        ControlProxy bpm;
        ControlProxy rateRatio;
        ControlProxy trackSamples;
        ControlProxy volume;
        ControlProxy eqLow;
        ControlProxy eqMid;
        ControlProxy eqHigh;
        // [QuickEffectRack1_[ChannelN]],super1: 0.5 neutral, -> 1.0 HPF, -> 0.0 LPF.
        ControlProxy filter;
        // loop_enabled is a state CO; reloop_toggle (alias reloop_exit)
        // exits an active loop. Only pulsed when a loop is active, so MIX
        // never starts a loop.
        ControlProxy loopEnabled;
        ControlProxy reloopToggle;
        // Mixxx 2.6 stems (docs/decisions/0020): [ChannelN_StemM],volume,
        // linear gain 0..1, applied before the deck EQ. stem_count is 0 for a
        // normal track (then the stem volumes are no-ops).
        ControlProxy stemDrums;
        ControlProxy stemBass;
        ControlProxy stemOther;
        ControlProxy stemVocals;
        ControlProxy stemCount;
        // Sync on a deck owns its rate: the tempo lanes stay off then.
        ControlProxy syncEnabled;
        QSharedPointer<VisualPlayPosition> pVisualPlayPos;
    };

    // One deck's rate_ratio under automation (meet_return): its rate at the
    // transition start times the plan's shared factor. Same takeover rules as
    // LaneRuntime, on its own clock (transition beats, then return beats).
    struct TempoLane {
        ControlProxy* pRate = nullptr;
        double startRate = 1.0;
        double lastWritten = 1.0;
        bool manual = false;
        bool gliding = false;
        double glideFrom = 0.0;
        double glideStartBeat = 0.0;
        double glideBeats = 0.0;
    };

    // The incoming deck gliding back to its own tempo after the transition.
    struct TempoReturn {
        bool active = false;
        AutomixTempoPlan plan;
        int incomingDeckNumber = 0;
        int outgoingDeckNumber = 0;
        TrackPointer pIncomingTrack;
        TrackPointer pOutgoingTrack;
        TempoLane incoming;
        // Follows with the same factor while it still plays its track.
        TempoLane outgoing;
        bool outgoingFollows = false;
        // Incoming beats since the return started (its own grid).
        double beat = 0.0;
        double lastGridBeat = 0.0;
        bool haveLastGridBeat = false;
    };

    struct LaneRuntime {
        AutomixLane lane;
        ControlProxy* pControl = nullptr;
        double startValue = 0.0;
        double lastWritten = 0.0;
        bool manual = false;
        bool gliding = false;
        double glideFrom = 0.0;
        double glideStartBeat = 0.0;
        double glideBeats = 0.0;
        // Vocals stem lane scaled by the vocal guard gain of lane.deck.
        bool vocalGuard = false;
        // Bass/other/vocals stem lane scaled by the key guard gain of lane.deck
        // (bass at the bass swap, other/vocals at the melody swap).
        bool keyGuard = false;
    };

    void onTrigger(int fromDeckNumber, double value);
    void arm(int fromDeckNumber);
    void disarm();
    void start();
    void finish();
    void abort();
    void refuse(int fromDeckNumber, ButtonState reason);
    void setButtonState(int deckNumber, ButtonState state);
    void reloadRecipesIfChanged();
    void writeLanes();
    bool tracksChanged() const;
    // Lane value at `beat`, guard gains included (the smaller one if both).
    double laneTarget(const LaneRuntime& runtime, double beat) const;

    // Arm time: vocal maps of both tracks from brain.db (read-only).
    void loadVocalMaps();
    // Start time: decides whether the guard runs and plans it. Returns why it
    // is off (empty = on).
    QString planVocalGuard();
    // Start time: decides whether the key guard runs (stems on both decks,
    // both keys known and clashing) and plans it. Returns why it is off.
    // Called after planVocalGuard(): uses m_vocalGuardPlan when the vocal
    // guard is active (melody swap at the vocal handover).
    QString planKeyGuard();
    // A transition cancelled by a track change must not leave a stem muted
    // on a deck that keeps playing: guard lanes go back to their curve value
    // without the guard gain (decks with a new track are left alone, Mixxx
    // resets their stems on load).
    void releaseGuards();

    // Start time: the meet_return schedule from both decks' tempo now. Leaves
    // m_tempoPlan empty (and logs why) when no tempo lanes run.
    void planTempo();
    // Writes `target` to an automated rate lane (detecting a takeover first,
    // gliding after "Reia auto"). False if the lane is manual.
    bool writeTempoLane(TempoLane* pLane, double target, double clockBeat);
    void resumeTempoLane(TempoLane* pLane, double clockBeat);
    // finish(): hands the incoming tempo over to the return glide.
    void startTempoReturn();
    void tickTempoReturn(double dtSeconds);
    void stopTempoReturn(const QString& reason);
    // " tempo X BPM, phase in-out Y ms" for the per-bar log line: the live
    // beatmatch check through the ramps.
    QString tempoLogNote();

    // Outgoing deck position in beats from its grid anchor (Mixxx's first
    // downbeat = beat 0), fractional. False if there is no track, beatgrid
    // or valid play position.
    bool readGridBeat(DeckControls& deck, double* pBeat) const;

    DeckControls& deck(int deckNumber) {
        return deckNumber == 1 ? m_deck1 : m_deck2;
    }
    DeckControls& outgoingDeck() {
        return deck(m_fromDeckNumber);
    }
    DeckControls& incomingDeck() {
        return deck(m_fromDeckNumber == 1 ? 2 : 1);
    }

    UserSettingsPointer m_pConfig;
    QString m_recipeFilePath;
    QDateTime m_recipeFileModified;
    qint64 m_recipeFileSize = -1;
    AutomixRecipeBook m_book;

    DeckControls m_deck1;
    DeckControls m_deck2;
    ControlProxy m_crossfader;

    // [Channel1],automix_transition_to_2 / [Channel2],automix_transition_to_1
    ControlPushButton m_triggerToDeck2;
    ControlPushButton m_triggerToDeck1;
    // [ChannelN],automix_state -- see ButtonState.
    ControlObject m_buttonStateDeck1;
    ControlObject m_buttonStateDeck2;
    // [AutomixTransition],state: 0 idle, 1 armed, 2 running.
    ControlObject m_engineState;
    // [AutomixTransition],countdown: armed -> beats until start, running ->
    // bars left.
    ControlObject m_countdown;
    // [AutomixTransition],manual: 1 while any lane is under manual control.
    ControlObject m_manual;
    // [AutomixTransition],resume: "Reia auto".
    ControlPushButton m_resume;
    // [AutomixTransition],recipe: selector slot (AutomixRecipeBook::selectorIds()),
    // persisted, cycled by the skin button.
    ControlPushButton m_recipeSelector;
    // [AutomixTransition],vocal_guard: 1 = guard on ("GARDA VOCE", default),
    // 0 = voices may overlap ("VOCE LIBERA"). Persisted, read when a
    // transition starts.
    ControlPushButton m_vocalGuardToggle;
    // [AutomixTransition],key_guard: 1 = "GARDA TON" (default), 0 = "TON
    // LIBER" (EQ blend even when the keys clash). Persisted.
    ControlPushButton m_keyGuardToggle;

    QTimer m_tickTimer;
    QTimer m_longPressTimer;
    QTimer m_refusalTimer;
    QElapsedTimer m_tickClock;

    State m_state = State::Idle;
    int m_fromDeckNumber = 0;
    int m_pressedDeckNumber = 0;
    // Copy of the recipe in use, so a hot reload never changes a running
    // transition.
    AutomixRecipe m_recipe;
    double m_armGridBeat = 0.0;
    double m_startGridBeat = 0.0;
    double m_lastGridBeat = 0.0;
    double m_transitionBeat = 0.0;
    // Recipe length, stretched if a guard needs longer (only a recipe
    // shorter than four guard fades).
    double m_runLengthBeats = 0.0;
    bool m_incomingNeedsPlay = false;
    TrackPointer m_pOutgoingTrack;
    TrackPointer m_pIncomingTrack;
    std::vector<LaneRuntime> m_lanes;
    QString m_brainDbPath;
    std::optional<AutomixVocalMap> m_outgoingVocalMap;
    std::optional<AutomixVocalMap> m_incomingVocalMap;
    bool m_vocalGuardActive = false;
    AutomixVocalGuardPlan m_vocalGuardPlan;
    bool m_keyGuardActive = false;
    AutomixKeyGuardPlan m_keyGuardPlan;
    // meet_return during the transition (empty = no tempo lanes).
    std::optional<AutomixTempoPlan> m_tempoPlan;
    TempoLane m_outgoingTempo;
    TempoLane m_incomingTempo;
    TempoReturn m_tempoReturn;

    DISALLOW_COPY_AND_ASSIGN(AutomixTransitionController);
};
