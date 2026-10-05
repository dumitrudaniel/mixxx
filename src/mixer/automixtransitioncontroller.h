#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <memory>

#include "control/controlproxy.h"
#include "control/controlpushbutton.h"
#include "util/class.h"

// Faza 1.5 -- Automix semiauto (see PLAN.md).
//
// Self-contained, Mixxx-C++-only automation of the mechanical part of a DJ
// transition between deck 1 and deck 2: crossfader sweep (the sole primary
// volume blend) + low-band-only EQ swap (tonal, avoids bass mud) + filter
// sweep + enabling Mixxx's own Sync engine on the incoming deck. Cue points,
// loops and track choice remain fully manual (native Mixxx hotcues/loops) --
// this class only drives the fader/EQ/filter/sync mechanics once Dan presses
// one of the two trigger buttons it exposes.
//
// 2026-10-05: upgraded from the original MVP (crossfader sweep + instant
// low-band-only swap at the midpoint) to continuous 3-band EQ + filter sweep,
// then the mid/high portion of that EQ automation was REMOVED again later
// the same day after Dan reported a volume "jump" around the transition
// midpoint -- see docs/decisions/0008 addenda. Root cause: crossfader gain
// (constant-power) and mid/high EQ gain were both independently attenuating
// each deck's volume at the same time, compounding multiplicatively into a
// real dip at progress=0.5 that read as a jump on recovery. Mid/high bands
// are now left untouched (unity gain, never written by this class) for the
// whole transition -- the crossfader alone carries their presence. Only the
// low band keeps its own front-loaded swap curve (AutomixTransitionMath::
// outgoing/incomingBassGainForProgress), which is NOT redundant with the
// crossfader: it addresses bass-mud between two simultaneously-playing
// basslines, a problem that exists regardless of their relative crossfader
// volume.
//
// Explicitly out of scope for this MVP (Dan's approval, 2026-10-04):
//  - Duration selection (hardcoded 2 bars as of 2026-10-05, was 16 bars).
//  - Any brain/ (Python) involvement -- this lives entirely inside Mixxx.
//
// Hardcoded to the 2-deck [Channel1]/[Channel2] case because the SeratoLike
// skin itself is 2-deck only (see docs/decisions/0003). See docs/decisions/0008
// for the full design writeup, including the manual-override detection
// strategy (ControlObject has no "claim exclusive ownership" primitive, so
// this class remembers the last value *it* wrote to each automated control
// and treats any observed divergence as a human touching it).
class AutomixTransitionController : public QObject {
    Q_OBJECT
  public:
    explicit AutomixTransitionController(QObject* pParent);
    ~AutomixTransitionController() override = default;

  private slots:
    void slotTriggerToDeck2(double value);
    void slotTriggerToDeck1(double value);
    void slotTick();

  private:
    struct DeckControls {
        DeckControls(const QString& group);

        QString group;
        ControlProxy bpm;
        ControlProxy volume;
        // Low band only -- mid/high are deliberately left untouched (unity
        // gain) for the whole transition, see the class comment above for
        // why. No eqMidGain/eqHighGain members: this class never reads or
        // writes those COs anymore.
        ControlProxy eqLowGain;
        // EXPERIMENTAL (2026-10-05, see docs/decisions/0008 addendum): mid
        // band, re-added solely for the symmetric mid-scoop effect
        // (AutomixTransitionMath::midScoopGainForProgress). Only
        // read/written when kMidScoopDepth > 0.0 -- see writeMidScoop() and
        // wasManuallyOverridden() below. No eqHighGain: the scoop is
        // deliberately mid-only, see automixtransitionmath.h.
        ControlProxy eqMidGain;
        // Quick-filter ("Filter" knob), [QuickEffectRack1_[ChannelN]],super1
        // -- confirmed in source (effects/backends/builtin/filtereffect.cpp),
        // not the skin's separate EQ knobs. See automixtransitionmath.h for
        // the confirmed value range/semantics (0.5 neutral, 1.0 full
        // high-pass, 0.0 full low-pass).
        ControlProxy filter;
        ControlProxy syncEnabled;
        ControlProxy syncLeader;
    };

    // fromDeckNumber is 1 or 2; the other deck is the transition target.
    void startTransition(int fromDeckNumber);
    void cancelTransition(const char* reason);
    void finishTransition();
    // Disables sync_enabled on the deck that was synced-in and clears
    // sync_leader on the other one, so neither transition exit path leaves
    // a deck permanently phase/tempo-locked to its partner. See
    // docs/decisions/0008 addendum (2026-10-05, SYNC left engaged bug).
    void releaseSyncLock();

    // Returns true if any automated control (crossfader, either deck's low
    // EQ, either deck's filter) or any watched-but-not-written control
    // (either deck's volume) has a current value that no longer matches what
    // this class last wrote / observed as the baseline -- i.e. a human
    // touched it. Mid/high EQ is deliberately NOT monitored: this class
    // never writes those COs, so a human touching them during a transition
    // is an independent action, not an override of our automation.
    bool wasManuallyOverridden() const;

    void writeCrossfader(double value);
    // Low band only (front-loaded curve, see AutomixTransitionMath::
    // outgoing/incomingBassGainForProgress). Mid/high are never written.
    void writeOutgoingBass(double bassValue);
    void writeIncomingBass(double bassValue);
    void writeOutgoingFilter(double value);
    void writeIncomingFilter(double value);
    // EXPERIMENTAL mid scoop (see automixtransitionmath.h kMidScoopDepth doc
    // comment). No-ops (does not touch the CO at all) when
    // AutomixTransitionMath::kMidScoopDepth <= 0.0, so setting that constant
    // to 0.0 truly disables the effect rather than just writing a no-op
    // value over whatever a human last set manually.
    void writeOutgoingMidScoop(double value);
    void writeIncomingMidScoop(double value);

    DeckControls m_deck1;
    DeckControls m_deck2;
    ControlProxy m_crossfader;

    // New trigger ControlObjects exposed to the skin/controllers:
    //   [Channel1],automix_transition_to_2  -- starts deck1 -> deck2
    //   [Channel2],automix_transition_to_1  -- starts deck2 -> deck1
    ControlPushButton m_triggerToDeck2;
    ControlPushButton m_triggerToDeck1;

    QTimer m_timer;
    QElapsedTimer m_elapsed;

    bool m_active = false;
    int m_fromDeckNumber = 0; // 1 or 2 while active, 0 when idle
    double m_durationSeconds = 0.0;

    // See automixtransitioncontroller.cpp (startTransition) for why this
    // exists: enabling sync on the incoming deck is deferred by one tick
    // (50ms) after requesting leader status on the outgoing deck, instead of
    // writing both COs back-to-back in the same call, to avoid a real
    // cross-channel race in Mixxx's own EngineSync::pickLeader() that is
    // direction-dependent (only occurs for the deck2->deck1 transition).
    bool m_syncHandoffPending = false;

    // Last values *this class* wrote, for manual-override detection.
    double m_lastWrittenCrossfader = 0.0;
    // Low (bass) moves on its own front-loaded curve -- see
    // AutomixTransitionMath, 2026-10-05 bass-staggering addendum. Mid/high
    // have no equivalent here: they are never written (see class comment).
    double m_lastWrittenOutgoingBass = 0.0;
    double m_lastWrittenIncomingBass = 0.0;
    double m_lastWrittenOutgoingFilter = 0.0;
    double m_lastWrittenIncomingFilter = 0.0;
    // EXPERIMENTAL mid scoop baselines -- only meaningful/monitored when
    // AutomixTransitionMath::kMidScoopDepth > 0.0, see
    // wasManuallyOverridden().
    double m_lastWrittenOutgoingMidScoop = 0.0;
    double m_lastWrittenIncomingMidScoop = 0.0;

    // Baseline volumes captured at transition start (never written by this
    // class, but watched -- a touch here must also cancel the transition).
    double m_baselineOutgoingVolume = 0.0;
    double m_baselineIncomingVolume = 0.0;

    DISALLOW_COPY_AND_ASSIGN(AutomixTransitionController);
};
