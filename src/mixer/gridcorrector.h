#pragma once

#include <QObject>
#include <QString>
#include <optional>

#include "preferences/usersettings.h"
#include "track/beats.h"
#include "track/track_decl.h"
#include "util/class.h"

class BaseTrackPlayer;

// DJ App, docs/decisions/0018 + 0019 + 0025: applies the beat grid corrections
// that brain/ computed (table grid_corrections in brain.db).
//
//  - Phase rows (kind 'phase', or older brain.db without the kind column):
//    Mixxx's tempo is right but the grid sits on the off-beats, or beat 1 of
//    the grid is on beat 2 of the pattern; the grid is translated by
//    correction_beats.
//  - Tempo rows (kind 'tempo' / 'tempo+phase', ADR 0025): the tempo itself is
//    wrong (octave or 4:3 error, or a small constant error); the row holds the
//    full target grid and the track gets a constant grid at corrected_bpm with
//    its first beat at corrected_first_beat_frame.
//
// On track load: look up the track's correction (brain.db opened read-only)
// and, only if the track's CURRENT grid is still exactly the constant grid
// the correction was computed on, set the corrected grid. Mixxx then saves it
// in its own library like any manual grid edit (and it can be undone with
// Mixxx's beatgrid undo). The "still the original grid" check makes this
// idempotent: once applied, the grid no longer matches, and a grid Dan edited
// by hand is never touched. A track whose BPM is locked is never touched.
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
        // ADR 0025. Empty kind = a row written before the tempo corrections
        // (same as "phase").
        QString kind;
        double correctedBpm = 0.0;
        double correctedAnchorFrame = 0.0;

        // A 'tempo' / 'tempo+phase' row with a usable target grid.
        bool correctsTempo() const;
    };

    GridCorrector(UserSettingsPointer pConfig, QObject* pParent);
    ~GridCorrector() override = default;

    void watchPlayer(BaseTrackPlayer* pPlayer);

    // The current constant grid (bpm, anchor frame, sample rate) is exactly
    // the one the correction was computed on.
    static bool matchesAnalyzedGrid(double currentBpm,
            double currentAnchorFrame,
            double currentSampleRate,
            const Correction& correction);

    // Phase rows: frames to translate the grid by, or nullopt if the current
    // constant grid is not the one the correction was computed on, or there is
    // nothing to do.
    static std::optional<double> offsetFrames(double currentBpm,
            double currentAnchorFrame,
            double currentSampleRate,
            const Correction& correction);

    // The beats to set for `correction` on top of `pCurrent`, or nullptr if
    // the correction does not apply (non-constant grid, grid differs from the
    // analyzed one, nothing to do, invalid row). Keeps the grid's sub version,
    // like a manual edit, so Mixxx does not re-analyze the track.
    static mixxx::BeatsPointer correctedBeats(
            const mixxx::BeatsPointer& pCurrent, const Correction& correction);

    // Applies `correction` to the track: never if its BPM is locked, only via
    // correctedBeats(). Returns true if the track's grid was changed.
    static bool applyTo(const TrackPointer& pTrack, const Correction& correction);

    // The row to apply (apply = 1) for `location` in the brain.db at `dbPath`,
    // opened read-only. Works with a brain.db from before ADR 0025 (no kind /
    // corrected_* columns): those rows are phase rows.
    static std::optional<Correction> lookup(const QString& dbPath, const QString& location);

    // Same normalization on both sides of the lookup: forward slashes,
    // lower case (Windows paths are case-insensitive).
    static QString normalizedLocation(const QString& location);

  private slots:
    void slotTrackLoaded(TrackPointer pTrack);

  private:
    UserSettingsPointer m_pConfig;

    DISALLOW_COPY_AND_ASSIGN(GridCorrector);
};
