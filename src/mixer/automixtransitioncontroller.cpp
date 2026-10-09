#include "mixer/automixtransitioncontroller.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtDebug>
#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

#include "mixer/automixtransitionmath.h"
#include "mixer/playerinfo.h"
#include "moc_automixtransitioncontroller.cpp"
#include "track/keyutils.h"
#include "track/track.h"
#include "waveform/visualplayposition.h"

namespace {
const QString kEngineGroup = QStringLiteral("[AutomixTransition]");
const QString kRecipeFileName = QStringLiteral("automix_recipes.json");
// [DJApp],BrainDb in mixxx.cfg: path to brain.db (docs/decisions/0019).
const QString kDJAppGroup = QStringLiteral("[DJApp]");
// The guards need NI stem files: drums, bass, other, vocals.
constexpr double kGuardedStemCount = 4.0;

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
// rate_ratio: the finest manual step (rate_perm_up_small, 0.05 %) is 5e-4;
// what this class writes reads back exactly.
constexpr double kRateOverrideTolerance = 1e-6;
// "Reia auto" on a rate lane glides over at least one bar: a tempo jump back
// onto the curve within one beat would be heard as a lurch.
constexpr double kTempoResumeGlideBeats = 4.0;

bool diverged(double a, double b) {
    return std::abs(a - b) > kOverrideTolerance;
}

bool rateDiverged(double a, double b) {
    return std::abs(a - b) > kRateOverrideTolerance;
}

QString bpmText(double bpm) {
    return QString::number(bpm, 'f', 2);
}

double stateValue(AutomixTransitionController::ButtonState state) {
    return static_cast<double>(static_cast<int>(state));
}
// The key guard swaps these; drums are never touched.
bool isHarmonicStem(AutomixParam param) {
    return param == AutomixParam::StemBass || param == AutomixParam::StemOther ||
            param == AutomixParam::StemVocals;
}

// The bass swaps with the EQ at half the recipe; other + vocals may swap
// earlier, at the vocal handover (AutomixKeyGuardPlanner::melodySwapBeat).
AutomixKeyGuardStem keyGuardStem(AutomixParam param) {
    return param == AutomixParam::StemBass ? AutomixKeyGuardStem::Bass
                                           : AutomixKeyGuardStem::Melody;
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
          syncEnabled(group, QStringLiteral("sync_enabled")),
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
                  AutomixRecipeBook::kDefaultSelectorIndex),
          m_vocalGuardToggle(ConfigKey(kEngineGroup, QStringLiteral("vocal_guard")),
                  true,
                  1.0),
          m_keyGuardToggle(ConfigKey(kEngineGroup, QStringLiteral("key_guard")),
                  true,
                  1.0),
          m_vocalGuardActiveStatus(ConfigKey(kEngineGroup, QStringLiteral("vocal_guard_active"))),
          m_keyGuardActiveStatus(ConfigKey(kEngineGroup, QStringLiteral("key_guard_active"))) {
    // Mixxx 2.6: ButtonMode moved from ControlPushButton::TOGGLE to the
    // mixxx::control::ButtonMode enum class; setBehavior() applies both in
    // one step so the behavior is rebuilt only once.
    m_recipeSelector.setBehavior(mixxx::control::ButtonMode::Toggle,
            static_cast<int>(AutomixRecipeBook::selectorIds().size()));
    m_vocalGuardToggle.setButtonMode(mixxx::control::ButtonMode::Toggle);
    m_keyGuardToggle.setButtonMode(mixxx::control::ButtonMode::Toggle);

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

    // A new MIX takes over from a tempo return still running: the decks keep
    // the tempo they have now and the new transition meets from there.
    if (m_tempoReturn.active) {
        stopTempoReturn(QStringLiteral("new MIX armed"));
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

    loadVocalMaps();

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
    m_outgoingVocalMap.reset();
    m_incomingVocalMap.reset();
    m_vocalGuardActiveStatus.set(0.0);
    m_keyGuardActiveStatus.set(0.0);
}

void AutomixTransitionController::loadVocalMaps() {
    m_outgoingVocalMap.reset();
    m_incomingVocalMap.reset();
    m_brainDbPath = m_pConfig->getValueString(ConfigKey(kDJAppGroup, QStringLiteral("BrainDb")));
    // Without stems on both decks the guard cannot run: skip the lookup.
    if (!m_recipe.vocalGuard.enabled || m_brainDbPath.isEmpty() ||
            outgoingDeck().stemCount.get() != kGuardedStemCount ||
            incomingDeck().stemCount.get() != kGuardedStemCount) {
        return;
    }
    m_outgoingVocalMap = AutomixVocalMapStore::lookup(
            m_brainDbPath, m_pOutgoingTrack->getLocation());
    m_incomingVocalMap = AutomixVocalMapStore::lookup(
            m_brainDbPath, m_pIncomingTrack->getLocation());
}

QString AutomixTransitionController::planVocalGuard() {
    m_vocalGuardActive = false;
    if (!m_recipe.vocalGuard.enabled) {
        return QStringLiteral("recipe has vocal_guard off");
    }
    if (!m_vocalGuardToggle.toBool()) {
        return QStringLiteral("switched off on the skin (VOCE LIBERA)");
    }
    DeckControls& outgoing = outgoingDeck();
    DeckControls& incoming = incomingDeck();
    if (outgoing.stemCount.get() != kGuardedStemCount) {
        return QStringLiteral("outgoing track is not a stem file (stem_count %1)")
                .arg(outgoing.stemCount.get());
    }
    if (incoming.stemCount.get() != kGuardedStemCount) {
        return QStringLiteral("incoming track is not a stem file (stem_count %1)")
                .arg(incoming.stemCount.get());
    }
    if (m_brainDbPath.isEmpty()) {
        return QStringLiteral("[DJApp],BrainDb is not set");
    }
    if (!QFileInfo::exists(m_brainDbPath)) {
        return QStringLiteral("brain.db not found at ") + m_brainDbPath;
    }
    if (!m_outgoingVocalMap) {
        return QStringLiteral("no vocal map in brain.db for the outgoing track ") +
                m_pOutgoingTrack->getLocation();
    }
    if (!m_incomingVocalMap) {
        return QStringLiteral("no vocal map in brain.db for the incoming track ") +
                m_pIncomingTrack->getLocation();
    }
    // An instrumental's "vocals" stem carries the lead melody: never guard it.
    if (!m_outgoingVocalMap->hasVocals) {
        return QStringLiteral("outgoing track is instrumental (has_vocals 0)");
    }
    if (!m_incomingVocalMap->hasVocals) {
        return QStringLiteral("incoming track is instrumental (has_vocals 0)");
    }
    const mixxx::BeatsPointer pOutgoingBeats = m_pOutgoingTrack->getBeats();
    if (!pOutgoingBeats) {
        return QStringLiteral("outgoing track has no beatgrid");
    }

    // Outgoing: the transition clock runs on its grid, beat 0 = start bar.
    const std::vector<AutomixVocalBlock> outgoingBlocks =
            AutomixVocalGuardPlanner::toTransitionBeats(
                    AutomixVocalGuardPlanner::secondsToGridBeats(
                            *pOutgoingBeats, m_outgoingVocalMap->segmentsSec),
                    m_startGridBeat,
                    0.0);

    // Incoming: its grid beat now (at its cue if stopped) is where it will be
    // when it plays from transition beat `incomingStart` on (now if already
    // playing, else when this transition presses play), then it advances
    // outgoingBpm / incomingBpm transition beats per beat of its own (1 when
    // tempo matched). Only feeds the "comes in mid phrase" note: the guard's
    // timing depends on the outgoing voice alone.
    std::vector<AutomixVocalBlock> incomingBlocks;
    bool incomingPlaced = false;
    double incomingGridBeat = 0.0;
    const mixxx::BeatsPointer pIncomingBeats = m_pIncomingTrack->getBeats();
    if (pIncomingBeats && readGridBeat(incoming, &incomingGridBeat)) {
        const double incomingStart = m_incomingNeedsPlay
                ? std::max(m_transitionBeat, m_recipe.incomingPlayAtBeat)
                : m_transitionBeat;
        const double outgoingBpm = outgoing.bpm.get();
        const double incomingBpm = incoming.bpm.get();
        const double beatRatio =
                outgoingBpm > 0.0 && incomingBpm > 0.0 ? outgoingBpm / incomingBpm : 1.0;
        incomingBlocks = AutomixVocalGuardPlanner::toTransitionBeats(
                AutomixVocalGuardPlanner::secondsToGridBeats(
                        *pIncomingBeats, m_incomingVocalMap->segmentsSec),
                incomingGridBeat,
                incomingStart,
                beatRatio);
        incomingPlaced = true;
    }

    // The outgoing voice hands over by the recipe's bass/high swap at the
    // latest, even mid phrase (see AutomixVocalGuardPlanner::latestHandoverBeats).
    m_vocalGuardPlan = AutomixVocalGuardPlanner::plan(outgoingBlocks,
            incomingBlocks,
            m_recipe.vocalGuard.maxWaitBeats,
            m_recipe.vocalGuard.fadeBeats,
            AutomixVocalGuardPlanner::latestHandoverBeats(m_recipe.lengthBeats));
    m_vocalGuardActive = true;
    // Handover at half the length at the latest: the guard only outlasts a
    // recipe shorter than four fades (2 bars with the default 2-beat fade).
    m_runLengthBeats = std::max(m_recipe.lengthBeats, m_vocalGuardPlan.endBeat());

    const auto beats = [](double value) {
        return QString::number(value, 'f', 2);
    };
    QString outgoingNote;
    if (m_vocalGuardPlan.cutMidPhrase) {
        outgoingNote = QStringLiteral("phrase cut, max wait ") +
                beats(m_recipe.vocalGuard.maxWaitBeats) +
                QStringLiteral(", latest handover ") +
                beats(AutomixVocalGuardPlanner::latestHandoverBeats(m_recipe.lengthBeats));
    } else if (m_vocalGuardPlan.outgoingFadeStart > 0.0) {
        outgoingNote = QStringLiteral("after its phrase");
    } else {
        outgoingNote = QStringLiteral("not singing at start");
    }
    QString incomingNote;
    if (!incomingPlaced) {
        incomingNote = QStringLiteral("incoming position unknown");
    } else if (m_vocalGuardPlan.incomingMidPhrase) {
        incomingNote = QStringLiteral("incoming enters mid phrase");
    } else {
        incomingNote = QStringLiteral("incoming enters between phrases");
    }
    QString notes = incomingNote;
    if (m_outgoingVocalMap->viaStemExport || m_incomingVocalMap->viaStemExport) {
        notes += QStringLiteral(", stem file uses its original's map");
    }
    if (m_runLengthBeats > m_recipe.lengthBeats) {
        notes += QStringLiteral(", transition stretched to ") + beats(m_runLengthBeats) +
                QStringLiteral(" beats");
    }
    qInfo().noquote() << QStringLiteral("Automix: vocal guard on: outgoing voice fades out at "
                                        "beat %1 (%2), incoming voice fades in at beat %3, "
                                        "fades of %4 beats (%5)")
                                 .arg(beats(m_vocalGuardPlan.outgoingFadeStart),
                                         outgoingNote,
                                         beats(m_vocalGuardPlan.incomingFadeStart),
                                         beats(m_vocalGuardPlan.fadeBeats),
                                         notes);
    return QString();
}

QString AutomixTransitionController::planKeyGuard() {
    m_keyGuardActive = false;
    if (!m_recipe.keyGuard.enabled) {
        return QStringLiteral("recipe has key_guard off");
    }
    if (!m_keyGuardToggle.toBool()) {
        return QStringLiteral("switched off on the skin (TON LIBER)");
    }
    if (outgoingDeck().stemCount.get() != kGuardedStemCount) {
        return QStringLiteral("outgoing track is not a stem file (stem_count %1)")
                .arg(outgoingDeck().stemCount.get());
    }
    if (incomingDeck().stemCount.get() != kGuardedStemCount) {
        return QStringLiteral("incoming track is not a stem file (stem_count %1)")
                .arg(incomingDeck().stemCount.get());
    }
    const mixxx::track::io::key::ChromaticKey outgoingKey = m_pOutgoingTrack->getKey();
    const mixxx::track::io::key::ChromaticKey incomingKey = m_pIncomingTrack->getKey();
    if (outgoingKey == mixxx::track::io::key::INVALID) {
        return QStringLiteral("outgoing track has no key");
    }
    if (incomingKey == mixxx::track::io::key::INVALID) {
        return QStringLiteral("incoming track has no key");
    }
    const QString keys = KeyUtils::keyToString(outgoingKey) + QStringLiteral(" -> ") +
            KeyUtils::keyToString(incomingKey);
    if (!AutomixKeyGuardPlanner::keysClash(outgoingKey, incomingKey)) {
        return QStringLiteral("keys fit (") + keys + QStringLiteral(")");
    }
    // Runs after planVocalGuard(): with the vocal guard active, the melody
    // (other + vocals) follows the outgoing voice's handover when that comes
    // before the bass swap.
    m_keyGuardPlan = AutomixKeyGuardPlanner::plan(m_recipe.lengthBeats,
            m_recipe.keyGuard.fadeBeats,
            m_vocalGuardActive ? &m_vocalGuardPlan : nullptr,
            m_recipe.incomingPlayAtBeat);
    m_keyGuardActive = true;
    m_runLengthBeats = std::max(m_runLengthBeats, m_keyGuardPlan.endBeat());
    const auto beats = [](double value) {
        return QString::number(value, 'f', 2);
    };
    if (m_keyGuardPlan.earlyMelody()) {
        qInfo().noquote() << QStringLiteral("Automix: key guard on (%1): only the incoming drums "
                                            "until beat %2, then other + vocals swap at the "
                                            "vocal handover, bass at beat %3, over %4 beats")
                                     .arg(keys,
                                             beats(m_keyGuardPlan.melodySwapBeat),
                                             beats(m_keyGuardPlan.swapBeat),
                                             beats(m_keyGuardPlan.fadeBeats));
    } else {
        qInfo().noquote() << QStringLiteral("Automix: key guard on (%1): only the incoming drums "
                                            "until beat %2, harmonic stems swap over %3 beats")
                                     .arg(keys,
                                             beats(m_keyGuardPlan.swapBeat),
                                             beats(m_keyGuardPlan.fadeBeats));
    }
    return QString();
}

void AutomixTransitionController::start() {
    m_state = State::Running;
    m_longPressTimer.stop();
    setButtonState(m_fromDeckNumber, ButtonState::Running);
    m_engineState.set(2.0);

    DeckControls& outgoing = outgoingDeck();
    DeckControls& incoming = incomingDeck();
    // A stopped incoming deck is inaudible: match it again in case the
    // outgoing tempo moved while armed, so it starts in phase AND in tempo.
    if (m_recipe.tempoMatch && m_incomingNeedsPlay) {
        const double ratio = AutomixTransitionMath::tempoMatchedIncomingRateRatio(
                outgoing.bpm.get(), incoming.bpm.get(), incoming.rateRatio.get());
        if (ratio > 0.0 && rateDiverged(ratio, incoming.rateRatio.get())) {
            incoming.rateRatio.set(ratio);
        }
    }
    m_runLengthBeats = m_recipe.lengthBeats;
    const QString guardOffReason = planVocalGuard();
    if (!guardOffReason.isEmpty()) {
        qInfo().noquote() << "Automix: vocal guard off:" << guardOffReason;
    }
    m_vocalGuardActiveStatus.set(m_vocalGuardActive ? 1.0 : 0.0);
    const QString keyGuardOffReason = planKeyGuard();
    if (!keyGuardOffReason.isEmpty()) {
        qInfo().noquote() << "Automix: key guard off:" << keyGuardOffReason;
    }
    m_keyGuardActiveStatus.set(m_keyGuardActive ? 1.0 : 0.0);
    planTempo();

    m_lanes.clear();
    m_lanes.reserve(m_recipe.lanes.size() + 6);
    const auto addLane = [this, &outgoing, &incoming](const AutomixLane& lane) {
        LaneRuntime runtime;
        runtime.lane = lane;
        runtime.pControl = (lane.deck == AutomixDeckRole::Outgoing ? outgoing : incoming)
                                   .control(lane.param);
        // Read now, never remembered from an earlier track: Mixxx 2.6 resets
        // stem volumes on track load (stem_auto_reset).
        runtime.startValue = runtime.pControl->get();
        runtime.lastWritten = runtime.startValue;
        runtime.vocalGuard = m_vocalGuardActive && lane.param == AutomixParam::StemVocals;
        runtime.keyGuard = m_keyGuardActive && isHarmonicStem(lane.param);
        m_lanes.push_back(std::move(runtime));
    };
    std::set<std::pair<AutomixDeckRole, AutomixParam>> recipeLanes;
    for (const AutomixLane& lane : m_recipe.lanes) {
        addLane(lane);
        recipeLanes.insert({lane.deck, lane.param});
    }
    // Implicit stem lanes for the guards: flat at the knob's start value,
    // scaled by the guard gain. A recipe lane on the same stem is scaled
    // instead.
    std::vector<AutomixParam> guardedStems;
    if (m_keyGuardActive) {
        guardedStems = {AutomixParam::StemBass, AutomixParam::StemOther, AutomixParam::StemVocals};
    } else if (m_vocalGuardActive) {
        guardedStems = {AutomixParam::StemVocals};
    }
    for (const AutomixDeckRole role : {AutomixDeckRole::Outgoing, AutomixDeckRole::Incoming}) {
        for (const AutomixParam param : guardedStems) {
            if (recipeLanes.count({role, param}) > 0) {
                continue;
            }
            AutomixLane lane;
            lane.deck = role;
            lane.param = param;
            addLane(lane);
        }
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
        double value = laneTarget(runtime, m_transitionBeat);
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
    if (m_tempoPlan) {
        // Same factor on both decks in the same tick: equal tempos, kept phase.
        const double factor = m_tempoPlan->transitionFactor(m_transitionBeat);
        for (TempoLane* pLane : {&m_outgoingTempo, &m_incomingTempo}) {
            writeTempoLane(pLane, pLane->startRate * factor, m_transitionBeat);
            anyManual = anyManual || pLane->manual;
        }
    }
    m_manual.set(anyManual ? 1.0 : 0.0);
}

void AutomixTransitionController::planTempo() {
    m_tempoPlan.reset();
    m_outgoingTempo = TempoLane();
    m_incomingTempo = TempoLane();
    DeckControls& outgoing = outgoingDeck();
    DeckControls& incoming = incomingDeck();
    if (m_recipe.tempo.mode == AutomixTempoMode::MeetReturn &&
            (outgoing.syncEnabled.toBool() || incoming.syncEnabled.toBool())) {
        qInfo() << "Automix: tempo lanes off: sync is on, Mixxx sync owns the rate";
        return;
    }
    QString whyNot;
    m_tempoPlan = AutomixTempoPlanner::plan(m_recipe.tempo,
            m_recipe.lengthBeats,
            outgoing.bpm.get(),
            incoming.bpm.get(),
            incoming.rateRatio.get(),
            &whyNot);
    if (!m_tempoPlan) {
        qInfo().noquote() << "Automix: tempo lanes off:" << whyNot;
        return;
    }
    for (auto [pLane, pDeck] : {std::pair{&m_outgoingTempo, &outgoing},
                 std::pair{&m_incomingTempo, &incoming}}) {
        pLane->pRate = &pDeck->rateRatio;
        pLane->startRate = pDeck->rateRatio.get();
        pLane->lastWritten = pLane->startRate;
    }
    const AutomixTempoPlan& plan = *m_tempoPlan;
    QString returnNote = QStringLiteral(", stays there (return_bars 0)");
    if (plan.hasReturn()) {
        returnNote = QStringLiteral(", then incoming back to its own %1 over %2 of its beats")
                             .arg(bpmText(plan.incomingBpm), QString::number(plan.returnBeats));
    }
    qInfo().noquote() << QStringLiteral(
            "Automix: tempo meet in the middle: %1 and %2 -> %3 BPM, both decks over "
            "transition beats 0-%4%5")
                                 .arg(bpmText(plan.outgoingBpm),
                                         bpmText(plan.incomingBpm),
                                         bpmText(plan.meetBpm),
                                         QString::number(plan.meetEndBeat),
                                         returnNote);
}

bool AutomixTransitionController::writeTempoLane(
        TempoLane* pLane, double target, double clockBeat) {
    if (!pLane->pRate) {
        return false;
    }
    if (!pLane->manual && rateDiverged(pLane->pRate->get(), pLane->lastWritten)) {
        pLane->manual = true;
        pLane->gliding = false;
        qInfo() << "Automix: manual takeover of" << pLane->pRate->getKey().group
                << pLane->pRate->getKey().item;
    }
    if (pLane->manual) {
        return false;
    }
    double value = target;
    if (pLane->gliding) {
        const double t = pLane->glideBeats > 0.0
                ? (clockBeat - pLane->glideStartBeat) / pLane->glideBeats
                : 1.0;
        if (t >= 1.0) {
            pLane->gliding = false;
        } else {
            value = AutomixTransitionMath::interpolate(
                    pLane->glideFrom, target, t, AutomixTransitionMath::Shape::Smoothstep);
        }
    }
    if (value > 0.0) {
        pLane->pRate->set(value);
        pLane->lastWritten = value;
    }
    return true;
}

void AutomixTransitionController::resumeTempoLane(TempoLane* pLane, double clockBeat) {
    if (!pLane->pRate || !pLane->manual) {
        return;
    }
    pLane->manual = false;
    pLane->gliding = true;
    pLane->glideFrom = pLane->pRate->get();
    pLane->glideStartBeat = clockBeat;
    pLane->glideBeats = std::max(m_book.resumeGlideBeats(), kTempoResumeGlideBeats);
    pLane->lastWritten = pLane->glideFrom;
}

void AutomixTransitionController::startTempoReturn() {
    if (!m_tempoPlan || !m_tempoPlan->hasReturn() || !m_incomingTempo.pRate) {
        return;
    }
    TempoReturn& r = m_tempoReturn;
    r = TempoReturn();
    r.plan = *m_tempoPlan;
    r.incomingDeckNumber = m_fromDeckNumber == 1 ? 2 : 1;
    r.outgoingDeckNumber = m_fromDeckNumber;
    r.pIncomingTrack = m_pIncomingTrack;
    r.pOutgoingTrack = m_pOutgoingTrack;
    r.incoming = m_incomingTempo;
    r.outgoing = m_outgoingTempo;
    r.outgoingFollows = m_outgoingTempo.pRate != nullptr;
    // A "Reia auto" glide still running restarts on the return's clock.
    for (TempoLane* pLane : {&r.incoming, &r.outgoing}) {
        if (pLane->gliding) {
            pLane->glideFrom = pLane->lastWritten;
            pLane->glideStartBeat = 0.0;
        }
    }
    DeckControls& incoming = deck(r.incomingDeckNumber);
    r.haveLastGridBeat = readGridBeat(incoming, &r.lastGridBeat);
    r.active = true;
    qInfo().noquote() << QStringLiteral(
            "Automix: tempo return: incoming %1 -> %2 BPM over %3 of its beats%4")
                                 .arg(bpmText(r.plan.meetBpm),
                                         bpmText(r.plan.incomingBpm),
                                         QString::number(r.plan.returnBeats),
                                         r.incoming.manual
                                                 ? QStringLiteral(" (rate under manual control)")
                                                 : QString());
}

void AutomixTransitionController::tickTempoReturn(double dtSeconds) {
    TempoReturn& r = m_tempoReturn;
    DeckControls& incoming = deck(r.incomingDeckNumber);
    if (PlayerInfo::instance().getTrackInfo(incoming.group) != r.pIncomingTrack) {
        stopTempoReturn(QStringLiteral("incoming track changed"));
        return;
    }
    // Clock: the incoming deck's own beats, so the return lasts return_bars
    // of the music Dan hears, whatever happens to the old deck.
    double gridBeat = 0.0;
    const bool haveGrid = readGridBeat(incoming, &gridBeat);
    const double rawDelta = haveGrid && r.haveLastGridBeat ? gridBeat - r.lastGridBeat : 0.0;
    if (haveGrid) {
        r.lastGridBeat = gridBeat;
        r.haveLastGridBeat = true;
    }
    const double expectedDelta = dtSeconds * incoming.bpm.get() / 60.0;
    const double previousBeat = r.beat;
    r.beat += AutomixTransitionMath::clockAdvance(
            rawDelta, expectedDelta, incoming.play.toBool() && haveGrid);

    const double factor = r.plan.returnFactorAt(r.beat);
    writeTempoLane(&r.incoming, r.incoming.startRate * factor, r.beat);
    if (r.outgoingFollows) {
        DeckControls& outgoing = deck(r.outgoingDeckNumber);
        if (PlayerInfo::instance().getTrackInfo(outgoing.group) != r.pOutgoingTrack ||
                !outgoing.play.toBool()) {
            // The old track is gone: its deck is Dan's again.
            r.outgoingFollows = false;
        } else {
            writeTempoLane(&r.outgoing, r.outgoing.startRate * factor, r.beat);
        }
    }
    const bool anyManual = r.incoming.manual || (r.outgoingFollows && r.outgoing.manual);
    m_manual.set(anyManual ? 1.0 : 0.0);

    const double bar = std::floor(r.beat / AutomixTransitionMath::kBeatsPerBar);
    if (bar > std::floor(previousBeat / AutomixTransitionMath::kBeatsPerBar)) {
        qInfo().noquote() << QStringLiteral("Automix: tempo return bar %1: incoming %2 BPM%3")
                                     .arg(QString::number(bar),
                                             bpmText(incoming.bpm.get()),
                                             r.outgoingFollows
                                                     ? QStringLiteral(" (old deck follows)")
                                                     : QString());
    }

    const bool gliding = r.incoming.gliding || (r.outgoingFollows && r.outgoing.gliding);
    if (r.beat >= r.plan.returnBeats && !gliding) {
        // Land exactly on the own tempo (rate 1) unless Dan holds the fader.
        if (!r.incoming.manual && !rateDiverged(r.incoming.pRate->get(), r.incoming.lastWritten)) {
            r.incoming.pRate->set(r.incoming.startRate * r.plan.returnFactor);
        }
        if (r.outgoingFollows && !r.outgoing.manual &&
                !rateDiverged(r.outgoing.pRate->get(), r.outgoing.lastWritten)) {
            r.outgoing.pRate->set(r.outgoing.startRate * r.plan.returnFactor);
        }
        stopTempoReturn(r.incoming.manual
                        ? QStringLiteral("done, incoming rate left where Dan put it")
                        : QStringLiteral("done, incoming back at its own %1 BPM")
                                  .arg(bpmText(r.plan.incomingBpm)));
    }
}

void AutomixTransitionController::stopTempoReturn(const QString& reason) {
    if (!m_tempoReturn.active) {
        return;
    }
    qInfo().noquote() << "Automix: tempo return ended:" << reason;
    m_tempoReturn = TempoReturn();
    if (m_state == State::Idle) {
        m_manual.set(0.0);
        m_tickTimer.stop();
    }
}

QString AutomixTransitionController::tempoLogNote() {
    DeckControls& outgoing = outgoingDeck();
    DeckControls& incoming = incomingDeck();
    QString note = QStringLiteral(", tempo %1/%2 BPM")
                           .arg(bpmText(outgoing.bpm.get()), bpmText(incoming.bpm.get()));
    double outgoingBeat = 0.0;
    double incomingBeat = 0.0;
    if (incoming.play.toBool() && readGridBeat(outgoing, &outgoingBeat) &&
            readGridBeat(incoming, &incomingBeat)) {
        // Both positions come from the same engine callback: the fractional
        // beat difference is the beatmatch error.
        const double diff = incomingBeat - outgoingBeat;
        const double errorBeats = diff - std::round(diff);
        note += QStringLiteral(", phase in-out %1 ms")
                        .arg(QString::number(errorBeats * 60000.0 /
                                        std::max(outgoing.bpm.get(), 1.0),
                                'f',
                                1));
    }
    return note;
}

double AutomixTransitionController::laneTarget(const LaneRuntime& runtime, double beat) const {
    const double value = runtime.lane.valueAt(beat, runtime.startValue);
    const bool outgoingRole = runtime.lane.deck == AutomixDeckRole::Outgoing;
    double gain = 1.0;
    if (runtime.vocalGuard) {
        gain = outgoingRole ? m_vocalGuardPlan.outgoingGain(beat)
                            : m_vocalGuardPlan.incomingGain(beat);
    }
    if (runtime.keyGuard) {
        const AutomixKeyGuardStem stem = keyGuardStem(runtime.lane.param);
        gain = std::min(gain,
                outgoingRole ? m_keyGuardPlan.outgoingGain(stem, beat)
                             : m_keyGuardPlan.incomingGain(stem, beat));
    }
    return value * gain;
}

void AutomixTransitionController::releaseGuards() {
    for (LaneRuntime& runtime : m_lanes) {
        if (!(runtime.vocalGuard || runtime.keyGuard) || runtime.manual ||
                diverged(runtime.pControl->get(), runtime.lastWritten)) {
            continue;
        }
        const bool outgoingRole = runtime.lane.deck == AutomixDeckRole::Outgoing;
        const DeckControls& deck = outgoingRole ? outgoingDeck() : incomingDeck();
        if (PlayerInfo::instance().getTrackInfo(deck.group) !=
                (outgoingRole ? m_pOutgoingTrack : m_pIncomingTrack)) {
            continue;
        }
        runtime.pControl->set(runtime.lane.valueAt(m_transitionBeat, runtime.startValue));
    }
}

void AutomixTransitionController::slotResume(double value) {
    if (value <= 0.0) {
        return;
    }
    if (m_tempoReturn.active) {
        resumeTempoLane(&m_tempoReturn.incoming, m_tempoReturn.beat);
        if (m_tempoReturn.outgoingFollows) {
            resumeTempoLane(&m_tempoReturn.outgoing, m_tempoReturn.beat);
        }
        qInfo() << "Automix: resume auto (tempo return)";
    }
    if (m_state != State::Running) {
        return;
    }
    resumeTempoLane(&m_outgoingTempo, m_transitionBeat);
    resumeTempoLane(&m_incomingTempo, m_transitionBeat);
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
    const std::optional<double> beat = AutomixTransitionMath::gridBeatAt(*pBeats, position);
    if (!beat) {
        return false;
    }
    *pBeat = *beat;
    return true;
}

void AutomixTransitionController::slotTick() {
    const double dtSeconds = m_tickClock.restart() / 1000.0;
    if (m_tempoReturn.active) {
        tickTempoReturn(dtSeconds);
    }
    if (m_state == State::Idle) {
        if (!m_tempoReturn.active) {
            m_tickTimer.stop();
        }
        return;
    }
    if (tracksChanged()) {
        qInfo() << "Automix: track changed on a deck";
        if (m_state == State::Armed) {
            disarm();
        } else {
            releaseGuards();
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
        qInfo().noquote() << "Automix: bar" << bar << "tick late by" << lateMs << "ms"
                          << tempoLogNote();
    }

    if (m_incomingNeedsPlay && m_transitionBeat >= m_recipe.incomingPlayAtBeat) {
        incomingDeck().play.set(1.0);
        m_incomingNeedsPlay = false;
    }
    writeLanes();
    m_countdown.set(std::ceil(std::max(0.0, m_runLengthBeats - m_transitionBeat) /
            AutomixTransitionMath::kBeatsPerBar));

    bool anyAutomated = false;
    bool anyGliding = false;
    for (const LaneRuntime& runtime : m_lanes) {
        anyAutomated = anyAutomated || !runtime.manual;
        anyGliding = anyGliding || runtime.gliding;
    }
    if (m_tempoPlan) {
        for (const TempoLane* pLane : {&m_outgoingTempo, &m_incomingTempo}) {
            anyAutomated = anyAutomated || !pLane->manual;
            anyGliding = anyGliding || pLane->gliding;
        }
    }
    if ((m_transitionBeat >= m_runLengthBeats && !anyGliding) || !anyAutomated) {
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
        runtime.pControl->set(laneTarget(runtime, m_runLengthBeats));
    }
    if (m_tempoPlan) {
        const double factor = m_tempoPlan->transitionFactor(m_runLengthBeats);
        for (TempoLane* pLane : {&m_outgoingTempo, &m_incomingTempo}) {
            if (pLane->manual || rateDiverged(pLane->pRate->get(), pLane->lastWritten)) {
                continue;
            }
            pLane->gliding = false;
            pLane->lastWritten = pLane->startRate * factor;
            pLane->pRate->set(pLane->lastWritten);
        }
    }
    if (m_incomingNeedsPlay) {
        incomingDeck().play.set(1.0);
        m_incomingNeedsPlay = false;
    }
    // Dan, 2026-10-09: "piesa veche continua sa ruleze... lasa pana la
    // final" - every recipe's outgoing EQ lanes land on kill (confirmed
    // silent by the loop above), but nothing ever stopped the deck itself:
    // it kept its transport running, audibly silent, all the way to the
    // physical end of the file. That also left both decks' `play` true at
    // once, which is exactly the case djapp::autopilot::decide() treats as
    // "ambiguous, wait" (WaitsWhenNeitherOrBothDecksPlay) - so the autopilot
    // could stall after its very first transition until Dan stopped the old
    // deck by hand. Pause it (not eject: the track stays loaded, same as
    // today) once it is already silent.
    outgoingDeck().play.set(0.0);
    qInfo() << "Automix: finished" << m_recipe.id;
    // The incoming deck's glide back to its own tempo outlives the transition.
    startTempoReturn();
    abort();
}

void AutomixTransitionController::abort() {
    if (!m_tempoReturn.active) {
        m_tickTimer.stop();
    }
    m_longPressTimer.stop();
    m_lanes.clear();
    m_state = State::Idle;
    setButtonState(m_fromDeckNumber, ButtonState::Idle);
    m_engineState.set(0.0);
    m_countdown.set(0.0);
    m_manual.set(m_tempoReturn.active && m_tempoReturn.incoming.manual ? 1.0 : 0.0);
    m_tempoPlan.reset();
    m_outgoingTempo = TempoLane();
    m_incomingTempo = TempoLane();
    m_fromDeckNumber = 0;
    m_incomingNeedsPlay = false;
    m_pOutgoingTrack.reset();
    m_pIncomingTrack.reset();
    m_outgoingVocalMap.reset();
    m_incomingVocalMap.reset();
    m_vocalGuardActive = false;
    m_keyGuardActive = false;
    m_vocalGuardActiveStatus.set(0.0);
    m_keyGuardActiveStatus.set(0.0);
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
