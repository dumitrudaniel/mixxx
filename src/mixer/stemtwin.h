#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <optional>

#include "preferences/usersettings.h"
#include "track/track_decl.h"
#include "util/class.h"

class BaseTrackPlayer;
class Track;
class TrackCollectionManager;

// DJ App stem twins (decision #2 in docs/plan-ui-integrare.md, docs/decisions/0028,
// "opțiunea A"): when Dan loads an ORIGINAL track that has a brain `.stem.mp4`
// export, the deck plays the twin instead, so the stems-based guards
// (automixvocalguard.h, automixkeyguard.h) work on it; his playlists, cue
// edits and history stay on the original.
//
// Mechanism (deliberately not touching the engine's audio reader, Track or
// SoundSourceProxy, see ADR 0028's rejected "option B"): PlayerManager asks
// `substituteForLoad()` for the TrackPointer to actually hand to the deck.
// If a valid twin exists, that IS the twin Track (a normal Mixxx Track for
// the .stem.mp4 file, hidden from the library so Dan never sees it browsing),
// so everything that follows from a track being "the one on the deck" --
// stems, waveform, PlayerInfo, history, hotcue buttons -- naturally operates
// on it. Before Dan ever hears it, the twin's grid, cues/hotcues, key and
// ReplayGain are overwritten with the original's (one-way, every load: a
// stems take never carries its own edits forward). When the twin comes off
// the deck (replaced or ejected), its cue points (which may now include
// hotcues Dan set while it played) are copied BACK onto the original, which
// is then saved -- the only two-way part of the sync.
//
// `[DJApp],StemTwins` (mixxx.cfg, default on) switches the whole thing off:
// originals play as themselves, like before this feature existed.
class StemTwinController : public QObject {
    Q_OBJECT
  public:
    explicit StemTwinController(UserSettingsPointer pConfig, QObject* pParent);
    ~StemTwinController() override = default;

    // Needed to resolve/hide the twin's Track; unavailable until
    // PlayerManager::bindToLibrary() runs. A load before that (should not
    // happen in practice) just plays the original.
    void setTrackCollectionManager(TrackCollectionManager* pTrackCollectionManager);

    // Connects trackUnloaded so a plain eject (no new track loaded) still
    // copies a twin's cues back to its original.
    void watchPlayer(BaseTrackPlayer* pPlayer);

    // Called from PlayerManager::slotLoadTrackToPlayer with the track Dan
    // asked to load: returns that same track, unless brain.db has a current
    // twin for it, in which case the twin's copies are refreshed and IT is
    // returned (what actually gets loaded on the deck). Also releases
    // whatever twin was previously mapped to `group` (copies its cues back).
    //
    // "Current" (never plays a stale export): the twin's row in
    // stem_exports has the same sample rate as the original AND its
    // frame count matches the original's duration within a couple of
    // frames (kFrameTolerance) -- see docs/decisions/0028. Anything else
    // (no row, sample rate changed, length mismatch) falls back to the
    // original, logged once per load.
    TrackPointer substituteForLoad(const TrackPointer& pOriginal, const QString& group);

    // A couple of frames of slack on the "within a frame" length check
    // (docs/decisions/0028): Track::getDuration() round-trips through a
    // double of seconds, which can round the expected frame count by a
    // fraction of a frame either way. A stale export (wrong file version,
    // wrong source) misses by much more than this. Public: unit tested in
    // src/test/stemtwin_test.cpp without a live Track.
    static constexpr double kFrameTolerance = 2.0;

    // True if `location` is itself a stem export (never look for ITS twin).
    static bool isStemFileLocation(const QString& location);

    // The twin's stem_exports row (twinSampleRate, twinFrames) is still
    // current for a track whose known audio properties are
    // originalSampleRate / originalDurationSeconds: same sample rate, and
    // the twin's frame count within kFrameTolerance frames of the
    // original's duration. Pure, so it is unit-testable without a Track.
    static bool exportIsCurrent(double originalSampleRate,
            double originalDurationSeconds,
            double twinSampleRate,
            double twinFrames);

    // Grid (shared Beats, immutable, same sample rate per the re-export),
    // key and ReplayGain, plus a fresh clone of the cue points (so editing
    // one track's cues never mutates the other's).
    static void copyOntoTwin(const Track& original, Track* pTwin);

  private:
    struct TwinState {
        TrackPointer pOriginal;
        TrackPointer pTwin;
    };

    // If `group` has a mapped twin, copies its current cues back onto the
    // recorded original (saved via the collection manager) and clears the
    // mapping.
    void releaseGroup(const QString& group);

    UserSettingsPointer m_pConfig;
    TrackCollectionManager* m_pTrackCollectionManager = nullptr;
    QHash<QString, TwinState> m_byGroup;

    DISALLOW_COPY_AND_ASSIGN(StemTwinController);
};

// brain.db's stem_exports row for one original track (read-only lookup, like
// AutomixVocalMapStore / GridCorrector::lookup).
struct StemTwinInfo {
    QString stemPath;
    double sampleRate = 0.0;
    double frames = 0.0;
};

class StemTwinStore {
  public:
    // Opens brain.db read-only and returns the stem_exports row whose
    // source_location matches `originalLocation`
    // (GridCorrector::normalizedLocation() rules), or nullopt if there is no
    // database, no table or no matching row.
    static std::optional<StemTwinInfo> lookup(const QString& dbPath, const QString& originalLocation);
};
