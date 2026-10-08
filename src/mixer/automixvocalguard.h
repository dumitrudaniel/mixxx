#pragma once

#include <QByteArray>
#include <QString>
#include <optional>
#include <vector>

namespace mixxx {
class Beats;
} // namespace mixxx

// DJ App vocal guard, "astept fraza" (Dan's practice, 2026-10-08). C++ port of
// brain/automix/vocalguard.py (plan_guard) used live by the automix executor
// (mixer/automixtransitioncontroller.h) on Mixxx 2.6 stem tracks:
//
//  1. The outgoing voice finishes the vocal block it is singing when the
//     transition starts, then fades out in fadeBeats (cos). If it is not
//     singing at the start it fades out right away; a block still running
//     after maxWaitBeats, or after the recipe's handover point (half its
//     length, where the bass and highs swap), is cut anyway: by then the EQ
//     lanes are taking the outgoing mids down, and waiting longer for the
//     voice leaves an energy hole (simulator, Gata Morena -> Masoupwel-la:
//     6.5 dB with the whole Standard 8 spent waiting).
//  2. The incoming voice stays muted until the outgoing fade has ended, then
//     fades in over fadeBeats (sin). If the outgoing phrase was cut, the two
//     voices crossfade over the same beats instead (no hole without a voice).
//     Equal-power, like the band swaps.
//
// Vocal blocks = vocal phrases merged over gaps shorter than kBlockGapBeats
// (brain/analysis/vocalmap.py merge_phrases / vocal_end_after): a beat or two
// between two lines of a verse is too short for another voice to come in.
//
// Positions are in TRANSITION beats: 0 = the bar the transition starts on,
// counted on the outgoing deck's grid. Pure math, unit tested in
// src/test/automixvocalguard_test.cpp.

struct AutomixVocalBlock {
    double start = 0.0;
    double end = 0.0;
};

struct AutomixVocalGuardPlan {
    // Outgoing voice: full until here, then cos fade over fadeBeats.
    double outgoingFadeStart = 0.0;
    // Incoming voice: muted until here, then sin fade over fadeBeats. Equal
    // to outgoingFadeStart when the outgoing phrase was cut (crossfade).
    double incomingFadeStart = 0.0;
    double fadeBeats = 0.0;
    // The outgoing block was still running when the wait ran out (maxWait or
    // the latest handover) and got cut.
    bool cutMidPhrase = false;
    // The incoming voice comes in in the middle of one of its blocks
    // (informative: the guard does not wait for the incoming phrasing).
    bool incomingMidPhrase = false;

    double outgoingGain(double beat) const;
    double incomingGain(double beat) const;
    // Transition beat from which both gains are final.
    double endBeat() const {
        return incomingFadeStart + fadeBeats;
    }
};

class AutomixVocalGuardPlanner {
  public:
    // Minimum silence (beats) between two vocal blocks: one bar.
    static constexpr double kBlockGapBeats = 4.0;

    // Sorts by start and merges blocks separated by less than minGapBeats.
    static std::vector<AutomixVocalBlock> mergeBlocks(
            std::vector<AutomixVocalBlock> blocks,
            double minGapBeats = kBlockGapBeats);

    // plan_guard(): blocks (phrases, merged here) in transition beats.
    // Outgoing fade start = min(end of the block containing beat 0,
    // maxWaitBeats, latestHandoverBeats), 0 if not singing at beat 0.
    // Incoming fade start = outgoing fade start + fadeBeats, or the outgoing
    // fade start itself (crossfade) when the phrase was cut.
    static AutomixVocalGuardPlan plan(const std::vector<AutomixVocalBlock>& outgoing,
            const std::vector<AutomixVocalBlock>& incoming,
            double maxWaitBeats,
            double fadeBeats,
            double latestHandoverBeats);

    // Latest start of the outgoing voice fade for a recipe of lengthBeats:
    // half of it, the bass/high swap point (Standard 8 -> 16, Scurt 4 -> 8,
    // Urgenta 2 -> 4, Lung 16 -> 32).
    static double latestHandoverBeats(double lengthBeats);

    // Raw little-endian float32 array of (start, end) pairs, as written by
    // brain/store/braindb.py pack_vector. A trailing odd value and pairs with
    // non-finite values are dropped.
    static std::vector<AutomixVocalBlock> parseFloat32Pairs(const QByteArray& blob);

    // Vocal segments in seconds of the audio file -> blocks in grid beats of
    // the deck's CURRENT grid (so grid corrections and edits are honored),
    // indexed like the transition clock (AutomixTransitionMath::gridBeatAt).
    // Segments that do not map to a positive span are dropped.
    static std::vector<AutomixVocalBlock> secondsToGridBeats(
            const mixxx::Beats& beats, const std::vector<AutomixVocalBlock>& segmentsSec);

    // Grid beats of one deck -> transition beats, assuming steady playback:
    // grid beat `referenceGridBeat` falls on transition beat
    // `referenceTransitionBeat`, and one grid beat lasts
    // `transitionBeatsPerGridBeat` transition beats (1 when tempo matched).
    //   t = referenceTransitionBeat + (b - referenceGridBeat) * transitionBeatsPerGridBeat
    static std::vector<AutomixVocalBlock> toTransitionBeats(
            const std::vector<AutomixVocalBlock>& gridBlocks,
            double referenceGridBeat,
            double referenceTransitionBeat,
            double transitionBeatsPerGridBeat = 1.0);
};

// Vocal map of one track, from brain.db (table vocal_maps).
struct AutomixVocalMap {
    // vocal_maps.location of the row used.
    QString location;
    // The track is a .stem.mp4 exported by brain; the map is its original's
    // (same audio timeline, so seconds map 1:1).
    bool viaStemExport = false;
    // 0 in brain.db = instrumental: its "vocals" stem carries the lead
    // melody (accordion, sax), so the guard must never touch it.
    bool hasVocals = false;
    // Vocal phrases (start_sec, end_sec) on the audio timeline of the file.
    std::vector<AutomixVocalBlock> segmentsSec;
};

class AutomixVocalMapStore {
  public:
    // Opens brain.db read-only (QSQLITE_OPEN_READONLY, never written) and
    // returns the vocal map of the track at `trackLocation`:
    //  1. the vocal_maps row with the same location, else
    //  2. if the location is a stem_exports.stem_path, the vocal_maps row of
    //     its source_location.
    // Locations compare like GridCorrector::normalizedLocation() (forward
    // slashes, Unicode-aware lower case in C++, not SQLite's ASCII lower()).
    // nullopt if there is no database, no table or no matching map.
    static std::optional<AutomixVocalMap> lookup(
            const QString& dbPath, const QString& trackLocation);
};
