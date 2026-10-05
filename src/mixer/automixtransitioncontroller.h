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
// transition between deck 1 and deck 2: crossfader sweep + bass swap at the
// midpoint + enabling Mixxx's own Sync engine on the incoming deck. Cue
// points, loops and track choice remain fully manual (native Mixxx hotcues/
// loops) -- this class only drives the fader/EQ/sync mechanics once Dan
// presses one of the two trigger buttons it exposes.
//
// Explicitly out of scope for this MVP (Dan's approval, 2026-10-04):
//  - Duration selection (hardcoded 2 bars as of 2026-10-05, was 16 bars).
//  - Filter sweep.
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
        ControlProxy eqLowGain;
        ControlProxy syncEnabled;
        ControlProxy syncLeader;
    };

    // fromDeckNumber is 1 or 2; the other deck is the transition target.
    void startTransition(int fromDeckNumber);
    void cancelTransition(const char* reason);
    void finishTransition();

    // Returns true if any automated control (crossfader, either deck's low
    // EQ) or any watched-but-not-written control (either deck's volume) has
    // a current value that no longer matches what this class last wrote /
    // observed as the baseline -- i.e. a human touched it.
    bool wasManuallyOverridden() const;

    void writeCrossfader(double value);
    void writeOutgoingBass(double value);
    void writeIncomingBass(double value);

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
    double m_lastWrittenOutgoingBass = 0.0;
    double m_lastWrittenIncomingBass = 0.0;

    // Baseline volumes captured at transition start (never written by this
    // class, but watched -- a touch here must also cancel the transition).
    double m_baselineOutgoingVolume = 0.0;
    double m_baselineIncomingVolume = 0.0;

    DISALLOW_COPY_AND_ASSIGN(AutomixTransitionController);
};
