#include "library/djapp/djappautopilot.h"

#include <QDateTime>
#include <QSqlDatabase>
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
#include "util/db/dbconnectionpooled.h"
#include "util/db/dbconnectionpooler.h"

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
    const QString targetKey = BrainDbReader::locationKey(location);
    BrainDbReader::withReadOnlyConnection(
            dbPath,
            2000,
            [&](const QSqlDatabase& db) {
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
}

void DJAppAutopilot::setStatus(const Status& status) {
    emit statusTextChanged(statusText(status));
}

void DJAppAutopilot::triggerMix(int playingDeckNumber) {
    ControlProxy* pTrigger = playingDeckNumber == 1 ? &m_triggerToDeck2 : &m_triggerToDeck1;
    // Same effect as a button tap: press, then release.
    pTrigger->set(1.0);
    pTrigger->set(0.0);
    m_overrideEmptyDeck = 0;
    m_awaitingLoadDeck = 0;
}

void DJAppAutopilot::pickAndLoad(int playingDeckNumber, int otherDeckNumber) {
    const TrackPointer pSource = PlayerInfo::instance().getTrackInfo(
            PlayerManager::groupForDeck(playingDeckNumber - 1));
    if (!pSource) {
        return;
    }
    const QString sourceLocation = pSource->getLocation();

    const mixxx::DbConnectionPoolPtr pPool = m_pLibrary->dbConnectionPool();
    const mixxx::DbConnectionPooler pooler(pPool);
    const QSqlDatabase mixxxDb = mixxx::DbConnectionPooled(pPool);

    QStringList poolLocations;
    QHash<QString, DJAppLibraryTrack> libraryByKey;
    QString playedError;
    QStringList playedToday;
    if (mixxxDb.isOpen()) {
        QString libraryError;
        const QList<DJAppLibraryTrack> tracks =
                DJAppAnalysis::readLibraryTracks(mixxxDb, &libraryError);
        for (const DJAppLibraryTrack& t : tracks) {
            poolLocations << t.location;
            libraryByKey.insert(BrainDbReader::locationKey(t.location), t);
        }
        const QDateTime dayStart = DJAppSuggestions::djDayStart(QDateTime::currentDateTime());
        playedToday = DJAppSuggestions::readPlayedSince(mixxxDb, dayStart.toUTC(), &playedError);
    }

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
    request.limit = 5;
    request.riskyLimit = 0;
    request.restrictToPool = true;
    request.pool = poolLocations;
    request.exclude = playedToday + excludeOnDecks;

    const DJAppSuggestionResult result = DJAppSuggestions::query(brainDbPath(), request);
    if (result.allowed.isEmpty()) {
        setStatus(Status{StatusKind::Picking, playingDeckNumber, 0.0, QString()});
        return;
    }

    const DJAppSuggestion& best = result.allowed.first();
    const QString matchLocation = best.poolLocation.isEmpty() ? best.location : best.poolLocation;
    const auto it = libraryByKey.constFind(BrainDbReader::locationKey(matchLocation));
    if (it == libraryByKey.constEnd()) {
        setStatus(Status{StatusKind::Picking, playingDeckNumber, 0.0, QString()});
        return;
    }

    const TrackPointer pTrack = m_pLibrary->trackCollectionManager()->getTrackById(
            TrackId(QVariant(it.value().id)));
    if (!pTrack) {
        setStatus(Status{StatusKind::Picking, playingDeckNumber, 0.0, QString()});
        return;
    }

    m_awaitingLoadDeck = otherDeckNumber;
    if (m_overrideEmptyDeck == otherDeckNumber) {
        m_overrideEmptyDeck = 0;
    }
    const QString targetGroup = PlayerManager::groupForDeck(otherDeckNumber - 1);
    qInfo() << "DJ App autopilot: loading" << trackLabel(it.value()) << "onto" << targetGroup
            << "for deck" << playingDeckNumber << "mixing out";
    emit loadTrackToPlayer(pTrack, targetGroup, false);
    setStatus(Status{StatusKind::Picked, playingDeckNumber, 0.0, trackLabel(it.value())});
}

void DJAppAutopilot::tick() {
    const bool enabled = isEnabled();
    if (enabled != m_lastEnabled) {
        m_lastEnabled = enabled;
        emit enabledChanged(enabled);
    }
    refreshMixOutCache(1);
    refreshMixOutCache(2);

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
        setStatus(Status{StatusKind::Off, 0, 0.0, QString()});
        return;
    }
    if (engineState == 1) {
        setStatus(Status{StatusKind::Armed, 0, 0.0, QString()});
        return;
    }
    if (engineState == 2) {
        setStatus(Status{StatusKind::Running, 0, 0.0, QString()});
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
        setStatus(Status{StatusKind::Watching, playingDeck, mixOutSec.value(), QString()});
        return;
    case Action::PickAndLoad:
        if (m_awaitingLoadDeck == decision.otherDeckNumber) {
            // Already asked for a load on that deck; wait for it to land
            // instead of re-querying brain.db every tick.
            setStatus(Status{StatusKind::Picked, playingDeck, 0.0, QString()});
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
