#include "mixer/automixtransitioncontroller.h"

#include "mixer/automixtransitionmath.h"
#include "moc_automixtransitioncontroller.cpp"
#include "util/math.h"

namespace {
// Tick rate for the crossfader/EQ interpolation. 50ms (20Hz) is smooth enough
// for a fader move and cheap enough to not matter -- this is UI-rate
// automation, not an audio-rate DSP process (that stays entirely inside
// Mixxx's existing engine buffer callbacks, untouched by this class).
constexpr int kTimerIntervalMs = 50;

// Tolerance for comparing a CO's current value against the value this class
// last wrote (or, for volume, the baseline captured at transition start).
// ControlObject values are doubles; a human touching a slider/knob always
// produces a visible jump well above float rounding noise.
constexpr double kOverrideTolerance = 1e-6;

bool diverged(double a, double b) {
    return std::abs(a - b) > kOverrideTolerance;
}
} // namespace

AutomixTransitionController::DeckControls::DeckControls(const QString& group)
        : group(group),
          bpm(group, QStringLiteral("bpm")),
          volume(group, QStringLiteral("volume")),
          eqLowGain(QStringLiteral("[EqualizerRack1_") + group + QStringLiteral("_Effect1]"),
                  QStringLiteral("parameter1")),
          syncEnabled(group, QStringLiteral("sync_enabled")),
          syncLeader(group, QStringLiteral("sync_leader")) {
}

AutomixTransitionController::AutomixTransitionController(QObject* pParent)
        : QObject(pParent),
          m_deck1(QStringLiteral("[Channel1]")),
          m_deck2(QStringLiteral("[Channel2]")),
          m_crossfader(QStringLiteral("[Master]"), QStringLiteral("crossfader")),
          m_triggerToDeck2(ConfigKey(QStringLiteral("[Channel1]"),
                  QStringLiteral("automix_transition_to_2"))),
          m_triggerToDeck1(ConfigKey(QStringLiteral("[Channel2]"),
                  QStringLiteral("automix_transition_to_1"))) {
    connect(&m_triggerToDeck2,
            &ControlPushButton::valueChanged,
            this,
            &AutomixTransitionController::slotTriggerToDeck2);
    connect(&m_triggerToDeck1,
            &ControlPushButton::valueChanged,
            this,
            &AutomixTransitionController::slotTriggerToDeck1);

    m_timer.setInterval(kTimerIntervalMs);
    connect(&m_timer, &QTimer::timeout, this, &AutomixTransitionController::slotTick);
}

void AutomixTransitionController::slotTriggerToDeck2(double value) {
    if (value > 0.0) {
        startTransition(1);
    }
}

void AutomixTransitionController::slotTriggerToDeck1(double value) {
    if (value > 0.0) {
        startTransition(2);
    }
}

void AutomixTransitionController::startTransition(int fromDeckNumber) {
    if (m_active) {
        // A transition is already running; ignore re-triggers (including the
        // opposite direction) rather than guessing what the user wants.
        return;
    }

    DeckControls& outgoing = (fromDeckNumber == 1) ? m_deck1 : m_deck2;
    DeckControls& incoming = (fromDeckNumber == 1) ? m_deck2 : m_deck1;

    const double outgoingBpm = outgoing.bpm.get();
    const double duration = AutomixTransitionMath::transitionDurationSeconds(outgoingBpm);
    if (duration <= 0.0) {
        // No usable BPM on the outgoing deck (no track loaded/analyzed yet).
        // Refuse to start rather than automate against garbage timing.
        return;
    }

    m_active = true;
    m_fromDeckNumber = fromDeckNumber;
    m_durationSeconds = duration;
    m_baselineOutgoingVolume = outgoing.volume.get();
    m_baselineIncomingVolume = incoming.volume.get();

    // Tempo matching: reuse Mixxx's own Sync engine rather than building
    // custom tempo ramping. Make the outgoing deck the explicit sync leader,
    // then enable sync on the incoming deck so it syncs to that leader.
    // (SyncLeaderLight::Explicit == 2, see engine/sync/syncable.h.)
    outgoing.syncLeader.set(2.0);
    incoming.syncEnabled.set(1.0);

    // Write the progress-0 state immediately so the transition starts from a
    // known point and the override baseline below is consistent with what we
    // just wrote.
    writeCrossfader(AutomixTransitionMath::crossfaderForProgress(0.0, fromDeckNumber == 1));
    writeOutgoingBass(AutomixTransitionMath::outgoingBassGainForProgress(0.0));
    writeIncomingBass(AutomixTransitionMath::incomingBassGainForProgress(0.0));

    m_elapsed.start();
    m_timer.start();
}

void AutomixTransitionController::slotTick() {
    if (!m_active) {
        return;
    }

    if (wasManuallyOverridden()) {
        cancelTransition("manual touch of crossfader/EQ/volume detected");
        return;
    }

    const double elapsedSeconds = m_elapsed.elapsed() / 1000.0;
    const double progress = AutomixTransitionMath::progressForElapsed(
            elapsedSeconds, m_durationSeconds);

    const bool fromDeck1 = (m_fromDeckNumber == 1);
    writeCrossfader(AutomixTransitionMath::crossfaderForProgress(progress, fromDeck1));
    writeOutgoingBass(AutomixTransitionMath::outgoingBassGainForProgress(progress));
    writeIncomingBass(AutomixTransitionMath::incomingBassGainForProgress(progress));

    if (progress >= 1.0) {
        finishTransition();
    }
}

bool AutomixTransitionController::wasManuallyOverridden() const {
    DeckControls const& outgoing = (m_fromDeckNumber == 1) ? m_deck1 : m_deck2;
    DeckControls const& incoming = (m_fromDeckNumber == 1) ? m_deck2 : m_deck1;

    if (diverged(m_crossfader.get(), m_lastWrittenCrossfader)) {
        return true;
    }
    if (diverged(outgoing.eqLowGain.get(), m_lastWrittenOutgoingBass)) {
        return true;
    }
    if (diverged(incoming.eqLowGain.get(), m_lastWrittenIncomingBass)) {
        return true;
    }
    // Volume faders are never written by this class, but touching them
    // during a transition must still cancel it per the safety requirement.
    if (diverged(outgoing.volume.get(), m_baselineOutgoingVolume)) {
        return true;
    }
    if (diverged(incoming.volume.get(), m_baselineIncomingVolume)) {
        return true;
    }
    return false;
}

void AutomixTransitionController::writeCrossfader(double value) {
    m_crossfader.set(value);
    m_lastWrittenCrossfader = value;
}

void AutomixTransitionController::writeOutgoingBass(double value) {
    DeckControls& outgoing = (m_fromDeckNumber == 1) ? m_deck1 : m_deck2;
    outgoing.eqLowGain.set(value);
    m_lastWrittenOutgoingBass = value;
}

void AutomixTransitionController::writeIncomingBass(double value) {
    DeckControls& incoming = (m_fromDeckNumber == 1) ? m_deck2 : m_deck1;
    incoming.eqLowGain.set(value);
    m_lastWrittenIncomingBass = value;
}

void AutomixTransitionController::finishTransition() {
    m_timer.stop();
    m_active = false;
    m_fromDeckNumber = 0;
}

void AutomixTransitionController::cancelTransition(const char* reason) {
    Q_UNUSED(reason);
    // Hand control back immediately: stop touching any control at all and
    // leave everything exactly where the human just put it. No snap-back,
    // no further writes -- that's the point of "hands it back immediately".
    m_timer.stop();
    m_active = false;
    m_fromDeckNumber = 0;
}
