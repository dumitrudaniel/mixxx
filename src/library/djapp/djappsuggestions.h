#pragma once

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <optional>

#include "library/djapp/djappanalysis.h"

// DJ App (docs/decisions/0029, Sugestii): the next-track suggestions for the
// track that is playing, read from brain.db (index-builder's contract, ADR 0026:
// track_index + pair_scores, automix/pairindex.py SUGGEST_SQL). Read only, on a
// worker thread; brain computes every score, Mixxx only lists them.

struct DJAppSuggestionTerm {
    QString name; // key, intro, outro, energy, play, tempo, vibe (+ any new one)
    double value = 0.0; // 0..1
};

// One pair_scores row A -> B, plus B's track_index fields.
struct DJAppSuggestion {
    QString location; // track_index.location of B (exact Mixxx location)
    int bId = -1;     // track_index.id of B
    double score = 0.0;
    bool allowed = false; // passes the strict rules (ADR 0024)
    bool clash = false;   // keys clash, or a key is unknown
    double stepBpm = 0.0; // BPM B - BPM A
    QList<DJAppSuggestionTerm> terms;
    QStringList flags;
    QString introNote;
    QString outroNote;
    QString recipeHint; // standard8, fade_curat, ...
    bool keyGuard = false; // GARDĂ TON will work on this pair
    std::optional<double> bpm; // B
    QString camelot;           // B, "" = unknown
    QString key;               // B, Mixxx key text

    // The pool location B matched (B itself, or its .stem.mp4 twin / original
    // when only that one is in the pool); "" without a pool.
    QString poolLocation;

    // Mixxx library (filled by the view's worker), -1 = not in the library.
    int mixxxTrackId = -1;
    QString artist;
    QString title;
};

struct DJAppSuggestionRequest {
    QString location; // A: the track to suggest after
    QString recipe = QStringLiteral("standard8");
    int limit = 10;      // rows that pass the strict rules
    int riskyLimit = 5;  // rows that do not (keys, tempo, Dan's "bad" flags)
    QStringList exclude; // locations not to suggest (played today, on decks, "Nu acum")
    // Only suggest tracks of this pool (the Mixxx library; later a crate or a
    // playlist). brain may index files this Mixxx does not have.
    bool restrictToPool = false;
    QStringList pool;
};

struct DJAppSuggestionResult {
    bool ok = false; // brain.db opened and the index tables read
    QString error;
    bool hasIndex = false;      // track_index + pair_scores exist
    bool sourceIndexed = false; // A is in track_index
    QString sourceLocation;     // A as brain knows it
    bool sourceViaStemExport = false; // A was a .stem.mp4: its original's row
    QString sourceState;        // track_index.state of A
    std::optional<double> sourceBpm;
    QString sourceCamelot;
    QList<DJAppSuggestion> allowed;
    QList<DJAppSuggestion> risky;
    int skipped = 0; // rows dropped: excluded, A itself, or a .stem.mp4 twin shown already
    int outsidePool = 0; // rows dropped because B is not in the pool
    qint64 queryMs = 0;
};

// A deck as the "free deck" choice sees it.
struct DJAppDeckState {
    QString group;      // [ChannelN]
    bool playing = false;
    bool loaded = false;
};

class DJAppSuggestions {
  public:
    static constexpr const char* kDefaultRecipe = "standard8";

    // Reads the suggestions for `request.location` from brain.db, read-only.
    static DJAppSuggestionResult query(const QString& brainDbPath,
            const DJAppSuggestionRequest& request,
            int busyTimeoutMs = 2000);

    // "terms" JSON -> the known terms in display order, then any new ones.
    static QList<DJAppSuggestionTerm> parseTerms(const QString& json);
    // "a|b|c" -> [a, b, c]
    static QStringList parseFlags(const QString& flags);

    // Display texts (Romanian).
    static QString recipeText(const QString& recipeHint);
    static QString termName(const QString& term);
    static QString scoreText(double score);  // ",94"
    static QString scoreBar(double score);   // "███████▌"
    static QString stepText(double stepBpm); // "+2,0" / "−3,0" / "±0"
    static QString keyText(const QString& camelotA, const DJAppSuggestion& suggestion);
    // 2–3 words for the row: the first of Dan's "bad" flags, else the strongest
    // intro detail.
    static QString shortReason(const DJAppSuggestion& suggestion);
    // The "De ce?" lines: key + tempo, intro, outro, flags, score terms.
    static QStringList whyLines(const QString& camelotA, const DJAppSuggestion& suggestion);

    // Start of the "DJ day" containing `nowLocal`: 06:00 local time, so a
    // night set that crosses midnight stays one day.
    static QDateTime djDayStart(const QDateTime& nowLocal);

    // Locations of the tracks Mixxx played since `sinceUtc` (history playlists
    // + library.last_played_at), on Mixxx's own connection. Read only.
    static QStringList readPlayedSince(
            const QSqlDatabase& mixxxDb, const QDateTime& sinceUtc, QString* pError = nullptr);

    // The deck to load a suggestion on: a stopped empty deck first, then a
    // stopped deck other than `sourceGroup`; "" if every deck is busy.
    static QString chooseFreeDeck(const QList<DJAppDeckState>& decks, const QString& sourceGroup);

    // JSON body of POST /feedback (brain-service, plan §3.7). Stub until the
    // HTTP client exists.
    static QByteArray feedbackJson(const QString& sourceLocation,
            const DJAppSuggestion& suggestion,
            const QString& outcome,
            int position);
};
