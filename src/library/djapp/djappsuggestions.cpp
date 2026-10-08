#include "library/djapp/djappsuggestions.h"

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimeZone>
#include <QVariant>
#include <QtDebug>
#include <algorithm>
#include <cmath>

#include "library/djapp/braindbreader.h"

namespace {

const QString kTrackIndex = QStringLiteral("track_index");
const QString kPairScores = QStringLiteral("pair_scores");
const QString kStemExports = QStringLiteral("stem_exports");

// The pair_scores columns SUGGEST_SQL reads (automix/pairindex.py).
const QStringList kPairColumns = {
        QStringLiteral("recipe"),
        QStringLiteral("a_id"),
        QStringLiteral("b_id"),
        QStringLiteral("score"),
        QStringLiteral("allowed"),
        QStringLiteral("clash"),
        QStringLiteral("step_bpm"),
        QStringLiteral("terms"),
        QStringLiteral("flags"),
        QStringLiteral("intro_note"),
        QStringLiteral("outro_note"),
        QStringLiteral("recipe_hint"),
        QStringLiteral("key_guard"),
};

// ADR 0024 terms, in the order "De ce?" lists them.
const QStringList kTermOrder = {
        QStringLiteral("key"),
        QStringLiteral("intro"),
        QStringLiteral("outro"),
        QStringLiteral("energy"),
        QStringLiteral("play"),
        QStringLiteral("tempo"),
        QStringLiteral("vibe"),
};

const QLocale& romanian() {
    static const QLocale kRomanian(QLocale::Romanian, QLocale::Romania);
    return kRomanian;
}

std::optional<double> optionalDouble(const QVariant& value) {
    if (value.isNull()) {
        return std::nullopt;
    }
    bool ok = false;
    const double result = value.toDouble(&ok);
    return ok ? std::optional<double>(result) : std::nullopt;
}

struct IndexRow {
    int id = -1;
    QString location;
    QString state;
    std::optional<double> bpm;
    QString camelot;
};

// "t.<name>" if track_index has the column, else NULL.
QString indexColumn(const QSet<QString>& columns, const QString& name) {
    return columns.contains(name) ? QStringLiteral("t.") + name : QStringLiteral("NULL");
}

class SuggestionReader {
  public:
    SuggestionReader(const QSqlDatabase& db,
            const DJAppSuggestionRequest& request,
            DJAppSuggestionResult* pResult)
            : m_db(db),
              m_request(request),
              m_pResult(pResult) {
    }

    void run() {
        QString error;
        const QSet<QString> tables = BrainDbReader::tableNames(m_db, &error);
        if (!error.isEmpty()) {
            m_pResult->error = error;
            return;
        }
        m_pResult->hasIndex = tables.contains(kTrackIndex) && tables.contains(kPairScores);
        if (!m_pResult->hasIndex) {
            m_pResult->ok = true; // readable, just no index yet
            return;
        }
        m_indexColumns = BrainDbReader::columnNames(m_db, kTrackIndex);
        if (!m_indexColumns.contains(QStringLiteral("id")) ||
                !m_indexColumns.contains(QStringLiteral("location"))) {
            m_pResult->error = QStringLiteral("track_index are altă schemă (lipsesc id/location)");
            return;
        }
        const QSet<QString> pairColumns = BrainDbReader::columnNames(m_db, kPairScores);
        for (const QString& name : kPairColumns) {
            if (!pairColumns.contains(name)) {
                m_pResult->error = QStringLiteral("pair_scores are altă schemă (lipsește %1)")
                                           .arg(name);
                return;
            }
        }
        if (tables.contains(kStemExports)) {
            readStemExports();
        }
        if (!resolveSource()) {
            m_pResult->ok = true; // A not indexed yet: nothing to suggest
            return;
        }
        buildExclusions();
        buildPool();
        bool ok = fetch(true, m_request.limit, &m_pResult->allowed) &&
                fetch(false, m_request.riskyLimit, &m_pResult->risky);
        m_pResult->ok = ok;
    }

  private:
    void readStemExports() {
        QSqlQuery query(m_db);
        if (!query.exec(QStringLiteral("SELECT source_location, stem_path FROM stem_exports"))) {
            return; // optional: no twins known
        }
        while (query.next()) {
            const QString source = query.value(0).toString();
            const QString stem = query.value(1).toString();
            if (source.isEmpty() || stem.isEmpty()) {
                continue;
            }
            m_stemToSource.insert(BrainDbReader::locationKey(stem), source);
            m_sourceToStem.insert(BrainDbReader::locationKey(source), stem);
        }
    }

    // The original's key for a brain .stem.mp4 export, else `key` itself.
    QString identity(const QString& key) const {
        const auto it = m_stemToSource.constFind(key);
        return it == m_stemToSource.constEnd() ? key : BrainDbReader::locationKey(it.value());
    }

    std::optional<IndexRow> readIndexRow(QSqlQuery& query) const {
        IndexRow row;
        row.id = query.value(0).toInt();
        row.location = query.value(1).toString();
        row.state = query.value(2).toString();
        row.bpm = optionalDouble(query.value(3));
        row.camelot = query.value(4).toString();
        return row;
    }

    std::optional<IndexRow> findIndexRow(const QString& location) const {
        const QString key = BrainDbReader::locationKey(location);
        const QString columns = QStringLiteral("t.id, t.location, %1, %2, %3")
                                        .arg(indexColumn(m_indexColumns, QStringLiteral("state")),
                                                indexColumn(m_indexColumns, QStringLiteral("bpm")),
                                                indexColumn(m_indexColumns,
                                                        QStringLiteral("camelot")));
        QSqlQuery query(m_db);
        if (m_indexColumns.contains(QStringLiteral("location_key"))) {
            // The indexed lookup brain built for us (idx_track_index_key).
            query.prepare(QStringLiteral("SELECT %1 FROM track_index t WHERE t.location_key = ?")
                                  .arg(columns));
            query.addBindValue(key);
            if (query.exec()) {
                while (query.next()) {
                    if (BrainDbReader::locationKey(query.value(1).toString()) == key) {
                        return readIndexRow(query);
                    }
                }
            }
        }
        // Fallback: compare in C++ (a location_key written with another rule,
        // or an index without the column). Small columns only.
        if (!query.exec(QStringLiteral("SELECT %1 FROM track_index t").arg(columns))) {
            return std::nullopt;
        }
        while (query.next()) {
            if (BrainDbReader::locationKey(query.value(1).toString()) == key) {
                return readIndexRow(query);
            }
        }
        return std::nullopt;
    }

    bool resolveSource() {
        std::optional<IndexRow> source = findIndexRow(m_request.location);
        if (!source) {
            // Dan loads brain's .stem.mp4 twin in 2.6: the original's row applies.
            const auto it = m_stemToSource.constFind(BrainDbReader::locationKey(m_request.location));
            if (it != m_stemToSource.constEnd()) {
                source = findIndexRow(it.value());
                m_pResult->sourceViaStemExport = source.has_value();
            }
        }
        if (!source) {
            return false;
        }
        m_sourceId = source->id;
        m_pResult->sourceIndexed = true;
        m_pResult->sourceLocation = source->location;
        m_pResult->sourceState = source->state;
        m_pResult->sourceBpm = source->bpm;
        m_pResult->sourceCamelot = source->camelot;
        return true;
    }

    void buildExclusions() {
        QStringList excluded = m_request.exclude;
        excluded << m_request.location << m_pResult->sourceLocation;
        for (const QString& location : std::as_const(excluded)) {
            const QString key = BrainDbReader::locationKey(location);
            m_excluded.insert(identity(key));
        }
    }

    void buildPool() {
        if (!m_request.restrictToPool) {
            return;
        }
        for (const QString& location : m_request.pool) {
            const QString key = BrainDbReader::locationKey(location);
            const QString id = identity(key);
            // Prefer the pool's own original over its .stem.mp4 twin.
            if (!m_pool.contains(id) || key == id) {
                m_pool.insert(id, location);
            }
        }
    }

    bool fetch(bool allowed, int wanted, QList<DJAppSuggestion>* pOut) {
        if (wanted <= 0) {
            return true;
        }
        // The margin covers rows dropped by the exclusions, the twin dedupe and
        // the pool; if it is not enough, read again with a larger limit.
        int limit = wanted * 2 + static_cast<int>(m_excluded.size()) + 10;
        const QSet<QString> seenBefore = m_seen;
        const int skippedBefore = m_pResult->skipped;
        const int outsideBefore = m_pResult->outsidePool;
        for (;;) {
            pOut->clear();
            m_seen = seenBefore;
            m_pResult->skipped = skippedBefore;
            m_pResult->outsidePool = outsideBefore;
            int rows = 0;
            if (!fetchOnce(allowed, wanted, limit, pOut, &rows)) {
                return false;
            }
            if (pOut->size() >= wanted || rows < limit) {
                return true;
            }
            limit *= 4;
        }
    }

    bool fetchOnce(bool allowed, int wanted, int limit, QList<DJAppSuggestion>* pOut, int* pRows) {
        // Same columns and order as SUGGEST_SQL (automix/pairindex.py), split by
        // `allowed` and with a_id resolved first; both use
        // idx_pair_scores_suggest (recipe, a_id, allowed, score DESC).
        QSqlQuery query(m_db);
        query.setForwardOnly(true);
        query.prepare(QStringLiteral(
                "SELECT t.location, p.score, p.allowed, p.clash, p.step_bpm, p.terms, p.flags, "
                "p.intro_note, p.outro_note, p.recipe_hint, p.key_guard, p.b_id, %1, %2, %3 "
                "FROM pair_scores p JOIN track_index t ON t.id = p.b_id "
                "WHERE p.recipe = ? AND p.a_id = ? AND p.allowed = ? "
                "ORDER BY p.score DESC LIMIT ?")
                              .arg(indexColumn(m_indexColumns, QStringLiteral("bpm")),
                                      indexColumn(m_indexColumns, QStringLiteral("camelot")),
                                      indexColumn(m_indexColumns, QStringLiteral("key"))));
        query.addBindValue(m_request.recipe);
        query.addBindValue(m_sourceId);
        query.addBindValue(allowed ? 1 : 0);
        query.addBindValue(limit);
        if (!query.exec()) {
            m_pResult->error = query.lastError().text();
            return false;
        }
        *pRows = 0;
        while (query.next()) {
            ++*pRows;
            if (pOut->size() >= wanted) {
                continue; // count the rows only
            }
            DJAppSuggestion suggestion;
            suggestion.location = query.value(0).toString();
            const QString id = identity(BrainDbReader::locationKey(suggestion.location));
            if (m_excluded.contains(id) || m_seen.contains(id)) {
                ++m_pResult->skipped;
                continue;
            }
            if (m_request.restrictToPool) {
                const auto pool = m_pool.constFind(id);
                if (pool == m_pool.constEnd()) {
                    ++m_pResult->outsidePool;
                    continue;
                }
                suggestion.poolLocation = pool.value();
            }
            m_seen.insert(id);
            suggestion.score = query.value(1).toDouble();
            suggestion.allowed = query.value(2).toInt() != 0;
            suggestion.clash = query.value(3).toInt() != 0;
            suggestion.stepBpm = query.value(4).toDouble();
            suggestion.terms = DJAppSuggestions::parseTerms(query.value(5).toString());
            suggestion.flags = DJAppSuggestions::parseFlags(query.value(6).toString());
            suggestion.introNote = query.value(7).toString();
            suggestion.outroNote = query.value(8).toString();
            suggestion.recipeHint = query.value(9).toString();
            suggestion.keyGuard = query.value(10).toInt() != 0;
            suggestion.bId = query.value(11).toInt();
            suggestion.bpm = optionalDouble(query.value(12));
            suggestion.camelot = query.value(13).toString();
            suggestion.key = query.value(14).toString();
            pOut->append(suggestion);
        }
        return true;
    }

    const QSqlDatabase& m_db;
    const DJAppSuggestionRequest& m_request;
    DJAppSuggestionResult* m_pResult;
    QSet<QString> m_indexColumns;
    QHash<QString, QString> m_stemToSource; // key(stem) -> source location
    QHash<QString, QString> m_sourceToStem; // key(source) -> stem location
    int m_sourceId = -1;
    QSet<QString> m_excluded; // identities
    QSet<QString> m_seen;     // identities already listed
    QHash<QString, QString> m_pool; // identity -> pool location
};

} // namespace

// static
DJAppSuggestionResult DJAppSuggestions::query(const QString& brainDbPath,
        const DJAppSuggestionRequest& request,
        int busyTimeoutMs) {
    DJAppSuggestionResult result;
    if (request.location.isEmpty()) {
        result.error = QStringLiteral("nicio piesă");
        return result;
    }
    QElapsedTimer timer;
    timer.start();
    BrainDbReader::withReadOnlyConnection(
            brainDbPath,
            busyTimeoutMs,
            [&request, &result](const QSqlDatabase& db) {
                SuggestionReader(db, request, &result).run();
            },
            &result.error);
    result.queryMs = timer.elapsed();
    if (!result.ok) {
        result.allowed.clear();
        result.risky.clear();
        qWarning() << "DJ App: cannot read suggestions from brain.db" << brainDbPath
                   << result.error;
    }
    return result;
}

// static
QList<DJAppSuggestionTerm> DJAppSuggestions::parseTerms(const QString& json) {
    QList<DJAppSuggestionTerm> terms;
    const QJsonObject object = QJsonDocument::fromJson(json.toUtf8()).object();
    for (const QString& name : kTermOrder) {
        if (object.contains(name)) {
            terms.append({name, object.value(name).toDouble()});
        }
    }
    // Terms added to the score later are shown too, after the known ones.
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!kTermOrder.contains(it.key())) {
            terms.append({it.key(), it.value().toDouble()});
        }
    }
    return terms;
}

// static
QStringList DJAppSuggestions::parseFlags(const QString& flags) {
    QStringList out;
    for (const QString& flag : flags.split(QLatin1Char('|'), Qt::SkipEmptyParts)) {
        const QString trimmed = flag.trimmed();
        if (!trimmed.isEmpty()) {
            out.append(trimmed);
        }
    }
    return out;
}

// static
QString DJAppSuggestions::recipeText(const QString& recipeHint) {
    static const QHash<QString, QString> kNames = {
            {QStringLiteral("standard8"), QStringLiteral("Standard 8")},
            {QStringLiteral("scurt4"), QStringLiteral("Scurt 4")},
            {QStringLiteral("lung16"), QStringLiteral("Lung 16")},
            {QStringLiteral("urgenta2"), QStringLiteral("Urgență 2")},
            {QStringLiteral("fade_curat"), QStringLiteral("Fade curat")},
    };
    return kNames.value(recipeHint, recipeHint);
}

// static
QString DJAppSuggestions::termName(const QString& term) {
    static const QHash<QString, QString> kNames = {
            {QStringLiteral("key"), QStringLiteral("tonalitate")},
            {QStringLiteral("intro"), QStringLiteral("intro")},
            {QStringLiteral("outro"), QStringLiteral("outro")},
            {QStringLiteral("energy"), QStringLiteral("energie")},
            {QStringLiteral("play"), QStringLiteral("ascultat")},
            {QStringLiteral("tempo"), QStringLiteral("tempo")},
            {QStringLiteral("vibe"), QStringLiteral("vibe")},
    };
    return kNames.value(term, term);
}

// static
QString DJAppSuggestions::scoreText(double score) {
    // Dan's notation: ",94" (1,00 stays whole).
    const QString text = romanian().toString(score, 'f', 2);
    if (text.startsWith(QLatin1String("0,"))) {
        return text.mid(1);
    }
    if (text.startsWith(QLatin1String("-0,"))) {
        return QStringLiteral("−") + text.mid(2);
    }
    return text;
}

// static
QString DJAppSuggestions::scoreBar(double score) {
    static const QString kEighths[] = {QString(),
            QStringLiteral("▏"),
            QStringLiteral("▎"),
            QStringLiteral("▍"),
            QStringLiteral("▌"),
            QStringLiteral("▋"),
            QStringLiteral("▊"),
            QStringLiteral("▉")};
    const int eighths = static_cast<int>(std::lround(std::clamp(score, 0.0, 1.0) * 64.0));
    return QString(eighths / 8, QChar(0x2588)) + kEighths[eighths % 8];
}

// static
QString DJAppSuggestions::stepText(double stepBpm) {
    if (std::abs(stepBpm) < 0.05) {
        return QStringLiteral("±0");
    }
    const QString magnitude = romanian().toString(std::abs(stepBpm), 'f', 1);
    return (stepBpm > 0 ? QStringLiteral("+") : QStringLiteral("−")) + magnitude;
}

// static
QString DJAppSuggestions::keyText(const QString& camelotA, const DJAppSuggestion& suggestion) {
    const QString a = camelotA.isEmpty() ? QStringLiteral("?") : camelotA;
    const QString b = suggestion.camelot.isEmpty() ? QStringLiteral("?") : suggestion.camelot;
    QString verdict;
    if (camelotA.isEmpty() || suggestion.camelot.isEmpty()) {
        verdict = QStringLiteral("necunoscută");
    } else {
        verdict = suggestion.clash ? QStringLiteral("se bat") : QStringLiteral("ok");
    }
    QString text = QStringLiteral("%1→%2 %3").arg(a, b, verdict);
    if (suggestion.keyGuard) {
        text += QStringLiteral(" · GARDĂ TON");
    }
    return text;
}

// static
QString DJAppSuggestions::shortReason(const DJAppSuggestion& suggestion) {
    if (!suggestion.flags.isEmpty()) {
        return suggestion.flags.mid(0, 2).join(QStringLiteral(" · "));
    }
    QStringList parts;
    const QStringList intro = suggestion.introNote.split(QStringLiteral(", "), Qt::SkipEmptyParts);
    for (const QString& part : intro) {
        // "groove 0" alone says little; "tobe bara 0" is what Dan listens for.
        if (part.startsWith(QLatin1String("tobe")) || part.startsWith(QLatin1String("fara tobe"))) {
            parts << part;
            break;
        }
    }
    const QStringList outro = suggestion.outroNote.split(QStringLiteral(", "), Qt::SkipEmptyParts);
    if (!outro.isEmpty()) {
        parts << outro.first();
    }
    if (parts.isEmpty() && !intro.isEmpty()) {
        parts << intro.first();
    }
    return parts.join(QStringLiteral(" · "));
}

// static
QStringList DJAppSuggestions::whyLines(const QString& camelotA, const DJAppSuggestion& suggestion) {
    QStringList lines;
    lines << QStringLiteral("Tonalitate %1 · tempo %2 BPM · rețetă %3%4")
                     .arg(keyText(camelotA, suggestion),
                             stepText(suggestion.stepBpm),
                             recipeText(suggestion.recipeHint),
                             suggestion.allowed ? QString()
                                                : QStringLiteral(" · nu trece regulile stricte"));
    if (!suggestion.introNote.isEmpty()) {
        lines << QStringLiteral("Intrarea (piesa nouă): ") + suggestion.introNote;
    }
    if (!suggestion.outroNote.isEmpty()) {
        lines << QStringLiteral("Ieșirea (piesa veche): ") + suggestion.outroNote;
    }
    if (!suggestion.flags.isEmpty()) {
        lines << QStringLiteral("Semnale: ") + suggestion.flags.join(QStringLiteral(" · "));
    }
    QStringList terms;
    for (const DJAppSuggestionTerm& term : suggestion.terms) {
        terms << QStringLiteral("%1 %2").arg(termName(term.name), scoreText(term.value));
    }
    lines << QStringLiteral("Scor %1 = %2")
                     .arg(scoreText(suggestion.score), terms.join(QStringLiteral(" · ")));
    return lines;
}

// static
QDateTime DJAppSuggestions::djDayStart(const QDateTime& nowLocal) {
    const QTime kDayStart(6, 0);
    QDate date = nowLocal.date();
    if (nowLocal.time() < kDayStart) {
        date = date.addDays(-1);
    }
    return QDateTime(date, kDayStart, nowLocal.timeZone());
}

// static
QStringList DJAppSuggestions::readPlayedSince(
        const QSqlDatabase& mixxxDb, const QDateTime& sinceUtc, QString* pError) {
    // pl_datetime_added and last_played_at are SQLite CURRENT_TIMESTAMP (UTC),
    // so a string comparison is a time comparison.
    const QString since = sinceUtc.toUTC().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"));
    // History playlists (hidden = 2, PlaylistDAO::PLHT_SET_LOG) get a track
    // the moment it counts as played; last_played_at catches anything else.
    const QString history = QStringLiteral(
            "SELECT tl.location FROM PlaylistTracks pt "
            "JOIN Playlists p ON p.id = pt.playlist_id "
            "JOIN library l ON l.id = pt.track_id "
            "JOIN track_locations tl ON tl.id = l.location "
            "WHERE p.hidden = 2 AND pt.pl_datetime_added >= ?");
    const QString lastPlayed = QStringLiteral(
            "SELECT tl.location FROM library l "
            "JOIN track_locations tl ON tl.id = l.location "
            "WHERE l.last_played_at >= ?");
    QSqlQuery query(mixxxDb);
    query.setForwardOnly(true);
    query.prepare(history + QStringLiteral(" UNION ") + lastPlayed);
    query.addBindValue(since);
    query.addBindValue(since);
    bool ok = query.exec();
    if (!ok) {
        query.prepare(history);
        query.addBindValue(since);
        ok = query.exec();
    }
    QStringList locations;
    if (!ok) {
        if (pError) {
            *pError = query.lastError().text();
        }
        return locations;
    }
    while (query.next()) {
        locations.append(query.value(0).toString());
    }
    return locations;
}

// static
QString DJAppSuggestions::chooseFreeDeck(
        const QList<DJAppDeckState>& decks, const QString& sourceGroup) {
    for (const DJAppDeckState& deck : decks) {
        if (!deck.playing && !deck.loaded) {
            return deck.group;
        }
    }
    for (const DJAppDeckState& deck : decks) {
        if (!deck.playing && deck.group != sourceGroup) {
            return deck.group;
        }
    }
    return QString();
}

// static
QByteArray DJAppSuggestions::feedbackJson(const QString& sourceLocation,
        const DJAppSuggestion& suggestion,
        const QString& outcome,
        int position) {
    QJsonObject object;
    object.insert(QStringLiteral("source"), sourceLocation);
    object.insert(QStringLiteral("suggestion"), suggestion.location);
    object.insert(QStringLiteral("outcome"), outcome);
    object.insert(QStringLiteral("position"), position);
    object.insert(QStringLiteral("score"), suggestion.score);
    object.insert(QStringLiteral("allowed"), suggestion.allowed);
    object.insert(QStringLiteral("recipe"), suggestion.recipeHint);
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}
