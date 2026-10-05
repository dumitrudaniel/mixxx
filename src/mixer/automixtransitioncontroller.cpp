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
    // custom tempo ramping. Request leader status on the outgoing deck, then
    // (one tick later, see slotTick()) enable sync on the incoming deck so
    // it syncs to that leader.
    //
    // NOTE on the actual CO semantics (verified in source, not assumed):
    // SyncControl::slotSyncLeaderEnabledChangeRequest()
    // (engine/sync/synccontrol.cpp) explicitly disables true
    // SyncMode::LeaderExplicit for any externally-requested value > 0 --
    // this is a deliberate upstream workaround for known bugs
    // (mixxxdj/mixxx#11788), documented right there in the comment next to
    // it. Writing 2.0 (SyncLeaderLight::Explicit) here does NOT make Mixxx
    // enter LeaderExplicit; it is coerced into SyncMode::LeaderSoft, exactly
    // like writing 1.0 would be. We still write 2.0 because that is the
    // "please make me leader" request the CO is designed to accept --
    // SyncLeaderLight::Explicit is just its parameter space, not a promise
    // of what SyncMode results -- but don't assume elsewhere in this file
    // that the outgoing deck ends up in LeaderExplicit, because it won't.
    //
    // NOTE on why incoming's sync_enabled write is deferred to the next tick
    // instead of being written here, synchronously, right after
    // syncLeader.set(): both writes are deferred by EngineBuffer until each
    // channel's own next processSyncRequests() call (because both decks are
    // playing -- see EngineBuffer::requestSyncMode/requestEnableSync), and
    // channels are drained in fixed registration order (deck 1 before deck
    // 2), NOT in the order we call .set(). For the deck1->deck2 direction
    // that's harmless (deck 1/outgoing drains first and cleanly claims
    // leadership). For deck2->deck1, deck 1/incoming drains FIRST --
    // EngineSync::pickLeader() (engine/sync/enginesync.cpp) skips any
    // non-triggering deck that isn't yet isSynchronized(), so at that moment
    // it sees only the incoming deck as a candidate and self-elects it as
    // leader, moments before the outgoing deck's queued request lands and
    // forcibly demotes it back to Follower. The final state is correct
    // either way, but the incoming deck's sync COs take two writes
    // (None -> LeaderSoft -> Follower) within the same audio callback for
    // that one direction -- a transient a human single-button click never
    // produces. Waiting a full tick (50ms, several audio buffers) before
    // touching the incoming deck's sync_enabled guarantees the outgoing
    // deck's leader request has already landed and stabilized, so
    // pickLeader() sees it as synchronized immediately and never
    // self-elects the incoming deck. See docs/decisions/0008 addendum.
    outgoing.syncLeader.set(2.0);
    m_syncHandoffPending = true;

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

    if (m_syncHandoffPending) {
        // See startTransition() for why this is deferred to here (one tick
        // after requesting leader status on the outgoing deck) instead of
        // being written synchronously in the same call.
        DeckControls& incoming = (m_fromDeckNumber == 1) ? m_deck2 : m_deck1;
        incoming.syncEnabled.set(1.0);
        m_syncHandoffPending = false;
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
    releaseSyncLock();
    m_timer.stop();
    m_active = false;
    m_fromDeckNumber = 0;
    m_syncHandoffPending = false;
}

void AutomixTransitionController::cancelTransition(const char* reason) {
    Q_UNUSED(reason);
    // Hand control back immediately: stop touching the crossfader/EQ/filter
    // at all and leave those exactly where the human just put it -- no
    // snap-back there, that's the point of "hands it back immediately".
    // Sync is the one exception: startTransition() may already have set
    // sync_enabled=1 on the incoming deck (and sync_leader on the outgoing
    // one) before the cancellation happened, and leaving that engaged would
    // permanently lock the deck to the other one's tempo/phase -- silently
    // fighting every manual jog/pitch nudge from then on. Release it so
    // "hands control back" is actually true for tempo too, not just the
    // faders.
    releaseSyncLock();
    m_timer.stop();
    m_active = false;
    m_fromDeckNumber = 0;
    m_syncHandoffPending = false;
}

void AutomixTransitionController::releaseSyncLock() {
    if (m_fromDeckNumber == 0) {
        return; // Nothing was ever touched this transition (e.g. refused at start).
    }
    DeckControls& outgoing = (m_fromDeckNumber == 1) ? m_deck1 : m_deck2;
    DeckControls& incoming = (m_fromDeckNumber == 1) ? m_deck2 : m_deck1;
    // Only the incoming deck's sync_enabled actively keeps correcting phase
    // (that's what makes manual beatmatching feel "impossible" afterward,
    // per Dan's live-test report). The outgoing deck's sync_leader doesn't
    // itself lock anything once no other deck is following it, but clear it
    // too for a clean, fully-manual handback on both sides.
    incoming.syncEnabled.set(0.0);
    outgoing.syncLeader.set(0.0);
}
