#include "library/djapp/dlgdjappsuggestions.h"

#include <QCheckBox>
#include <QColor>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QScrollArea>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <QtDebug>

#include "control/controlobject.h"
#include "library/dao/playlistdao.h"
#include "library/dao/trackschema.h"
#include "library/djapp/djappui.h"
#include "library/library.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "moc_dlgdjappsuggestions.cpp"
#include "track/track.h"
#include "util/db/dbconnectionpooled.h"
#include "util/db/dbconnectionpooler.h"
#include "widget/wlibrary.h"

namespace {

const QString kConfigGroup = QStringLiteral("[DJApp]");
constexpr int kDebounceMs = 150;
constexpr int kAllowedLimit = 10;
constexpr int kRiskyLimit = 6;

const QColor kColorError(0xff, 0x6b, 0x6b);
const QColor kColorWarning(0xf0, 0xa8, 0x48);
const QColor kColorMuted(0x80, 0x80, 0x80);

QString bpmNumber(double bpm) {
    static const QLocale kRomanian(QLocale::Romanian, QLocale::Romania);
    return kRomanian.toString(bpm, 'f', 1);
}

QString deckLabel(const QString& group) {
    int number = -1;
    // PlayerManager::isDeckGroup returns the number straight out of the
    // group name ("[Channel1]" -> 1), already what Dan calls "deck 1".
    if (PlayerManager::isDeckGroup(group, &number)) {
        return QStringLiteral("deck %1").arg(number);
    }
    return group;
}

QString trackText(const DJAppSuggestion& suggestion) {
    if (!suggestion.artist.isEmpty() && !suggestion.title.isEmpty()) {
        return suggestion.artist + QStringLiteral(" – ") + suggestion.title;
    }
    if (!suggestion.title.isEmpty()) {
        return suggestion.title;
    }
    const QString location =
            suggestion.poolLocation.isEmpty() ? suggestion.location : suggestion.poolLocation;
    const int slash = std::max(location.lastIndexOf('/'), location.lastIndexOf('\\'));
    return slash >= 0 ? location.mid(slash + 1) : location;
}

} // namespace

// ---------------------------------------------------------------- table model

DJAppSuggestionsTableModel::DJAppSuggestionsTableModel(QObject* pParent)
        : QAbstractTableModel(pParent) {
}

void DJAppSuggestionsTableModel::setRows(QList<DJAppSuggestion> rows, const QString& sourceCamelot) {
    beginResetModel();
    m_rows = std::move(rows);
    m_sourceCamelot = sourceCamelot;
    endResetModel();
}

const DJAppSuggestion* DJAppSuggestionsTableModel::suggestionAt(int row) const {
    return (row >= 0 && row < m_rows.size()) ? &m_rows.at(row) : nullptr;
}

int DJAppSuggestionsTableModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

int DJAppSuggestionsTableModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : kColumnCount;
}

QVariant DJAppSuggestionsTableModel::data(const QModelIndex& index, int role) const {
    const DJAppSuggestion* pRow = suggestionAt(index.row());
    if (!pRow) {
        return QVariant();
    }
    const DJAppSuggestion& s = *pRow;
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case kScore:
            return QString(DJAppSuggestions::scoreBar(s.score) + QStringLiteral(" ") +
                    DJAppSuggestions::scoreText(s.score));
        case kTrack:
            return trackText(s);
        case kBpm:
            return s.bpm ? QStringLiteral("%1 (%2)").arg(
                                   bpmNumber(*s.bpm), DJAppSuggestions::stepText(s.stepBpm))
                         : QStringLiteral("—");
        case kKey:
            return DJAppSuggestions::keyText(m_sourceCamelot, s);
        case kRecipe:
            return DJAppSuggestions::recipeText(s.recipeHint);
        case kReason: {
            const QString reason = DJAppSuggestions::shortReason(s);
            return reason.isEmpty() ? QStringLiteral("—") : reason;
        }
        default:
            return QVariant();
        }
    }
    if (role == Qt::ToolTipRole) {
        return DJAppSuggestions::whyLines(m_sourceCamelot, s).join(QStringLiteral("\n"));
    }
    if (role == Qt::ForegroundRole) {
        if (index.column() == kKey && s.clash) {
            return kColorWarning;
        }
        if (index.column() == kTrack && s.mixxxTrackId < 0) {
            return kColorMuted; // not in the current Mixxx library
        }
    }
    return QVariant();
}

QVariant DJAppSuggestionsTableModel::headerData(
        int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return QVariant();
    }
    switch (section) {
    case kScore:
        return QStringLiteral("Scor");
    case kTrack:
        return QStringLiteral("Piesa");
    case kBpm:
        return QStringLiteral("BPM");
    case kKey:
        return QStringLiteral("Key");
    case kRecipe:
        return QStringLiteral("Rețetă");
    case kReason:
        return QStringLiteral("De ce (scurt)");
    default:
        return QVariant();
    }
}

// ---------------------------------------------------------------- view

namespace {

QTableView* newSuggestionTable(DJAppSuggestionsTableModel* pModel, QWidget* pParent) {
    auto* pView = new QTableView(pParent);
    pView->setModel(pModel);
    pView->setSelectionBehavior(QAbstractItemView::SelectRows);
    pView->setSelectionMode(QAbstractItemView::SingleSelection);
    pView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    pView->setAlternatingRowColors(true);
    pView->setWordWrap(false);
    pView->verticalHeader()->hide();
    // Row height from the actual font metrics, not a hardcoded pixel guess: a fixed value
    // (22) overlapped text on a laptop whose theme/DPI rendered the library font larger.
    pView->verticalHeader()->setMinimumSectionSize(pView->fontMetrics().height() + 6);
    pView->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    pView->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    pView->horizontalHeader()->setSectionResizeMode(
            DJAppSuggestionsTableModel::kTrack, QHeaderView::Stretch);
    pView->setMaximumHeight(200);
    return pView;
}

} // namespace

DlgDJAppSuggestions::DlgDJAppSuggestions(
        WLibrary* pParent, UserSettingsPointer pConfig, Library* pLibrary)
        : QWidget(pParent),
          m_pConfig(pConfig),
          m_pLibrary(pLibrary),
          m_pAllowedModel(new DJAppSuggestionsTableModel(this)),
          m_pRiskyModel(new DJAppSuggestionsTableModel(this)) {
    djappui::setupView(this);

    // Laptop-screen fix (Dan reported this view "unusable due to poorly
    // sized space" on the DJ laptop, 1366x768-1600x900 class): the content
    // below (2 tables + header/filter/labels/2 rows of action buttons)
    // easily exceeds a short library panel's available height. Previously
    // this content sat directly in `this`'s layout with no way to shrink
    // further than its widgets' combined minimum size -- on a short panel
    // it simply got clipped, not scrolled. A QScrollArea makes the overflow
    // (if any) scrollable instead of invisible, on any screen size, while
    // changing nothing about the content itself on a tall-enough screen
    // (QScrollArea is otherwise invisible: no border, resizable widget).
    auto* pOuterLayout = new QVBoxLayout(this);
    pOuterLayout->setContentsMargins(0, 0, 0, 0);
    pOuterLayout->setSpacing(0);

    auto* pScrollArea = new QScrollArea(this);
    pScrollArea->setWidgetResizable(true);
    pScrollArea->setFrameShape(QFrame::NoFrame);
    pScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    pOuterLayout->addWidget(pScrollArea);

    auto* pContent = new QWidget(pScrollArea);
    pScrollArea->setWidget(pContent);

    auto* pLayout = new QVBoxLayout(pContent);
    pLayout->setContentsMargins(8, 6, 8, 6);
    pLayout->setSpacing(6);

    auto* pHeader = new QHBoxLayout();
    m_pSourceLabel = djappui::newTitle(QStringLiteral("Sugestii după: —"), pContent);
    pHeader->addWidget(m_pSourceLabel, 1);
    m_pRefreshButton = new QPushButton(QStringLiteral("Reîmprospătează"), pContent);
    connect(m_pRefreshButton, &QPushButton::clicked, this, &DlgDJAppSuggestions::refresh);
    pHeader->addWidget(m_pRefreshButton);
    pLayout->addLayout(pHeader);

    m_pStatusLabel = djappui::newMutedLabel(QString(), pContent);
    pLayout->addWidget(m_pStatusLabel);

    m_pIndexBanner = djappui::newWorkInProgressBanner(QString(), pContent);
    m_pIndexBanner->hide();
    pLayout->addWidget(m_pIndexBanner);

    auto* pFilterRow = new QHBoxLayout();
    m_pOnlyMatchingKeysCheck =
            new QCheckBox(QStringLiteral("doar tonalități potrivite"), pContent);
    connect(m_pOnlyMatchingKeysCheck,
            &QCheckBox::toggled,
            this,
            &DlgDJAppSuggestions::slotOnlyMatchingKeysToggled);
    pFilterRow->addWidget(m_pOnlyMatchingKeysCheck);
    pFilterRow->addStretch(1);
    pLayout->addLayout(pFilterRow);

    m_pAllowedView = newSuggestionTable(m_pAllowedModel, pContent);
    pLayout->addWidget(m_pAllowedView, 3);

    m_pRiskyLabel = djappui::newMutedLabel(
            QStringLiteral("── riscante (tonalitate) ──────────────────────"), pContent);
    pLayout->addWidget(m_pRiskyLabel);
    m_pRiskyView = newSuggestionTable(m_pRiskyModel, pContent);
    pLayout->addWidget(m_pRiskyView, 2);

    m_pWhyLabel = djappui::newMutedLabel(QString(), pContent);
    m_pWhyLabel->hide();
    pLayout->addWidget(m_pWhyLabel);

    // Two rows instead of one (was a single QHBoxLayout with all 5 buttons
    // plus a trailing stretch): at a modest laptop width, 5 Romanian-length
    // button labels in one row ("Încarcă pe deck-ul liber", "Adaugă după
    // curentă", ...) summed to more than the panel's available width, and a
    // QHBoxLayout never wraps -- the row (and everything sized to match it)
    // got clipped. Splitting into two rows of up to 3 buttons each keeps
    // every button at a readable, un-clipped size on a 1366px-wide screen.
    auto* pActionsRow1 = new QHBoxLayout();
    m_pLoadButton = new QPushButton(QStringLiteral("Încarcă pe deck-ul liber"), pContent);
    m_pPreviewButton = new QPushButton(QStringLiteral("Preascultă"), pContent);
    m_pAddButton = new QPushButton(QStringLiteral("Adaugă după curentă"), pContent);
    for (QPushButton* pButton : {m_pLoadButton, m_pPreviewButton, m_pAddButton}) {
        pButton->setEnabled(false);
        pActionsRow1->addWidget(pButton);
    }
    pActionsRow1->addStretch(1);
    pLayout->addLayout(pActionsRow1);

    auto* pActionsRow2 = new QHBoxLayout();
    m_pNotNowButton = new QPushButton(QStringLiteral("Nu acum"), pContent);
    m_pWhyButton = new QPushButton(QStringLiteral("De ce?"), pContent);
    for (QPushButton* pButton : {m_pNotNowButton, m_pWhyButton}) {
        pButton->setEnabled(false);
        pActionsRow2->addWidget(pButton);
    }
    pActionsRow2->addStretch(1);
    pLayout->addLayout(pActionsRow2);

    connect(m_pLoadButton, &QPushButton::clicked, this, &DlgDJAppSuggestions::slotLoadToFreeDeck);
    connect(m_pPreviewButton, &QPushButton::clicked, this, &DlgDJAppSuggestions::slotPreview);
    connect(m_pAddButton, &QPushButton::clicked, this, &DlgDJAppSuggestions::slotAddToQueue);
    connect(m_pNotNowButton, &QPushButton::clicked, this, &DlgDJAppSuggestions::slotNotNow);
    connect(m_pWhyButton, &QPushButton::clicked, this, &DlgDJAppSuggestions::slotToggleWhy);

    const auto onSelection = [this](DJAppSuggestionsTableModel* pModel, QTableView* pOtherView) {
        return [this, pModel, pOtherView](const QItemSelection& selected) {
            const int row = selected.indexes().isEmpty() ? -1 : selected.indexes().first().row();
            if (row < 0) {
                return;
            }
            const QSignalBlocker blocker(pOtherView->selectionModel());
            pOtherView->clearSelection();
            selectRow(pModel, row);
        };
    };
    connect(m_pAllowedView->selectionModel(),
            &QItemSelectionModel::selectionChanged,
            this,
            [this, onSelection](const QItemSelection& selected, const QItemSelection&) {
                onSelection(m_pAllowedModel, m_pRiskyView)(selected);
            });
    connect(m_pRiskyView->selectionModel(),
            &QItemSelectionModel::selectionChanged,
            this,
            [this, onSelection](const QItemSelection& selected, const QItemSelection&) {
                onSelection(m_pRiskyModel, m_pAllowedView)(selected);
            });

    m_pDebounceTimer = new QTimer(this);
    m_pDebounceTimer->setSingleShot(true);
    connect(m_pDebounceTimer, &QTimer::timeout, this, &DlgDJAppSuggestions::refresh);

    connect(&PlayerInfo::instance(),
            &PlayerInfo::trackChanged,
            this,
            &DlgDJAppSuggestions::slotTrackChanged);
    connect(&PlayerInfo::instance(),
            &PlayerInfo::currentPlayingTrackChanged,
            this,
            &DlgDJAppSuggestions::slotCurrentPlayingTrackChanged);

    // Initial deck state (a track may already be loaded before this view exists).
    const QMap<QString, TrackPointer> loaded = PlayerInfo::instance().getLoadedTracks();
    for (auto it = loaded.constBegin(); it != loaded.constEnd(); ++it) {
        if (PlayerManager::isDeckGroup(it.key()) && it.value()) {
            m_deckLocations.insert(it.key(), it.value()->getLocation());
            m_deckTracks.insert(it.key(), it.value());
            m_deckLoadOrder.insert(it.key(), ++m_loadCounter);
        }
    }

    connect(&m_watcher,
            &QFutureWatcher<DJAppSuggestionsQueryResult>::finished,
            this,
            &DlgDJAppSuggestions::slotReadFinished);

    updateSourceLabel();
}

DlgDJAppSuggestions::~DlgDJAppSuggestions() {
    m_watcher.disconnect(this);
}

void DlgDJAppSuggestions::onShow() {
    refresh();
}

bool DlgDJAppSuggestions::hasFocus() const {
    return m_pAllowedView->hasFocus() || m_pRiskyView->hasFocus();
}

void DlgDJAppSuggestions::setFocus() {
    m_pAllowedView->setFocus();
}

void DlgDJAppSuggestions::slotTrackChanged(
        const QString& group, TrackPointer pNewTrack, TrackPointer pOldTrack) {
    Q_UNUSED(pOldTrack);
    if (!PlayerManager::isDeckGroup(group)) {
        return;
    }
    if (pNewTrack) {
        m_deckLocations.insert(group, pNewTrack->getLocation());
        m_deckTracks.insert(group, pNewTrack);
        m_deckLoadOrder.insert(group, ++m_loadCounter);
    } else {
        m_deckLocations.remove(group);
        m_deckTracks.remove(group);
        m_deckLoadOrder.remove(group);
    }
    m_pDebounceTimer->start(kDebounceMs);
}

void DlgDJAppSuggestions::slotCurrentPlayingTrackChanged(TrackPointer pTrack) {
    Q_UNUSED(pTrack);
    m_pDebounceTimer->start(kDebounceMs);
}

QString DlgDJAppSuggestions::chooseSourceLocation(QString* pDeckGroup) const {
    const int playingDeck = PlayerInfo::instance().getCurrentPlayingDeck();
    if (playingDeck >= 0) {
        const QString group = PlayerManager::groupForDeck(playingDeck);
        const auto it = m_deckLocations.constFind(group);
        if (it != m_deckLocations.constEnd()) {
            *pDeckGroup = group;
            return it.value();
        }
    }
    QString bestGroup;
    qint64 bestOrder = -1;
    for (auto it = m_deckLoadOrder.constBegin(); it != m_deckLoadOrder.constEnd(); ++it) {
        if (it.value() > bestOrder && m_deckLocations.contains(it.key())) {
            bestOrder = it.value();
            bestGroup = it.key();
        }
    }
    *pDeckGroup = bestGroup;
    return bestGroup.isEmpty() ? QString() : m_deckLocations.value(bestGroup);
}

void DlgDJAppSuggestions::updateSourceLabel() {
    if (m_currentSourceLocation.isEmpty()) {
        m_pSourceLabel->setText(QStringLiteral("Sugestii după: — (niciun deck cu piesă)"));
        return;
    }
    QStringList parts;
    const TrackPointer pTrack = m_deckTracks.value(m_currentSourceDeckGroup);
    if (pTrack) {
        parts << pTrack->getInfo();
        if (pTrack->getBpm() > 0.0) {
            parts << bpmNumber(pTrack->getBpm());
        }
        if (!pTrack->getKeyText().isEmpty()) {
            parts << pTrack->getKeyText();
        }
    } else {
        parts << m_currentSourceLocation;
    }
    if (!m_currentSourceDeckGroup.isEmpty()) {
        parts << deckLabel(m_currentSourceDeckGroup);
    }
    m_pSourceLabel->setText(
            QStringLiteral("Sugestii după: ") + parts.join(QStringLiteral(" · ")));
}

void DlgDJAppSuggestions::refresh() {
    QString deckGroup;
    const QString source = chooseSourceLocation(&deckGroup);
    m_currentSourceLocation = source;
    m_currentSourceDeckGroup = deckGroup;
    updateSourceLabel();

    if (source.isEmpty()) {
        m_pAllowedModel->setRows({}, QString());
        m_pRiskyModel->setRows({}, QString());
        m_pStatusLabel->setText(QString());
        m_pIndexBanner->hide();
        for (QPushButton* pButton :
                {m_pLoadButton, m_pPreviewButton, m_pAddButton, m_pNotNowButton, m_pWhyButton}) {
            pButton->setEnabled(false);
        }
        return;
    }
    if (m_watcher.isRunning()) {
        m_pendingRefresh = true;
        return;
    }
    m_pRefreshButton->setEnabled(false);

    QStringList excludeOnDecks = m_deckLocations.values();

    const QString brainDbPath = BrainDbReader::configuredPath(
            m_pConfig->getValueString(ConfigKey(kConfigGroup, QStringLiteral("BrainDb"))));
    m_watcher.setFuture(QtConcurrent::run(&DlgDJAppSuggestions::readInBackground,
            brainDbPath,
            source,
            deckGroup,
            excludeOnDecks,
            m_dismissed.values(),
            m_pLibrary->dbConnectionPool()));
}

// static
DJAppSuggestionsQueryResult DlgDJAppSuggestions::readInBackground(QString brainDbPath,
        QString sourceLocation,
        QString sourceDeckGroup,
        QStringList excludeOnDecks,
        QStringList dismissed,
        mixxx::DbConnectionPoolPtr pDbConnectionPool) {
    DJAppSuggestionsQueryResult result;
    result.sourceLocation = sourceLocation;
    result.sourceDeckGroup = sourceDeckGroup;

    QStringList poolLocations;
    QHash<QString, DJAppLibraryTrack> libraryByKey;
    {
        const mixxx::DbConnectionPooler dbConnectionPooler(pDbConnectionPool);
        const QSqlDatabase mixxxDb = mixxx::DbConnectionPooled(pDbConnectionPool);
        if (!mixxxDb.isOpen()) {
            result.libraryError = QStringLiteral("biblioteca Mixxx nu s-a putut deschide");
        } else {
            const QList<DJAppLibraryTrack> tracks =
                    DJAppAnalysis::readLibraryTracks(mixxxDb, &result.libraryError);
            for (const DJAppLibraryTrack& track : tracks) {
                poolLocations << track.location;
                libraryByKey.insert(BrainDbReader::locationKey(track.location), track);
            }
            const auto sourceIt = libraryByKey.constFind(BrainDbReader::locationKey(sourceLocation));
            if (sourceIt != libraryByKey.constEnd()) {
                result.sourceInLibrary = true;
                result.sourceLibraryTrack = sourceIt.value();
            }

            const QDateTime dayStart = DJAppSuggestions::djDayStart(QDateTime::currentDateTime());
            QString playedError;
            const QStringList playedToday =
                    DJAppSuggestions::readPlayedSince(mixxxDb, dayStart.toUTC(), &playedError);

            DJAppSuggestionRequest request;
            request.location = sourceLocation;
            request.recipe = QString::fromLatin1(DJAppSuggestions::kDefaultRecipe);
            request.limit = kAllowedLimit;
            request.riskyLimit = kRiskyLimit;
            request.restrictToPool = true;
            request.pool = poolLocations;
            request.exclude = playedToday + excludeOnDecks + dismissed;

            result.brain = DJAppSuggestions::query(brainDbPath, request);

            const auto fillDisplay = [&libraryByKey](DJAppSuggestion& s) {
                const QString location = s.poolLocation.isEmpty() ? s.location : s.poolLocation;
                const auto it = libraryByKey.constFind(BrainDbReader::locationKey(location));
                if (it != libraryByKey.constEnd()) {
                    s.mixxxTrackId = it.value().id;
                    s.artist = it.value().artist;
                    s.title = it.value().title;
                }
            };
            for (DJAppSuggestion& s : result.brain.allowed) {
                fillDisplay(s);
            }
            for (DJAppSuggestion& s : result.brain.risky) {
                fillDisplay(s);
            }
        }
    }
    return result;
}

void DlgDJAppSuggestions::slotReadFinished() {
    const DJAppSuggestionsQueryResult result = m_watcher.result();
    m_pRefreshButton->setEnabled(true);

    if (result.sourceLocation != m_currentSourceLocation) {
        // Stale: the source changed again while this query was running.
        if (m_pendingRefresh) {
            m_pendingRefresh = false;
            refresh();
        }
        return;
    }
    m_pendingRefresh = false;
    m_currentSourceCamelot = result.brain.sourceCamelot;

    QStringList status;
    if (!result.libraryError.isEmpty()) {
        status << QStringLiteral("biblioteca Mixxx: ") + result.libraryError;
    }
    if (!result.brain.ok) {
        status << QStringLiteral("brain.db: ") + result.brain.error;
        m_pIndexBanner->setText(QStringLiteral(
                "<b>brain.db nu poate fi citit.</b> ") + result.brain.error.toHtmlEscaped());
        m_pIndexBanner->show();
    } else if (!result.brain.hasIndex) {
        m_pIndexBanner->setText(QStringLiteral(
                "<b>Indexul de perechi nu există încă în brain.db</b> (tabelele "
                "<code>track_index</code> / <code>pair_scores</code>, ADR 0026). Pornește "
                "index-builder, apoi „Reîmprospătează”."));
        m_pIndexBanner->show();
    } else if (!result.brain.sourceIndexed) {
        m_pIndexBanner->setText(QStringLiteral(
                "<b>Piesa asta nu e încă în index.</b> brain o indexează după ce trece prin "
                "analiză (vezi „Analiză” din DJ App)."));
        m_pIndexBanner->show();
    } else {
        m_pIndexBanner->hide();
        status << QStringLiteral("%1 permise · %2 riscante")
                          .arg(result.brain.allowed.size())
                          .arg(result.brain.risky.size());
        if (result.brain.skipped > 0) {
            status << QStringLiteral("%1 excluse (cântate azi / pe deck / „Nu acum”)")
                              .arg(result.brain.skipped);
        }
        status << QStringLiteral("citit în %1 ms").arg(result.brain.queryMs);
    }
    m_pStatusLabel->setText(status.join(QStringLiteral(" · ")));

    m_pAllowedModel->setRows(result.brain.allowed, m_currentSourceCamelot);
    m_pRiskyModel->setRows(result.brain.risky, m_currentSourceCamelot);
    m_pRiskyLabel->setVisible(!m_pOnlyMatchingKeysCheck->isChecked());
    m_pRiskyView->setVisible(!m_pOnlyMatchingKeysCheck->isChecked());
    for (int i = 0; i < DJAppSuggestionsTableModel::kColumnCount; ++i) {
        m_pAllowedView->resizeColumnToContents(i);
        m_pRiskyView->resizeColumnToContents(i);
    }

    m_pSelectedModel = nullptr;
    m_selectedRow = -1;
    for (QPushButton* pButton :
            {m_pLoadButton, m_pPreviewButton, m_pAddButton, m_pNotNowButton, m_pWhyButton}) {
        pButton->setEnabled(false);
    }
    m_pWhyLabel->hide();
}

void DlgDJAppSuggestions::slotOnlyMatchingKeysToggled(bool checked) {
    m_pRiskyLabel->setVisible(!checked);
    m_pRiskyView->setVisible(!checked);
}

void DlgDJAppSuggestions::selectRow(DJAppSuggestionsTableModel* pModel, int row) {
    m_pSelectedModel = pModel;
    m_selectedRow = row;
    const bool haveSelection = pModel->suggestionAt(row) != nullptr;
    for (QPushButton* pButton :
            {m_pLoadButton, m_pPreviewButton, m_pAddButton, m_pNotNowButton, m_pWhyButton}) {
        pButton->setEnabled(haveSelection);
    }
    updateWhyPanel();
}

std::optional<DJAppSuggestion> DlgDJAppSuggestions::selectedSuggestion() const {
    if (!m_pSelectedModel) {
        return std::nullopt;
    }
    const DJAppSuggestion* pSuggestion = m_pSelectedModel->suggestionAt(m_selectedRow);
    return pSuggestion ? std::make_optional(*pSuggestion) : std::nullopt;
}

void DlgDJAppSuggestions::updateWhyPanel() {
    if (!m_pWhyLabel->isVisible()) {
        return;
    }
    const auto suggestion = selectedSuggestion();
    if (!suggestion) {
        m_pWhyLabel->hide();
        return;
    }
    m_pWhyLabel->setText(
            DJAppSuggestions::whyLines(m_currentSourceCamelot, *suggestion).join(QStringLiteral("\n")));
}

void DlgDJAppSuggestions::slotToggleWhy() {
    if (m_pWhyLabel->isVisible()) {
        m_pWhyLabel->hide();
        return;
    }
    updateWhyPanel();
    if (!m_pWhyLabel->text().isEmpty()) {
        m_pWhyLabel->show();
    }
}

void DlgDJAppSuggestions::slotLoadToFreeDeck() {
    const auto suggestion = selectedSuggestion();
    if (!suggestion || suggestion->mixxxTrackId < 0) {
        return;
    }
    TrackPointer pTrack = m_pLibrary->trackCollectionManager()->getTrackById(
            TrackId(QVariant(suggestion->mixxxTrackId)));
    if (!pTrack) {
        return;
    }
    QList<DJAppDeckState> decks;
    const int numDecks = PlayerInfo::instance().numDecks();
    for (int i = 0; i < numDecks; ++i) {
        const QString group = PlayerManager::groupForDeck(i);
        DJAppDeckState deck;
        deck.group = group;
        deck.playing = ControlObject::get(ConfigKey(group, QStringLiteral("play"))) > 0.5;
        deck.loaded = m_deckLocations.contains(group);
        decks.append(deck);
    }
    const QString target =
            DJAppSuggestions::chooseFreeDeck(decks, m_currentSourceDeckGroup);
    if (target.isEmpty()) {
        m_pStatusLabel->setText(QStringLiteral("niciun deck liber"));
        return;
    }
    emit loadTrackToPlayer(pTrack, target, false);
}

void DlgDJAppSuggestions::slotPreview() {
    const auto suggestion = selectedSuggestion();
    if (!suggestion || suggestion->mixxxTrackId < 0) {
        return;
    }
    TrackPointer pTrack = m_pLibrary->trackCollectionManager()->getTrackById(
            TrackId(QVariant(suggestion->mixxxTrackId)));
    if (!pTrack) {
        return;
    }
    emit loadTrackToPlayer(pTrack, PlayerManager::groupForPreviewDeck(0), true);
}

void DlgDJAppSuggestions::slotAddToQueue() {
    const auto suggestion = selectedSuggestion();
    if (!suggestion || suggestion->mixxxTrackId < 0) {
        return;
    }
    // TODO(DJ App, Etapa 5): once AutomixPlaylistDriver and set_plan_items
    // exist, "Adaugă după curentă" should insert into the active automix
    // set rather than the Auto DJ queue. The Auto DJ playlist is a safe,
    // visible placeholder meanwhile (Auto DJ itself is not engaged).
    PlaylistDAO& playlistDao =
            m_pLibrary->trackCollectionManager()->internalCollection()->getPlaylistDAO();
    const int playlistId = playlistDao.getPlaylistIdFromName(QStringLiteral(AUTODJ_TABLE));
    if (playlistId < 0) {
        m_pStatusLabel->setText(QStringLiteral("playlist-ul Auto DJ nu există"));
        return;
    }
    playlistDao.appendTrackToPlaylist(TrackId(QVariant(suggestion->mixxxTrackId)), playlistId);
    m_pStatusLabel->setText(QStringLiteral("adăugată în coada Auto DJ"));
}

void DlgDJAppSuggestions::slotNotNow() {
    const auto suggestion = selectedSuggestion();
    if (!suggestion) {
        return;
    }
    // TODO(DJ App): send POST /feedback (plan §3.7) once BrainClient exists;
    // djappsuggestions.cpp already builds the JSON body
    // (DJAppSuggestions::feedbackJson). Until then this only hides the
    // suggestion for the rest of the session.
    m_dismissed.insert(BrainDbReader::locationKey(suggestion->location));
    refresh();
}
