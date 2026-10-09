#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QVector>

// DJ App (docs/decisions/0032): brain.db-derived markers for WOverview's
// mini waveform (the small per-deck scrub/seek widget, NOT the big zoomed
// waveform) -- mix-in/mix-out points, vocal-presence regions and section
// boundaries, read-only from brain.db.
//
// The pure data types and pure parsing/positioning functions live here and
// are unit tested in src/test/djappoverviewmarkers_test.cpp. The actual DB
// read (fetchOverviewMarkers) mirrors DJAppAutopilot::readMixOutSec /
// readMixInSec exactly: BrainDbReader::withReadOnlyConnection, location
// matching via BrainDbReader::locationKey, and the same stem-twin
// resolution via stem_exports (ADR 0028) before querying mix_points /
// vocal_maps / sections. It is exercised live (WOverview calls it off the
// UI thread via QtConcurrent::run), not unit tested, same as the rest of
// that established pattern elsewhere in djapp/.
namespace djapp::overview {

// One vocal-presence region on the track's own timeline, in seconds.
struct VocalSegment {
    double startSec = 0.0;
    double endSec = 0.0;
};

// One section boundary (sections.section_type: intro | verse | build |
// drop | breakdown | outro), in seconds on the track's own timeline.
struct SectionMark {
    double startSec = 0.0;
    double endSec = 0.0;
    QString sectionType;
};

struct BrainMarkers {
    // True if a mix_points row was found for this track (mixInSec/mixOutSec
    // are only meaningful when this is true). vocalSegments/sections are
    // independent of this flag -- a track can have sections/vocal data
    // without a mix_points row yet, or vice versa.
    bool valid = false;
    double mixInSec = -1.0; // -1 = none
    QVector<double> mixOutSec; // all recipe lengths' mix-out points, sorted ascending, deduped
    QVector<VocalSegment> vocalSegments; // empty if instrumental or no vocal map yet
    QVector<SectionMark> sections; // sorted by startSec
};

// Decodes vocal_maps.vocal_segments_sec: float32 little-endian pairs
// (start, end, start, end, ...), brain's store.braindb.pack_vector format
// (see brain/store/schema.sql). A trailing unpaired float (malformed/
// truncated blob) is dropped, never guessed. Empty/too-short blob gives an
// empty result.
QVector<VocalSegment> parseVocalSegmentsSec(const QByteArray& blob);

// All values of mix_points.mix_out_sec's {"beats": seconds} map (already
// parsed, e.g. by djapp::autopilot::parseMixOutMap), sorted ascending, with
// near-duplicate values (within 0.05s -- different recipe lengths landing
// on the same drum entry point) collapsed to one. Dan asked to see "the
// mix points" without picking a single recipe length, so this shows all of
// them rather than just the one for the currently-selected recipe
// (documented in docs/decisions/0032).
QVector<double> dedupedMixOutValues(const QMap<int, double>& byLength);

// Reads everything above for `location` from brain.db at `dbPath`,
// resolving a stem-twin's location back to its original via stem_exports
// first (ADR 0028), same as DJAppAutopilot::readMixOutSec. Safe to call
// with an empty/missing dbPath or no row for this track: returns a
// default-constructed (valid=false, all vectors empty) BrainMarkers, never
// throws. Synchronous -- call off the UI thread.
BrainMarkers fetchOverviewMarkers(
        const QString& dbPath, const QString& location, int busyTimeoutMs = 2000);

} // namespace djapp::overview
