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
          // parameter1/2/3 on the per-deck EqualizerRack1 effect == Low/Mid/
          // High respectively -- confirmed from the skin's EQ knob mapping
          // (mixer/channel_left.xml: EqParameter 1/2/3 -> Low/Mid/High via
          // mixer/eq_knob_left.xml's <ConfigKey>...,parameter<EqParameter>).
          eqLowGain(QStringLiteral("[EqualizerRack1_") + group + QStringLiteral("_Effect1]"),
                  QStringLiteral("parameter1")),
          // parameter2 (Mid) -- EXPERIMENTAL, re-added 2026-10-05 solely for
          // the symmetric mid-scoop effect (see automixtransitionmath.h,
          // kMidScoopDepth). No parameter3 (High) member -- the scoop is
          // deliberately mid-only, and high/low keep their existing
          // treatment (high untouched, low has its own bass-swap curve).
          eqMidGain(QStringLiteral("[EqualizerRack1_") + group + QStringLiteral("_Effect1]"),
                  QStringLiteral("parameter2")),
          // Quick-filter ("Filter" knob) -- confirmed from the skin
          // (mixer/quick_effect_knob_left.xml: KnobComposed bound to
          // <QuickEffectGroup>,super1) and from effects/effectchain.cpp
          // (m_pControlChainSuperParameter is a ControlPotmeter with range
          // [0.0, 1.0]). This is the single combined LPF/HPF "Filter" knob,
          // NOT the 3-band EQ knobs above.
          filter(QStringLiteral("[QuickEffectRack1_") + group + QStringLiteral("]"),
                  QStringLiteral("super1")),
          rateRatio(group, QStringLiteral("rate_ratio")),
          loopEnabled(group, QStringLiteral("loop_enabled")),
          reloopToggle(group, QStringLiteral("reloop_toggle")) {
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

    // Drop any active loop on BOTH decks before automating anything else
    // (2026-10-06, Dan's request): a transition shouldn't start with either
    // deck stuck repeating a loop. Only pulses reloop_toggle when
    // loop_enabled is actually 1, so a deck with no active loop is left
    // alone -- this never ACTIVATES a loop, only exits one already running.
    if (outgoing.loopEnabled.get() > 0.0) {
        outgoing.reloopToggle.set(1.0);
    }
    if (incoming.loopEnabled.get() > 0.0) {
        incoming.reloopToggle.set(1.0);
    }

    // Tempo matching (REWORKED 2026-10-05, see docs/decisions/0008 "snap to
    // grid" addendum): a single one-shot rate_ratio write on the incoming
    // deck, NOT Mixxx's Sync engine. The previous approach (outgoing.
    // syncLeader.set(2.0) + a deferred incoming.syncEnabled.set(1.0)) got
    // tempo matching "for free" from Sync, but Sync's tempo lock is
    // continuous and inseparable from its own continuous beatgrid-phase
    // correction -- it kept forcibly re-aligning the incoming deck's beat
    // position to the leader's beatgrid for as long as it stayed enabled,
    // silently overwriting any manual beatmatching/pitch-bend Dan had
    // already dialed in on the incoming track before pressing MIX. That is
    // the bug this rework fixes. Setting rate_ratio only ever changes
    // playback speed; it never reads or writes beat position, so it is
    // physically incapable of fighting manual alignment, and -- critically
    // -- it is applied exactly once, right here, never re-applied on any
    // later tick, so there is no ongoing lock of any kind for Dan's manual
    // touch to fight afterward either.
    const double tempoMatchedRatio = AutomixTransitionMath::tempoMatchedIncomingRateRatio(
            outgoing.bpm.get(), incoming.bpm.get(), incoming.rateRatio.get());
    if (tempoMatchedRatio > 0.0) {
        incoming.rateRatio.set(tempoMatchedRatio);
    }
    // else: incoming deck has no usable bpm/rate_ratio reading (e.g. no
    // track loaded/analyzed) -- skip tempo matching rather than writing a
    // garbage rate_ratio; the rest of the transition (crossfader/EQ/filter)
    // still proceeds, same as before this rework.

    // Write the progress-0 state immediately so the transition starts from a
    // known point and the override baseline below is consistent with what we
    // just wrote.
    writeCrossfader(AutomixTransitionMath::crossfaderForProgress(0.0, fromDeckNumber == 1));
    writeOutgoingBass(AutomixTransitionMath::outgoingBassGainForProgress(0.0));
    writeIncomingBass(AutomixTransitionMath::incomingBassGainForProgress(0.0));
    writeOutgoingFilter(AutomixTransitionMath::outgoingFilterForProgress(0.0));
    writeIncomingFilter(AutomixTransitionMath::incomingFilterForProgress(0.0));
    if (AutomixTransitionMath::kMidScoopDepth > 0.0) {
        const double midScoop = AutomixTransitionMath::midScoopGainForProgress(0.0);
        writeOutgoingMidScoop(midScoop);
        writeIncomingMidScoop(midScoop);
    }

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
    writeOutgoingFilter(AutomixTransitionMath::outgoingFilterForProgress(progress));
    writeIncomingFilter(AutomixTransitionMath::incomingFilterForProgress(progress));
    if (AutomixTransitionMath::kMidScoopDepth > 0.0) {
        const double midScoop = AutomixTransitionMath::midScoopGainForProgress(progress);
        writeOutgoingMidScoop(midScoop);
        writeIncomingMidScoop(midScoop);
    }

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
    if (diverged(outgoing.filter.get(), m_lastWrittenOutgoingFilter)) {
        return true;
    }
    if (diverged(incoming.filter.get(), m_lastWrittenIncomingFilter)) {
        return true;
    }
    // EXPERIMENTAL mid scoop -- only monitored while actually enabled
    // (kMidScoopDepth > 0.0); when disabled this class never writes
    // eqMidGain at all, so a human's own mid setting is simply not our
    // business.
    if (AutomixTransitionMath::kMidScoopDepth > 0.0) {
        if (diverged(outgoing.eqMidGain.get(), m_lastWrittenOutgoingMidScoop)) {
            return true;
        }
        if (diverged(incoming.eqMidGain.get(), m_lastWrittenIncomingMidScoop)) {
            return true;
        }
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

void AutomixTransitionController::writeOutgoingBass(double bassValue) {
    DeckControls& outgoing = (m_fromDeckNumber == 1) ? m_deck1 : m_deck2;
    outgoing.eqLowGain.set(bassValue);
    m_lastWrittenOutgoingBass = bassValue;
}

void AutomixTransitionController::writeIncomingBass(double bassValue) {
    DeckControls& incoming = (m_fromDeckNumber == 1) ? m_deck2 : m_deck1;
    incoming.eqLowGain.set(bassValue);
    m_lastWrittenIncomingBass = bassValue;
}

void AutomixTransitionController::writeOutgoingFilter(double value) {
    DeckControls& outgoing = (m_fromDeckNumber == 1) ? m_deck1 : m_deck2;
    outgoing.filter.set(value);
    m_lastWrittenOutgoingFilter = value;
}

void AutomixTransitionController::writeIncomingFilter(double value) {
    DeckControls& incoming = (m_fromDeckNumber == 1) ? m_deck2 : m_deck1;
    incoming.filter.set(value);
    m_lastWrittenIncomingFilter = value;
}

void AutomixTransitionController::writeOutgoingMidScoop(double value) {
    DeckControls& outgoing = (m_fromDeckNumber == 1) ? m_deck1 : m_deck2;
    outgoing.eqMidGain.set(value);
    m_lastWrittenOutgoingMidScoop = value;
}

void AutomixTransitionController::writeIncomingMidScoop(double value) {
    DeckControls& incoming = (m_fromDeckNumber == 1) ? m_deck2 : m_deck1;
    incoming.eqMidGain.set(value);
    m_lastWrittenIncomingMidScoop = value;
}

void AutomixTransitionController::finishTransition() {
    // No sync state to release anymore (2026-10-05 rework, see the class
    // comment in the header): tempo matching is a single one-shot
    // rate_ratio write at transition start, not a continuously-held engine
    // lock, so there is nothing left engaged on either deck to clean up
    // here. The incoming deck's rate_ratio simply stays at whatever value
    // this class (or Dan) last set it to -- exactly like a human pitch-bend
    // would behave after letting go of the pitch fader.
    m_timer.stop();
    m_active = false;
    m_fromDeckNumber = 0;
}

void AutomixTransitionController::cancelTransition(const char* reason) {
    Q_UNUSED(reason);
    // Hand control back immediately: stop touching the crossfader/EQ/filter
    // at all and leave those exactly where the human just put it -- no
    // snap-back there, that's the point of "hands it back immediately".
    // No sync state to release (see finishTransition() above and the
    // 2026-10-05 header comment) -- the one-shot rate_ratio tempo match
    // already happened (if at all) before this transition could even reach
    // slotTick()'s override check, and leaving it applied is correct: it is
    // indistinguishable from a manual pitch-bend Dan could have done
    // himself, not an ongoing lock that needs releasing.
    m_timer.stop();
    m_active = false;
    m_fromDeckNumber = 0;
}
