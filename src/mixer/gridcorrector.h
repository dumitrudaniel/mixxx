#pragma once

#include <QObject>
#include <QString>
#include <optional>

#include "preferences/usersettings.h"
#include "track/track_decl.h"
#include "util/class.h"

class BaseTrackPlayer;

// DJ App, docs/decisions/0018 + 0019: applies the beat grid corrections that
// brain/ computed by rhythm-pattern consensus over the library (table
// grid_corrections in brain.db). Mixxx's analyzer gets the tempo right on
// Dan's tracks but sometimes puts the grid on the off-beats, or puts beat 1 of
// the grid on beat 2 of the pattern; quantize and MIX then align the wrong
// thing.
//
// On track load: look up the track's correction (brain.db opened read-only)
// and, only if the track's CURRENT grid is still exactly the constant grid
// the correction was computed on, translate it by correction_beats. Mixxx then
// saves the translated grid in its own library like any manual grid edit
// (and it can be undone with Mixxx's beatgrid undo). The "still the original
// grid" check makes this idempotent: once applied, the grid no longer
// matches, and a grid Dan edited by hand is never touched.
//
// Config ([DJApp] in mixxx.cfg): BrainDb = path to brain.db (empty = off),
// GridCorrection = 1/0.
class GridCorrector : public QObject {
    Q_OBJECT
  public:
    struct Correction {
        double originalBpm = 0.0;
        double originalAnchorFrame = 0.0;
        double sampleRate = 0.0;
        double correctionBeats = 0.0;
        QString reason;
    };

    GridCorrector(UserSettingsPointer pConfig, QObject* pParent);
    ~GridCorrector() override = default;

    void watchPlayer(BaseTrackPlayer* pPlayer);

    // Frames to translate the grid by, or nullopt if the current constant
    // grid (bpm, anchor frame, sample rate) is not the one the correction was
    // computed on, or there is nothing to do.
    static std::optional<double> offsetFrames(double currentBpm,
            double currentAnchorFrame,
            double currentSampleRate,
            const Correction& correction);

    // Same normalization on both sides of the lookup: forward slashes,
    // lower case (Windows paths are case-insensitive).
    static QString normalizedLocation(const QString& location);

  private slots:
    void slotTrackLoaded(TrackPointer pTrack);

  private:
    std::optional<Correction> lookup(const QString& location) const;

    UserSettingsPointer m_pConfig;

    DISALLOW_COPY_AND_ASSIGN(GridCorrector);
};
