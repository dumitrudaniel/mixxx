#pragma once

#include <QList>
#include <QSqlDatabase>
#include <QString>

#include "library/djapp/braindbreader.h"

// DJ App (docs/decisions/0029): the rows of the "Analiză" view. Pure
// functions: Mixxx's library tracks joined with a brain.db snapshot by
// normalized location. Texts are Romanian, like the rest of DJ App.

// One track of the Mixxx library (read from Mixxx's own database).
struct DJAppLibraryTrack {
    int id = -1;
    QString location;
    QString artist;
    QString title;
    double bpm = 0.0;
    QString key;
};

enum class DJAppBrainStage {
    NotInBrain, // brain has not imported the file yet
    Queued,     // in analysis_queue, not started
    Working,    // a stage in progress
    Done,       // analysis_queue.stage = done
    Error,      // failed, or an error on the last attempt
};

struct DJAppAnalysisRow {
    DJAppLibraryTrack track;
    QString trackText; // "Artist – Title", or the file name

    QString mixxxText; // "ok", "fără BPM", "fără tonalitate"
    bool mixxxReady = false;

    DJAppBrainStage stage = DJAppBrainStage::NotInBrain;
    QString stageText;
    QString stageTooltip;

    // The library file is a .stem.mp4 exported by brain; brain's analysis of
    // its original applies (same timeline).
    bool isStemCopy = false;
    QString stemText;
    QString vocalText;
    QString gridText;
    QString gridTooltip;
    bool gridToApply = false;
    bool variableTempo = false;
    QString mixPointsText;
    QString mixPointsTooltip;
    QString indexText; // track_index.state (ADR 0026), "—" without the table
};

struct DJAppAnalysisSummary {
    int total = 0;
    int analysed = 0;
    int pending = 0; // queued or working
    int errors = 0;
    int notInBrain = 0;
    int variableTempo = 0;
    int gridToApply = 0;

    // Tracks still waiting for brain (sidebar badge "Analiză N").
    int waiting() const {
        return pending + notInBrain;
    }
};

class DJAppAnalysis {
  public:
    // SELECT on Mixxx's library (not deleted tracks), on a connection owned by
    // the calling thread. Read only.
    static QList<DJAppLibraryTrack> readLibraryTracks(
            const QSqlDatabase& mixxxDb, QString* pError = nullptr);

    static DJAppAnalysisRow buildRow(const DJAppLibraryTrack& track, const BrainSnapshot& brain);
    static QList<DJAppAnalysisRow> buildRows(
            const QList<DJAppLibraryTrack>& tracks, const BrainSnapshot& brain);
    static DJAppAnalysisSummary summarize(const QList<DJAppAnalysisRow>& rows);

    // "91 analizate · 2 în coadă · 8 cu tempo variabil · 0 erori · 3 nu sunt în brain"
    static QString summaryText(const DJAppAnalysisSummary& summary);
    // "brain.db · citit la 22:14:03 în 12 ms · jurnal WAL · 93 piese în brain"
    static QString snapshotText(const BrainSnapshot& brain);
};
