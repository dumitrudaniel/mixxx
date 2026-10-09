#include "library/djapp/djappautopilot.h"

#include <QDateTime>
#include <QDir>
#include <QSqlDatabase>
#include <algorithm>
#include <QSqlError>
#include <QSqlQuery>
#include <QtDebug>

#include "library/djapp/braindbreader.h"
#include "library/djapp/djappanalysis.h"
#include "library/djapp/djappsuggestions.h"
#include "library/library.h"
#include "library/trackcollectionmanager.h"
#include "mixer/automixrecipe.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "moc_djappautopilot.cpp"
#include "track/track.h"

using namespace djapp::autopilot;

namespace {
const QString kAutopilotGroup = QStringLiteral("[DJAppAutomix]");
const QString kDJAppGroup = QStringLiteral("[DJApp]");
constexpr int kTickIntervalMs = 500;

QString trackLabel(const DJAppLibraryTrack& track) {
    if (!track.artist.isEmpty() && !track.title.isEmpty()) {
        return QStringLiteral("%1 – %2").arg(track.artist, track.title);
    }
    return track.title.isEmpty() ? track.location : track.title;
}
} // namespace

DJAppAutopilot::DeckProxies::DeckProxies(const QString& group)
        : play(group, QStringLiteral("play")),
          trackLoaded(group, QStringLiteral("track_loaded")),
          playposition(group, QStringLiteral("playposition")),
          duration(group, QStringLiteral("duration")) {
}

DJAppAutopilot::DJAppAutopilot(UserSettingsPointer pConfig, Library* pLibrary, QObject* pParent)
        : QObject(pParent),
          m_pConfig(pConfig),
          m_pLibrary(pLibrary),
          m_enabled(ConfigKey(kAutopilotGroup, QStringLiteral("enabled")), true, 0.0),
          m_engineState(ConfigKey(QStringLiteral("[AutomixTransition]"), QStringLiteral("state"))),
          m_recipeSelector(
                  ConfigKey(QStringLiteral("[AutomixTransition]"), QStringLiteral("recipe"))),
          m_triggerToDeck2(ConfigKey(
                  QStringLiteral("[Channel1]"), QStringLiteral("automix_transition_to_2"))),
          m_triggerToDeck1(ConfigKey(
                  QStringLiteral("[Channel2]"), QStringLiteral("automix_transition_to_1"))),
          m_vocalGuardActiveStatus(ConfigKey(QStringLiteral("[AutomixTransition]"),
                  QStringLiteral("vocal_guard_active"))),
          m_keyGuardActiveStatus(ConfigKey(QStringLiteral("[AutomixTransition]"),
                  QStringLiteral("key_guard_active"))),
          m_deck1(QStringLiteral("[Channel1]")),
          m_deck2(QStringLiteral("[Channel2]")) {
    m_enabled.setButtonMode(mixxx::control::ButtonMode::Toggle);
    // Not ControlObject::valueChanged on m_enabled itself: a ControlObject
    // never notifies the instance that performed the set() (self-feedback
    // suppression), and setEnabled() below is the only writer today. tick()
    // below diffs against m_lastEnabled instead, so a future external writer
    // (e.g. a MIDI mapping on [DJAppAutomix],enabled) would still update the
    // UI, just within one tick.
    m_lastEnabled = isEnabled();

    m_tickTimer.setInterval(kTickIntervalMs);
    connect(&m_tickTimer, &QTimer::timeout, this, &DJAppAutopilot::tick);
    m_tickTimer.start();

    // Initial status, before the first tick.
    setStatus(Status{isEnabled() ? StatusKind::NoPlayingDeck : StatusKind::Off, 0, 0.0, QString()});
}

DJAppAutopilot::~DJAppAutopilot() = default;

bool DJAppAutopilot::isEnabled() const {
    return m_enabled.toBool();
}

void DJAppAutopilot::setEnabled(bool enabled) {
    m_enabled.set(enabled ? 1.0 : 0.0);
    if (enabled != m_lastEnabled) {
        m_lastEnabled = enabled;
        emit enabledChanged(enabled);
    }
}

void DJAppAutopilot::chooseCandidate(int index) {
    if (index < 0 || index >= m_candidates.size()) {
        return;
    }
    loadCandidate(m_candidatesPlayingDeck, m_candidatesOtherDeck, m_candidates.at(index));
}

QString DJAppAutopilot::brainDbPath() const {
    return BrainDbReader::configuredPath(
            m_pConfig->getValueString(ConfigKey(kDJAppGroup, QStringLiteral("BrainDb"))));
}

double DJAppAutopilot::activeRecipeLengthBeats() const {
    const AutomixRecipeBook& book = AutomixRecipeBook::builtin();
    const AutomixRecipe* pRecipe = book.forSelectorIndex(static_cast<int>(m_recipeSelector.get()));
    return pRecipe ? pRecipe->lengthBeats : 32.0; // Standard 8 default
}

std::optional<double> DJAppAutopilot::readMixOutSec(
        const QString& location, double lengthBeats) const {
    const QString dbPath = brainDbPath();
    if (dbPath.isEmpty() || location.isEmpty()) {
        return std::nullopt;
    }
    std::optional<double> result;
    QString error;
    QString targetKey = BrainDbReader::locationKey(location);
    BrainDbReader::withReadOnlyConnection(
            dbPath,
            2000,
            [&](const QSqlDatabase& db) {
                // The deck may be playing a stem twin (.stem.mp4), not the original -
                // mix_points is keyed by the original's location (ADR 0028). Resolve
                // it first, same mapping BrainDbReader/DJAppSuggestions already use.
                // A raw SQL "=" on stem_path would miss it (backslash vs forward-slash,
                // case), so match normalized keys in C++, like every other lookup here.
                QSqlQuery resolve(db);
                if (resolve.exec(QStringLiteral(
                            "SELECT source_location, stem_path FROM stem_exports"))) {
                    while (resolve.next()) {
                        if (BrainDbReader::locationKey(resolve.value(1).toString()) !=
                                targetKey) {
                            continue;
                        }
                        const QString original = resolve.value(0).toString();
                        if (!original.isEmpty()) {
                            targetKey = BrainDbReader::locationKey(original);
                        }
                        break;
                    }
                }

                QSqlQuery query(db);
                if (!query.exec(QStringLiteral(
                            "SELECT location, mix_out_sec FROM mix_points"))) {
                    return;
                }
                while (query.next()) {
                    if (BrainDbReader::locationKey(query.value(0).toString()) != targetKey) {
                        continue;
                    }
                    const QMap<int, double> byLength =
                            parseMixOutMap(query.value(1).toString());
                    result = nearestMixOut(byLength, lengthBeats);
                    return;
                }
            },
            &error);
    return result;
}

std::optional<double> DJAppAutopilot::readMixInSec(const QString& location) const {
    const QString dbPath = brainDbPath();
    if (dbPath.isEmpty() || location.isEmpty()) {
        return std::nullopt;
    }
    std::optional<double> result;
    QString error;
    const QString targetKey = BrainDbReader::locationKey(location);
    BrainDbReader::withReadOnlyConnection(
            dbPath,
            2000,
            [&](const QSqlDatabase& db) {
                QSqlQuery query(db);
                if (!query.exec(QStringLiteral(
                            "SELECT location, mix_in_sec FROM mix_points"))) {
                    return;
                }
                while (query.next()) {
                    if (BrainDbReader::locationKey(query.value(0).toString()) != targetKey) {
                        continue;
                    }
                    result = query.value(1).toDouble();
                    return;
                }
            },
            &error);
    return result;
}

void DJAppAutopilot::refreshMixOutCache(int deckNumber) {
    const int i = deckNumber - 1;
    const TrackPointer pTrack =
            PlayerInfo::instance().getTrackInfo(PlayerManager::groupForDeck(i));
    const QString location = pTrack ? pTrack->getLocation() : QString();
    const double lengthBeats = activeRecipeLengthBeats();
    if (location == m_cachedLocation[i] && lengthBeats == m_cachedRecipeLengthBeats[i]) {
        return;
    }
    m_cachedLocation[i] = location;
    m_cachedRecipeLengthBeats[i] = lengthBeats;
    m_cachedMixOutSec[i] = location.isEmpty() ? std::nullopt : readMixOutSec(location, lengthBeats);
    // The source (playing) deck's track changed: a genuinely new cycle, so
    // any candidates shown for the previous one are stale. A change on the
    // OTHER deck does NOT reset them here: that is exactly what picking a
    // candidate does (loads it there), and Dan asked to keep seeing the list
    // after picking one, in case he changes his mind and clicks another.
    if (m_candidatesQueried && deckNumber == m_candidatesPlayingDeck) {
        resetCandidates();
    }
}

void DJAppAutopilot::setStatus(const Status& status) {
    emit statusTextChanged(statusText(status));
}

void DJAppAutopilot::resetCandidates() {
    const bool hadCandidates = !m_candidates.isEmpty();
    m_candidates.clear();
    m_candidatesQueried = false;
    m_candidatesSourceLocation.clear();
    m_candidatesPlayingDeck = 0;
    m_candidatesOtherDeck = 0;
    if (hadCandidates) {
        emit candidatesChanged({});
    }
}

void DJAppAutopilot::triggerMix(int playingDeckNumber) {
    ControlProxy* pTrigger = playingDeckNumber == 1 ? &m_triggerToDeck2 : &m_triggerToDeck1;

    // Dan, 2026-10-09: brain.db's pair_scores.recipe_hint can differ from
    // whatever Dan left [AutomixTransition],recipe on - "fade_curat" when
    // this specific pair's tempo step is too big or either grid is
    // unreliable for a beatmatched mix. Flip the selector for this one
    // press only, the same as Dan manually choosing a different recipe row
    // right before pressing MIX himself, then restore his own choice
    // immediately after. arm() reads the selector synchronously inside
    // pTrigger->set(1.0) (no event-loop turn in between), so no tick can
    // land on the temporary value.
    const int originalRecipeIndex = static_cast<int>(m_recipeSelector.get());
    int overrideIndex = -1;
    if (!m_pickedRecipeHint.isEmpty()) {
        overrideIndex = AutomixRecipeBook::selectorIds().indexOf(m_pickedRecipeHint);
        if (overrideIndex < 0) {
            qWarning() << "DJ App autopilot: unknown recipe_hint" << m_pickedRecipeHint
                       << "- keeping Dan's own recipe selection";
        }
    }
    const bool overriding = overrideIndex >= 0 && overrideIndex != originalRecipeIndex;
    if (overriding) {
        qInfo() << "DJ App autopilot: using recipe" << m_pickedRecipeHint
                 << "for this transition (brain.db recipe_hint); Dan's own selection ("
                 << AutomixRecipeBook::selectorIds().value(originalRecipeIndex)
                 << ") resumes right after";
        m_recipeSelector.set(overrideIndex);
    }
    // Same effect as a button tap: press, then release.
    pTrigger->set(1.0);
    pTrigger->set(0.0);
    if (overriding) {
        m_recipeSelector.set(originalRecipeIndex);
    }

    m_overrideEmptyDeck = 0;
    m_awaitingLoadDeck = 0;
    m_pickedLabel.clear();
    m_pickedRisky = false;
    m_pickedRecipeHint.clear();
    m_pendingSeekDeck = 0;
    resetCandidates();
}

namespace {
struct LibraryPoolRead {
    QStringList poolLocations;
    QHash<QString, DJAppLibraryTrack> libraryByKey;
    QStringList playedToday;
};

// mixxx::DbConnectionPool hands out ONE connection per calling thread by a
// fixed name ("MIXXX-1" for the main thread); a second attempt on a thread
// that already holds one is refused ("Thread-local database connection
// already exists") and, worse, left Qt's SQL layer in a state that crashed
// soon after (confirmed live: a click on a candidate, on the main thread,
// that already owns the pool's main-thread connection elsewhere in Mixxx).
// Read mixxxdb.sqlite the same safe way BrainDbReader reads brain.db
// instead: our own short-lived, uniquely named, read-only connection that
// never touches Mixxx's own pool or its thread-affinity rules.
LibraryPoolRead readLibraryPool(const UserSettingsPointer& pConfig) {
    LibraryPoolRead out;
    const QString dbPath =
            QDir(pConfig->getSettingsPath()).filePath(QStringLiteral("mixxxdb.sqlite"));
    QString error;
    BrainDbReader::withReadOnlyConnection(
            dbPath,
            2000,
            [&](const QSqlDatabase& mixxxDb) {
                QString libraryError;
                const QList<DJAppLibraryTrack> tracks =
                        DJAppAnalysis::readLibraryTracks(mixxxDb, &libraryError);
                for (const DJAppLibraryTrack& t : tracks) {
                    out.poolLocations << t.location;
                    out.libraryByKey.insert(BrainDbReader::locationKey(t.location), t);
                }
                QString playedError;
                const QDateTime dayStart =
                        DJAppSuggestions::djDayStart(QDateTime::currentDateTime());
                out.playedToday = DJAppSuggestions::readPlayedSince(
                        mixxxDb, dayStart.toUTC(), &playedError);
            },
            &error);
    return out;
}
} // namespace

// static
QList<DJAppAutopilotCandidate> DJAppAutopilot::toDisplayList(const QList<Candidate>& candidates) {
    QList<DJAppAutopilotCandidate> out;
    out.reserve(candidates.size());
    for (const Candidate& c : candidates) {
        out.append(DJAppAutopilotCandidate{
                c.label, c.score, c.stepBpm, c.keyVerdict, c.allowed, c.recipeHint});
    }
    return out;
}

QList<DJAppAutopilot::Candidate> DJAppAutopilot::queryCandidates(
        int playingDeckNumber, int otherDeckNumber, bool* pRiskyFallback) {
    QList<Candidate> out;
    if (pRiskyFallback) {
        *pRiskyFallback = false;
    }
    const TrackPointer pSource = PlayerInfo::instance().getTrackInfo(
            PlayerManager::groupForDeck(playingDeckNumber - 1));
    if (!pSource) {
        return out;
    }
    const QString sourceLocation = pSource->getLocation();

    // Brief, bounded wait (a few dozen rows, same read Analiza does): simpler
    // and safer right now than restructuring this tick into an async
    // continuation, and it only runs inside the lookahead window / at the
    // deadline, not every tick.
    const LibraryPoolRead pool =
            readLibraryPool(m_pConfig);
    const QHash<QString, DJAppLibraryTrack>& libraryByKey = pool.libraryByKey;
    const QStringList& playedToday = pool.playedToday;

    QStringList excludeOnDecks;
    for (int d = 0; d < 2; ++d) {
        const TrackPointer pOnDeck =
                PlayerInfo::instance().getTrackInfo(PlayerManager::groupForDeck(d));
        if (pOnDeck) {
            excludeOnDecks << pOnDeck->getLocation();
        }
    }

    DJAppSuggestionRequest request;
    request.location = sourceLocation;
    request.recipe = QString::fromLatin1(DJAppSuggestions::kDefaultRecipe);
    request.limit = djapp::autopilot::kMaxCandidates;
    // Risky candidates are requested too now (changes 1 & 3): the fallback
    // when `allowed` is empty, and the rest of the up-to-3 picker list.
    request.riskyLimit = djapp::autopilot::kMaxCandidates;
    request.restrictToPool = true;
    request.pool = pool.poolLocations;
    request.exclude = playedToday + excludeOnDecks;

    const DJAppSuggestionResult result = DJAppSuggestions::query(brainDbPath(), request);
    const djapp::autopilot::CandidatePlan plan = djapp::autopilot::planCandidates(
            result.allowed.size(), result.risky.size());
    if (pRiskyFallback) {
        *pRiskyFallback = djapp::autopilot::isRiskyFallback(
                result.allowed.size(), result.risky.size());
    }

    auto resolve = [&](const DJAppSuggestion& suggestion, bool allowed) -> std::optional<Candidate> {
        const QString matchLocation =
                suggestion.poolLocation.isEmpty() ? suggestion.location : suggestion.poolLocation;
        const auto it = libraryByKey.constFind(BrainDbReader::locationKey(matchLocation));
        if (it == libraryByKey.constEnd()) {
            return std::nullopt;
        }
        const TrackPointer pTrack = m_pLibrary->trackCollectionManager()->getTrackById(
                TrackId(QVariant(it.value().id)));
        if (!pTrack) {
            return std::nullopt;
        }
        Candidate c;
        c.track = pTrack;
        c.label = trackLabel(it.value());
        c.score = suggestion.score;
        c.stepBpm = suggestion.stepBpm;
        c.keyVerdict = DJAppSuggestions::keyText(result.sourceCamelot, suggestion);
        c.allowed = allowed;
        c.recipeHint = suggestion.recipeHint;
        return c;
    };

    for (int i = 0; i < plan.allowedCount && i < result.allowed.size(); ++i) {
        if (auto c = resolve(result.allowed.at(i), true)) {
            out.append(*c);
        }
    }
    for (int i = 0; i < plan.riskyCount && i < result.risky.size(); ++i) {
        if (auto c = resolve(result.risky.at(i), false)) {
            out.append(*c);
        }
    }
    // Dan, 2026-10-09: "nu vreau sa mai ia prima optiune daca aceasta este
    // cu fade curat... fade curat sa aleaga doar daca nu are alta
    // optiune." A risky candidate can still have a non-fade_curat hint
    // (e.g. a key clash alone, with tempo and grid both fine) - push any
    // fade_curat candidate to the back, stable otherwise (allowed-before-
    // risky, score order within each, all untouched), so the first pick
    // only falls back to fade_curat when every other candidate is one too.
    std::stable_partition(out.begin(), out.end(), [](const Candidate& c) {
        return c.recipeHint != QLatin1String("fade_curat");
    });
    return out;
}

void DJAppAutopilot::loadCandidate(
        int playingDeckNumber, int otherDeckNumber, Candidate candidate) {
    m_awaitingLoadDeck = otherDeckNumber;
    if (m_overrideEmptyDeck == otherDeckNumber) {
        m_overrideEmptyDeck = 0;
    }
    const QString targetGroup = PlayerManager::groupForDeck(otherDeckNumber - 1);
    qInfo() << "DJ App autopilot: loading" << candidate.label << "onto" << targetGroup
            << "for deck" << playingDeckNumber << "mixing out"
            << (candidate.allowed ? "(allowed)" : "(risky fallback)");
    m_pickedLabel = candidate.label;
    m_pickedRisky = !candidate.allowed;
    m_pickedRecipeHint = candidate.recipeHint;
    // The picker list stays up (Dan: "nu vreau sa dispara lista, poate ma
    // razgandesc") - he may click a different row before the deadline,
    // which just reloads the free deck with that one instead. It clears
    // when the SOURCE track changes (refreshMixOutCache, a genuinely new
    // cycle) or the deadline is reached and the transition actually fires.
    emit loadTrackToPlayer(candidate.track, targetGroup, false);
    // Dan, 2026-10-09: "da play de la inceput" - nothing ever seeked a
    // newly loaded deck past its intro to brain's computed entry point;
    // tick() performs the actual seek once this deck reports loaded with a
    // known duration (readMixInSec here only does the brain.db lookup).
    // nullopt (no mix_points row) leaves the deck wherever Mixxx's own load
    // landed it, same as before this fix.
    const std::optional<double> mixInSec = readMixInSec(candidate.track->getLocation());
    m_pendingSeekDeck = mixInSec.has_value() ? otherDeckNumber : 0;
    m_pendingSeekSec = mixInSec.value_or(0.0);
    setStatus(Status{StatusKind::Picked, playingDeckNumber, 0.0, candidate.label, m_pickedRisky});
}

void DJAppAutopilot::showCandidates(int playingDeckNumber, int otherDeckNumber) {
    const TrackPointer pSource = PlayerInfo::instance().getTrackInfo(
            PlayerManager::groupForDeck(playingDeckNumber - 1));
    const QString sourceLocation = pSource ? pSource->getLocation() : QString();
    if (sourceLocation.isEmpty()) {
        return;
    }
    const bool sameTarget = m_candidatesQueried &&
            m_candidatesSourceLocation == sourceLocation &&
            m_candidatesOtherDeck == otherDeckNumber;
    if (sameTarget) {
        // Nothing new to do: either the auto-load below already fired for
        // this cycle (m_awaitingLoadDeck guards tick() from calling back in
        // here before that lands), or this is the true dead end and there
        // is nothing to retry without a new track on either deck.
        if (m_candidates.isEmpty()) {
            setStatus(Status{StatusKind::Picking, playingDeckNumber, 0.0, QString()});
        }
        return;
    }
    // The risky-fallback flag matters for the deadline's silent default
    // pick (pickAndLoad); the picker shows allowed vs. risky per row
    // instead (DJAppAutopilotCandidate::allowed), so it is not needed here.
    m_candidates = queryCandidates(playingDeckNumber, otherDeckNumber, nullptr);
    m_candidatesSourceLocation = sourceLocation;
    m_candidatesPlayingDeck = playingDeckNumber;
    m_candidatesOtherDeck = otherDeckNumber;
    m_candidatesQueried = true;
    emit candidatesChanged(toDisplayList(m_candidates));
    if (m_candidates.isEmpty()) {
        // Allowed AND risky both empty: the one true dead end (change 1).
        setStatus(Status{StatusKind::Picking, playingDeckNumber, 0.0, QString()});
        return;
    }
    // Dan, 2026-10-09: "intra prea devreme/iese prea tarziu, depaseste" -
    // waiting for the deadline to load a track (pickAndLoad) put the actual
    // track-load latency right on the critical path to the trigger, on top
    // of the engine's own bar-quantized start. Load the top candidate the
    // moment the picker opens instead, same as if Dan clicked row 0 himself
    // right away: by the deadline the track is long since loaded, decide()
    // goes straight to Action::Trigger, and there is only one load path
    // left to reason about (change 4: "nu alege mereu prima optiune"). Dan
    // can still click a different row any time before the deadline
    // (chooseCandidate reloads the free deck); clicking is just an earlier,
    // explicit version of the same load this already does implicitly.
    loadCandidate(playingDeckNumber, otherDeckNumber, m_candidates.first());
}

void DJAppAutopilot::pickAndLoad(int playingDeckNumber, int otherDeckNumber) {
    bool riskyFallback = false;
    const QList<Candidate> candidates =
            queryCandidates(playingDeckNumber, otherDeckNumber, &riskyFallback);
    if (candidates.isEmpty()) {
        // The one true dead end (change 1): neither an allowed nor a risky
        // candidate exists anywhere in the pool.
        setStatus(Status{StatusKind::Picking, playingDeckNumber, 0.0, QString()});
        return;
    }
    // Best allowed if any, else the best risky one -- flagged by
    // loadCandidate() via candidates.first().allowed (change 1 & 4).
    loadCandidate(playingDeckNumber, otherDeckNumber, candidates.first());
}

void DJAppAutopilot::tick() {
    const bool enabled = isEnabled();
    if (enabled != m_lastEnabled) {
        m_lastEnabled = enabled;
        emit enabledChanged(enabled);
    }
    refreshMixOutCache(1);
    refreshMixOutCache(2);

    if (m_pendingSeekDeck != 0) {
        DeckProxies& seekDeck = m_pendingSeekDeck == 1 ? m_deck1 : m_deck2;
        const double duration = seekDeck.duration.get();
        // Wait for the load to actually land (trackLoaded flips true before
        // duration is necessarily populated): retried every tick, no limit
        // needed -- the deadline is seconds to minutes away (fix #3 loads
        // the moment the picker opens), nothing else depends on this.
        if (seekDeck.trackLoaded.toBool() && duration > 0.0) {
            const double fraction = m_pendingSeekSec <= 0.0
                    ? 0.0
                    : std::min(1.0, m_pendingSeekSec / duration);
            seekDeck.playposition.set(fraction);
            m_pendingSeekDeck = 0;
        }
    }

    const int engineState = static_cast<int>(m_engineState.get());
    if (m_lastEngineState != 0 && engineState == 0 && m_pendingFromDeck != 0) {
        // A transition just finished: its outgoing deck is now free (left
        // alone, but eligible for the autopilot's next pick).
        m_overrideEmptyDeck = m_pendingFromDeck;
        m_pendingFromDeck = 0;
    }
    if (m_lastEngineState == 0 && engineState != 0) {
        m_pendingFromDeck = m_deck1.play.toBool() ? 1 : (m_deck2.play.toBool() ? 2 : 0);
    }
    m_lastEngineState = engineState;

    if (!enabled) {
        resetCandidates();
        m_pickedLabel.clear();
        m_pickedRisky = false;
        m_pickedRecipeHint.clear();
        m_pendingSeekDeck = 0;
        setStatus(Status{StatusKind::Off, 0, 0.0, QString()});
        return;
    }
    if (engineState == 1) {
        setStatus(Status{StatusKind::Armed, 0, 0.0, QString()});
        return;
    }
    if (engineState == 2) {
        const QString guardsText = QStringLiteral("GARDĂ VOCE %1 · GARDĂ TON %2")
                                            .arg(m_vocalGuardActiveStatus.toBool()
                                                            ? QStringLiteral("activă")
                                                            : QStringLiteral("inactivă"),
                                                    m_keyGuardActiveStatus.toBool()
                                                            ? QStringLiteral("activă")
                                                            : QStringLiteral("inactivă"));
        setStatus(Status{StatusKind::Running, 0, 0.0, guardsText});
        return;
    }

    Inputs in;
    in.enabled = enabled;
    in.engineIdle = engineState == 0;
    in.deck1Playing = m_deck1.play.toBool();
    in.deck2Playing = m_deck2.play.toBool();
    in.deck1Loaded = m_deck1.trackLoaded.toBool();
    in.deck2Loaded = m_deck2.trackLoaded.toBool();
    in.deck1PositionSec = m_deck1.playposition.get() * m_deck1.duration.get();
    in.deck2PositionSec = m_deck2.playposition.get() * m_deck2.duration.get();
    in.deck1MixOutSec = m_cachedMixOutSec[0];
    in.deck2MixOutSec = m_cachedMixOutSec[1];
    in.overrideEmptyDeck = m_overrideEmptyDeck;

    if (!in.deck1Playing && !in.deck2Playing) {
        setStatus(Status{StatusKind::NoPlayingDeck, 0, 0.0, QString()});
        return;
    }
    if (in.deck1Playing && in.deck2Playing) {
        setStatus(Status{StatusKind::BothPlaying, 0, 0.0, QString()});
        return;
    }

    const int playingDeck = in.deck1Playing ? 1 : 2;
    const std::optional<double> mixOutSec =
            playingDeck == 1 ? in.deck1MixOutSec : in.deck2MixOutSec;
    if (!mixOutSec.has_value()) {
        setStatus(Status{StatusKind::NoMixData, playingDeck, 0.0, QString()});
        return;
    }

    const Decision decision = decide(in);
    switch (decision.action) {
    case Action::None:
        if (decision.otherDeckNumber == 0) {
            // Far from the exit point yet (or just finished a cycle): a
            // fresh watch, so anything the picker/auto-pick showed for the
            // previous track no longer applies.
            resetCandidates();
            m_pickedLabel.clear();
            m_pickedRisky = false;
            m_pickedRecipeHint.clear();
            m_pendingSeekDeck = 0;
            setStatus(Status{StatusKind::Watching, playingDeck, mixOutSec.value(), QString()});
        } else if (!m_pickedLabel.isEmpty()) {
            // Inside the lookahead window, other deck already spoken for by
            // a click or a previous autopilot pick: nothing more to do
            // before the deadline triggers MIX.
            setStatus(Status{StatusKind::Picked, playingDeck, 0.0, m_pickedLabel, m_pickedRisky});
        } else {
            // Spoken for by Dan's own manual load (change 3's precedence
            // rule): leave it alone, same as the deadline's Trigger branch.
            setStatus(Status{StatusKind::Watching, playingDeck, mixOutSec.value(), QString()});
        }
        return;
    case Action::ShowCandidates:
        if (m_awaitingLoadDeck == decision.otherDeckNumber) {
            // A click (or the deadline, momentarily) already asked for a
            // load on that deck; wait for it to land instead of
            // re-querying brain.db every tick.
            setStatus(Status{StatusKind::Picked, playingDeck, 0.0, m_pickedLabel, m_pickedRisky});
            return;
        }
        showCandidates(decision.playingDeckNumber, decision.otherDeckNumber);
        return;
    case Action::PickAndLoad:
        if (m_awaitingLoadDeck == decision.otherDeckNumber) {
            setStatus(Status{StatusKind::Picked, playingDeck, 0.0, m_pickedLabel, m_pickedRisky});
            return;
        }
        pickAndLoad(decision.playingDeckNumber, decision.otherDeckNumber);
        return;
    case Action::Trigger:
        triggerMix(decision.playingDeckNumber);
        setStatus(Status{StatusKind::Armed, 0, 0.0, QString()});
        return;
    }
}
