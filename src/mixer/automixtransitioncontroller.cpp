#include "mixer/automixtransitioncontroller.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtDebug>
#include <cmath>

#include "mixer/automixtransitionmath.h"
#include "mixer/playerinfo.h"
#include "moc_automixtransitioncontroller.cpp"
#include "track/track.h"
#include "waveform/visualplayposition.h"

namespace {
const QString kEngineGroup = QStringLiteral("[AutomixTransition]");
const QString kRecipeFileName = QStringLiteral("automix_recipes.json");

// 50 Hz: a one-beat bass swap at 105 BPM lasts ~0.57 s, i.e. ~28 steps.
constexpr int kTickIntervalMs = 20;
// Holding MIX this long forces the start onto the very next bar.
constexpr int kLongPressMs = 450;
// How long a refusal reason stays on the MIX button.
constexpr int kRefusalDisplayMs = 2000;
// A press this late after a bar boundary still starts on that boundary.
constexpr double kStartGraceBeats = 0.1;
// MIX mixes from the EQs with the crossfader centered; it refuses rather than
// moving a crossfader someone left off-center.
constexpr double kCrossfaderCenterTolerance = 0.05;
// A knob whose value differs from what this class last wrote by more than
// this was touched by a human (any knob/mouse step is far larger).
constexpr double kOverrideTolerance = 1e-4;

bool diverged(double a, double b) {
    return std::abs(a - b) > kOverrideTolerance;
}

double stateValue(AutomixTransitionController::ButtonState state) {
    return static_cast<double>(static_cast<int>(state));
}
// "[Channel1]" + 4 -> "[Channel1_Stem4]" (Mixxx 2.6 stem groups, file order).
QString stemGroup(const QString& deckGroup, int stemNumber) {
    return deckGroup.left(deckGroup.size() - 1) + QStringLiteral("_Stem%1]").arg(stemNumber);
}
} // namespace

AutomixTransitionController::DeckControls::DeckControls(const QString& group)
        : group(group),
          play(group, QStringLiteral("play")),
          bpm(group, QStringLiteral("bpm")),
          rateRatio(group, QStringLiteral("rate_ratio")),
          trackSamples(group, QStringLiteral("track_samples")),
          volume(group, QStringLiteral("volume")),
          // parameter1/2/3 of the per-deck EqualizerRack1 effect == Low/Mid/High
          // (skin: mixer/eq_knob_left.xml). Value = linear band gain, 1.0 unity,
          // 0.0 full kill with the default Biquad Full Kill EQ.
          eqLow(QStringLiteral("[EqualizerRack1_") + group + QStringLiteral("_Effect1]"),
                  QStringLiteral("parameter1")),
          eqMid(QStringLiteral("[EqualizerRack1_") + group + QStringLiteral("_Effect1]"),
                  QStringLiteral("parameter2")),
          eqHigh(QStringLiteral("[EqualizerRack1_") + group + QStringLiteral("_Effect1]"),
                  QStringLiteral("parameter3")),
          filter(QStringLiteral("[QuickEffectRack1_") + group + QStringLiteral("]"),
                  QStringLiteral("super1")),
          loopEnabled(group, QStringLiteral("loop_enabled")),
          reloopToggle(group, QStringLiteral("reloop_toggle")),
          stemDrums(stemGroup(group, 1), QStringLiteral("volume")),
          stemBass(stemGroup(group, 2), QStringLiteral("volume")),
          stemOther(stemGroup(group, 3), QStringLiteral("volume")),
          stemVocals(stemGroup(group, 4), QStringLiteral("volume")),
          stemCount(group, QStringLiteral("stem_count")),
          // Created by the deck's EngineBuffer, which exists before this
          // controller (PlayerManager builds it after the second deck).
          pVisualPlayPos(VisualPlayPosition::getVisualPlayPosition(group)) {
}

ControlProxy* AutomixTransitionController::DeckControls::control(AutomixParam param) {
    switch (param) {
    case AutomixParam::EqLow:
        return &eqLow;
    case AutomixParam::EqMid:
        return &eqMid;
    case AutomixParam::EqHigh:
        return &eqHigh;
    case AutomixParam::Filter:
        return &filter;
    case AutomixParam::Volume:
        return &volume;
    case AutomixParam::StemDrums:
        return &stemDrums;
    case AutomixParam::StemBass:
        return &stemBass;
    case AutomixParam::StemOther:
        return &stemOther;
    case AutomixParam::StemVocals:
        return &stemVocals;
    }
    return &eqMid;
}

AutomixTransitionController::AutomixTransitionController(
        UserSettingsPointer pConfig, QObject* pParent)
        : QObject(pParent),
          m_pConfig(pConfig),
          m_recipeFilePath(QDir(pConfig->getSettingsPath()).filePath(kRecipeFileName)),
          m_book(AutomixRecipeBook::builtin()),
          m_deck1(QStringLiteral("[Channel1]")),
          m_deck2(QStringLiteral("[Channel2]")),
          m_crossfader(QStringLiteral("[Master]"), QStringLiteral("crossfader")),
          m_triggerToDeck2(ConfigKey(QStringLiteral("[Channel1]"),
                  QStringLiteral("automix_transition_to_2"))),
          m_triggerToDeck1(ConfigKey(QStringLiteral("[Channel2]"),
                  QStringLiteral("automix_transition_to_1"))),
          m_buttonStateDeck1(ConfigKey(QStringLiteral("[Channel1]"),
                  QStringLiteral("automix_state"))),
          m_buttonStateDeck2(ConfigKey(QStringLiteral("[Channel2]"),
                  QStringLiteral("automix_state"))),
          m_engineState(ConfigKey(kEngineGroup, QStringLiteral("state"))),
          m_countdown(ConfigKey(kEngineGroup, QStringLiteral("countdown"))),
          m_manual(ConfigKey(kEngineGroup, QStringLiteral("manual"))),
          m_resume(ConfigKey(kEngineGroup, QStringLiteral("resume"))),
          m_recipeSelector(ConfigKey(kEngineGroup, QStringLiteral("recipe")),
                  true,
                  AutomixRecipeBook::kDefaultSelectorIndex) {
    // Mixxx 2.6: ButtonMode moved from ControlPushButton::TOGGLE to the
    // mixxx::control::ButtonMode enum class; setBehavior() applies both in
    // one step so the behavior is rebuilt only once.
    m_recipeSelector.setBehavior(mixxx::control::ButtonMode::Toggle,
            static_cast<int>(AutomixRecipeBook::selectorIds().size()));

    connect(&m_triggerToDeck2,
            &ControlPushButton::valueChanged,
            this,
            &AutomixTransitionController::slotTriggerToDeck2);
    connect(&m_triggerToDeck1,
            &ControlPushButton::valueChanged,
            this,
            &AutomixTransitionController::slotTriggerToDeck1);
    connect(&m_resume,
            &ControlPushButton::valueChanged,
            this,
            &AutomixTransitionController::slotResume);

    m_tickTimer.setInterval(kTickIntervalMs);
    connect(&m_tickTimer, &QTimer::timeout, this, &AutomixTransitionController::slotTick);
    m_longPressTimer.setSingleShot(true);
    m_longPressTimer.setInterval(kLongPressMs);
    connect(&m_longPressTimer,
            &QTimer::timeout,
            this,
            &AutomixTransitionController::slotLongPress);
    m_refusalTimer.setSingleShot(true);
    m_refusalTimer.setInterval(kRefusalDisplayMs);
    connect(&m_refusalTimer,
            &QTimer::timeout,
            this,
            &AutomixTransitionController::slotClearRefusal);

    reloadRecipesIfChanged();
}

AutomixTransitionController::~AutomixTransitionController() = default;

void AutomixTransitionController::reloadRecipesIfChanged() {
    QFileInfo info(m_recipeFilePath);
    if (!info.exists()) {
        // First run: write the defaults so they can be tuned by ear.
        QFile file(m_recipeFilePath);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(AutomixRecipeBook::builtinJson());
            file.close();
            qInfo() << "Automix: wrote default recipes to" << m_recipeFilePath;
        } else {
            qWarning() << "Automix: cannot write" << m_recipeFilePath;
        }
        m_book = AutomixRecipeBook::builtin();
        info.refresh();
        m_recipeFileModified = info.lastModified();
        m_recipeFileSize = info.size();
        return;
    }
    if (info.lastModified() == m_recipeFileModified && info.size() == m_recipeFileSize) {
        return;
    }
    m_recipeFileModified = info.lastModified();
    m_recipeFileSize = info.size();

    QFile file(m_recipeFilePath);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Automix: cannot read" << m_recipeFilePath << "- keeping previous recipes";
        return;
    }
    AutomixRecipeBook book;
    QString error;
    if (!AutomixRecipeBook::parse(file.readAll(), &book, &error)) {
        qWarning() << "Automix: invalid" << m_recipeFilePath << ":" << error
                   << "- keeping previous recipes";
        return;
    }
    m_book = std::move(book);
    qInfo() << "Automix: loaded" << m_book.size() << "recipes from" << m_recipeFilePath;
}

void AutomixTransitionController::slotTriggerToDeck2(double value) {
    onTrigger(1, value);
}

void AutomixTransitionController::slotTriggerToDeck1(double value) {
    onTrigger(2, value);
}

void AutomixTransitionController::onTrigger(int fromDeckNumber, double value) {
    if (value <= 0.0) {
        // Release.
        m_longPressTimer.stop();
        m_pressedDeckNumber = 0;
        return;
    }
    m_pressedDeckNumber = fromDeckNumber;
    switch (m_state) {
    case State::Idle:
        arm(fromDeckNumber);
        if (m_state == State::Armed) {
            m_longPressTimer.start();
        }
        break;
    case State::Armed:
        if (fromDeckNumber == m_fromDeckNumber) {
            disarm();
        }
        break;
    case State::Running:
        // Ignored: manual takeover of each knob is the way out.
        break;
    }
}

void AutomixTransitionController::slotLongPress() {
    if (m_state != State::Armed || m_pressedDeckNumber != m_fromDeckNumber) {
        return;
    }
    // Held: start on the bar right after the press, whatever the quantum.
    m_startGridBeat = AutomixTransitionMath::nextStartBeat(m_armGridBeat,
            AutomixTransitionMath::kBeatsPerBar,
            kStartGraceBeats);
}

void AutomixTransitionController::arm(int fromDeckNumber) {
    reloadRecipesIfChanged();
    const AutomixRecipe* pRecipe =
            m_book.forSelectorIndex(static_cast<int>(m_recipeSelector.get()));
    if (!pRecipe) {
        qWarning() << "Automix: no recipe for selector slot" << m_recipeSelector.get();
        return;
    }

    DeckControls& outgoing = deck(fromDeckNumber);
    DeckControls& incoming = deck(fromDeckNumber == 1 ? 2 : 1);
    if (std::abs(m_crossfader.get()) > kCrossfaderCenterTolerance) {
        refuse(fromDeckNumber, ButtonState::RefusedCrossfader);
        return;
    }
    const TrackPointer pOutgoingTrack = PlayerInfo::instance().getTrackInfo(outgoing.group);
    const TrackPointer pIncomingTrack = PlayerInfo::instance().getTrackInfo(incoming.group);
    if (!pOutgoingTrack || !pIncomingTrack) {
        refuse(fromDeckNumber, ButtonState::RefusedNoTrack);
        return;
    }
    if (!outgoing.play.toBool()) {
        refuse(fromDeckNumber, ButtonState::RefusedNotPlaying);
        return;
    }
    double gridBeat = 0.0;
    if (!readGridBeat(outgoing, &gridBeat)) {
        refuse(fromDeckNumber, ButtonState::RefusedNoGrid);
        return;
    }

    m_recipe = *pRecipe;
    m_fromDeckNumber = fromDeckNumber;
    m_pOutgoingTrack = pOutgoingTrack;
    m_pIncomingTrack = pIncomingTrack;

    // Dan, 2026-10-06: MIX drops any active loop on both decks. Only pulsed
    // when a loop is running, so this never starts one.
    if (outgoing.loopEnabled.toBool()) {
        outgoing.reloopToggle.set(1.0);
    }
    if (incoming.loopEnabled.toBool()) {
        incoming.reloopToggle.set(1.0);
    }

    // One-shot tempo match: changes speed only, never phase, never held
    // (docs/decisions/0008, "snap to grid" addendum).
    if (m_recipe.tempoMatch) {
        const double ratio = AutomixTransitionMath::tempoMatchedIncomingRateRatio(
                outgoing.bpm.get(), incoming.bpm.get(), incoming.rateRatio.get());
        if (ratio > 0.0) {
            incoming.rateRatio.set(ratio);
        }
    }

    // A stopped incoming deck is silent, so presetting its knobs is
    // inaudible; it is started on the first bar of the transition.
    m_incomingNeedsPlay = m_recipe.autoPlayIncoming && !incoming.play.toBool();
    if (m_incomingNeedsPlay) {
        for (const AutomixPreset& preset : m_recipe.prepareIncoming) {
            incoming.control(preset.param)->set(preset.value);
        }
    }

    m_armGridBeat = gridBeat;
    m_startGridBeat = AutomixTransitionMath::nextStartBeat(gridBeat,
            m_book.armQuantumBars() * AutomixTransitionMath::kBeatsPerBar,
            kStartGraceBeats);
    m_state = State::Armed;
    m_refusalTimer.stop();
    setButtonState(fromDeckNumber, ButtonState::Armed);
    setButtonState(fromDeckNumber == 1 ? 2 : 1, ButtonState::Idle);
    m_engineState.set(1.0);
    m_countdown.set(std::ceil(m_startGridBeat - gridBeat));
    qInfo() << "Automix: armed" << m_recipe.id << "from deck" << fromDeckNumber
            << "grid beat" << gridBeat << "start at" << m_startGridBeat;

    m_tickClock.start();
    m_tickTimer.start();
    // A press within the grace window starts right away.
    slotTick();
}

void AutomixTransitionController::disarm() {
    qInfo() << "Automix: disarmed";
    m_tickTimer.stop();
    m_longPressTimer.stop();
    m_state = State::Idle;
    setButtonState(m_fromDeckNumber, ButtonState::Idle);
    m_engineState.set(0.0);
    m_countdown.set(0.0);
    m_fromDeckNumber = 0;
    m_pOutgoingTrack.reset();
    m_pIncomingTrack.reset();
}

void AutomixTransitionController::start() {
    m_state = State::Running;
    m_longPressTimer.stop();
    setButtonState(m_fromDeckNumber, ButtonState::Running);
    m_engineState.set(2.0);

    DeckControls& outgoing = outgoingDeck();
    DeckControls& incoming = incomingDeck();
    m_lanes.clear();
    m_lanes.reserve(m_recipe.lanes.size());
    for (const AutomixLane& lane : m_recipe.lanes) {
        LaneRuntime runtime;
        runtime.lane = lane;
        runtime.pControl = (lane.deck == AutomixDeckRole::Outgoing ? outgoing : incoming)
                                   .control(lane.param);
        runtime.startValue = runtime.pControl->get();
        runtime.lastWritten = runtime.startValue;
        m_lanes.push_back(std::move(runtime));
    }
    qInfo() << "Automix: start" << m_recipe.id << "at transition beat" << m_transitionBeat;
    writeLanes();
    if (m_incomingNeedsPlay && m_transitionBeat >= m_recipe.incomingPlayAtBeat) {
        incoming.play.set(1.0);
        m_incomingNeedsPlay = false;
    }
}

void AutomixTransitionController::writeLanes() {
    bool anyManual = false;
    for (LaneRuntime& runtime : m_lanes) {
        if (!runtime.manual && diverged(runtime.pControl->get(), runtime.lastWritten)) {
            runtime.manual = true;
            runtime.gliding = false;
            qInfo() << "Automix: manual takeover of" << runtime.pControl->getKey().group
                    << runtime.pControl->getKey().item;
        }
        if (runtime.manual) {
            anyManual = true;
            continue;
        }
        double value = runtime.lane.valueAt(m_transitionBeat, runtime.startValue);
        if (runtime.gliding) {
            const double t = runtime.glideBeats > 0.0
                    ? (m_transitionBeat - runtime.glideStartBeat) / runtime.glideBeats
                    : 1.0;
            if (t >= 1.0) {
                runtime.gliding = false;
            } else {
                value = AutomixTransitionMath::interpolate(runtime.glideFrom,
                        value,
                        t,
                        AutomixTransitionMath::Shape::Smoothstep);
            }
        }
        runtime.pControl->set(value);
        runtime.lastWritten = value;
    }
    m_manual.set(anyManual ? 1.0 : 0.0);
}

void AutomixTransitionController::slotResume(double value) {
    if (value <= 0.0 || m_state != State::Running) {
        return;
    }
    for (LaneRuntime& runtime : m_lanes) {
        if (!runtime.manual) {
            continue;
        }
        runtime.manual = false;
        runtime.gliding = true;
        runtime.glideFrom = runtime.pControl->get();
        runtime.glideStartBeat = m_transitionBeat;
        runtime.glideBeats = m_book.resumeGlideBeats();
        runtime.lastWritten = runtime.glideFrom;
    }
    qInfo() << "Automix: resume auto";
}

bool AutomixTransitionController::tracksChanged() const {
    return PlayerInfo::instance().getTrackInfo(m_deck1.group) !=
            (m_fromDeckNumber == 1 ? m_pOutgoingTrack : m_pIncomingTrack) ||
            PlayerInfo::instance().getTrackInfo(m_deck2.group) !=
            (m_fromDeckNumber == 1 ? m_pIncomingTrack : m_pOutgoingTrack);
}

bool AutomixTransitionController::readGridBeat(DeckControls& deck, double* pBeat) const {
    const TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(deck.group);
    if (!pTrack) {
        return false;
    }
    const mixxx::BeatsPointer pBeats = pTrack->getBeats();
    if (!pBeats) {
        return false;
    }
    if (!deck.pVisualPlayPos || !deck.pVisualPlayPos->isValid()) {
        return false;
    }
    const double trackSamples = deck.trackSamples.get();
    if (trackSamples <= 0.0) {
        return false;
    }
    // Fraction of the track at the latest engine callback (updated every
    // buffer, unlike playposition which is throttled to 15 Hz).
    const double fraction = deck.pVisualPlayPos->getEnginePlayPos();
    const auto position = mixxx::audio::FramePos::fromEngineSamplePos(fraction * trackSamples);

    auto next = pBeats->iteratorFrom(position); // first beat at or after position
    if (next == pBeats->cend() || next == pBeats->cbegin()) {
        return false;
    }
    auto prev = next;
    if (*next > position) {
        --prev;
    } else {
        ++next;
    }
    const double beatLength = *next - *prev;
    if (beatLength <= 0.0) {
        return false;
    }
    const int index = prev - pBeats->cfirstmarker();
    *pBeat = index + (position - *prev) / beatLength;
    return true;
}

void AutomixTransitionController::slotTick() {
    const double dtSeconds = m_tickClock.restart() / 1000.0;
    if (m_state == State::Idle) {
        m_tickTimer.stop();
        return;
    }
    if (tracksChanged()) {
        qInfo() << "Automix: track changed on a deck";
        if (m_state == State::Armed) {
            disarm();
        } else {
            abort();
        }
        return;
    }

    DeckControls& outgoing = outgoingDeck();
    double gridBeat = 0.0;
    const bool haveGrid = readGridBeat(outgoing, &gridBeat);
    const bool outgoingPlaying = outgoing.play.toBool();

    if (m_state == State::Armed) {
        if (!haveGrid || !outgoingPlaying) {
            disarm();
            return;
        }
        if (gridBeat < m_startGridBeat) {
            m_countdown.set(std::ceil(m_startGridBeat - gridBeat));
            return;
        }
        m_transitionBeat = gridBeat - m_startGridBeat;
        m_lastGridBeat = gridBeat;
        start();
        return;
    }

    // Running.
    const double rawDelta = haveGrid ? gridBeat - m_lastGridBeat : 0.0;
    if (haveGrid) {
        m_lastGridBeat = gridBeat;
    }
    const double expectedDelta = dtSeconds * outgoing.bpm.get() / 60.0;
    const double previousBeat = m_transitionBeat;
    m_transitionBeat += AutomixTransitionMath::clockAdvance(
            rawDelta, expectedDelta, outgoingPlaying && haveGrid);
    // One line per bar: how late the first tick of each bar landed is the
    // timing error of every phase change (acceptance: < 20 ms).
    const double bar = std::floor(m_transitionBeat / AutomixTransitionMath::kBeatsPerBar);
    if (bar > std::floor(previousBeat / AutomixTransitionMath::kBeatsPerBar)) {
        const double lateMs = (m_transitionBeat - bar * AutomixTransitionMath::kBeatsPerBar) *
                60000.0 / std::max(outgoing.bpm.get(), 1.0);
        qInfo() << "Automix: bar" << bar << "tick late by" << lateMs << "ms";
    }

    if (m_incomingNeedsPlay && m_transitionBeat >= m_recipe.incomingPlayAtBeat) {
        incomingDeck().play.set(1.0);
        m_incomingNeedsPlay = false;
    }
    writeLanes();
    m_countdown.set(std::ceil(std::max(0.0, m_recipe.lengthBeats - m_transitionBeat) /
            AutomixTransitionMath::kBeatsPerBar));

    bool anyAutomated = false;
    bool anyGliding = false;
    for (const LaneRuntime& runtime : m_lanes) {
        anyAutomated = anyAutomated || !runtime.manual;
        anyGliding = anyGliding || runtime.gliding;
    }
    if ((m_transitionBeat >= m_recipe.lengthBeats && !anyGliding) || !anyAutomated) {
        finish();
    }
}

void AutomixTransitionController::finish() {
    // Land every automated lane exactly on its final value; manual lanes stay
    // where Dan put them.
    for (LaneRuntime& runtime : m_lanes) {
        if (runtime.manual || diverged(runtime.pControl->get(), runtime.lastWritten)) {
            continue;
        }
        runtime.pControl->set(runtime.lane.valueAt(m_recipe.lengthBeats, runtime.startValue));
    }
    if (m_incomingNeedsPlay) {
        incomingDeck().play.set(1.0);
        m_incomingNeedsPlay = false;
    }
    qInfo() << "Automix: finished" << m_recipe.id;
    abort();
}

void AutomixTransitionController::abort() {
    m_tickTimer.stop();
    m_longPressTimer.stop();
    m_lanes.clear();
    m_state = State::Idle;
    setButtonState(m_fromDeckNumber, ButtonState::Idle);
    m_engineState.set(0.0);
    m_countdown.set(0.0);
    m_manual.set(0.0);
    m_fromDeckNumber = 0;
    m_incomingNeedsPlay = false;
    m_pOutgoingTrack.reset();
    m_pIncomingTrack.reset();
}

void AutomixTransitionController::refuse(int fromDeckNumber, ButtonState reason) {
    qInfo() << "Automix: refused, reason" << static_cast<int>(reason);
    setButtonState(fromDeckNumber, reason);
    m_refusalTimer.start();
}

void AutomixTransitionController::slotClearRefusal() {
    if (m_state != State::Idle) {
        return;
    }
    setButtonState(1, ButtonState::Idle);
    setButtonState(2, ButtonState::Idle);
}

void AutomixTransitionController::setButtonState(int deckNumber, ButtonState state) {
    if (deckNumber == 1) {
        m_buttonStateDeck1.set(stateValue(state));
    } else if (deckNumber == 2) {
        m_buttonStateDeck2.set(stateValue(state));
    }
}
