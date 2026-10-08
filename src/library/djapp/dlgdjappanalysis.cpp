#include "library/djapp/dlgdjappanalysis.h"

#include <QColor>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <QtDebug>

#include "library/djapp/djappui.h"
#include "library/library.h"
#include "library/trackcollectionmanager.h"
#include "moc_dlgdjappanalysis.cpp"
#include "track/track.h"
#include "util/db/dbconnectionpooled.h"
#include "util/db/dbconnectionpooler.h"
#include "widget/wlibrary.h"

namespace {

const QString kConfigGroup = QStringLiteral("[DJApp]");

const QColor kColorError(0xff, 0x6b, 0x6b);
const QColor kColorWarning(0xf0, 0xa8, 0x48);
const QColor kColorWorking(0x7f, 0xc8, 0xf8);
const QColor kColorMuted(0x80, 0x80, 0x80);
const QColor kColorDone(0x8c, 0xd0, 0x8c);

} // namespace

DJAppAnalysisTableModel::DJAppAnalysisTableModel(QObject* pParent)
        : QAbstractTableModel(pParent) {
}

void DJAppAnalysisTableModel::setRows(QList<DJAppAnalysisRow> rows) {
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
}

int DJAppAnalysisTableModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

int DJAppAnalysisTableModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : kColumnCount;
}

QVariant DJAppAnalysisTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= m_rows.size()) {
        return QVariant();
    }
    const DJAppAnalysisRow& row = m_rows.at(index.row());
    if (role == kTrackIdRole) {
        return row.track.id;
    }
    const int column = index.column();
    if (role == Qt::DisplayRole) {
        switch (column) {
        case kTrack:
            return row.trackText;
        case kMixxx:
            return row.mixxxText;
        case kBrain:
            return row.stageText;
        case kStems:
            return row.stemText;
        case kVoice:
            return row.vocalText;
        case kGrid:
            return row.gridText;
        case kMixPoints:
            return row.mixPointsText;
        case kIndex:
            return row.indexText;
        default:
            return QVariant();
        }
    }
    if (role == Qt::ToolTipRole) {
        switch (column) {
        case kTrack:
            return row.track.location;
        case kMixxx:
            return row.track.bpm > 0.0
                    ? QStringLiteral("%1 BPM · %2").arg(row.track.bpm, 0, 'f', 2).arg(row.track.key)
                    : QStringLiteral("Mixxx nu a analizat piesa (BPM, key, grilă)");
        case kBrain:
            return row.stageTooltip;
        case kGrid:
            return row.gridTooltip;
        case kMixPoints:
            return row.mixPointsTooltip;
        default:
            return QVariant();
        }
    }
    if (role == Qt::ForegroundRole) {
        switch (column) {
        case kMixxx:
            return row.mixxxReady ? QVariant() : QVariant(kColorWarning);
        case kBrain:
            switch (row.stage) {
            case DJAppBrainStage::Done:
                return kColorDone;
            case DJAppBrainStage::Error:
                return kColorError;
            case DJAppBrainStage::Queued:
            case DJAppBrainStage::Working:
                return kColorWorking;
            case DJAppBrainStage::NotInBrain:
                return kColorMuted;
            }
            return QVariant();
        case kGrid:
            if (row.variableTempo) {
                return kColorWarning;
            }
            return row.gridToApply ? QVariant(kColorWorking) : QVariant();
        default:
            return QVariant();
        }
    }
    return QVariant();
}

QVariant DJAppAnalysisTableModel::headerData(
        int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal) {
        return QVariant();
    }
    if (role == Qt::DisplayRole) {
        switch (section) {
        case kTrack:
            return QStringLiteral("Piesa");
        case kMixxx:
            return QStringLiteral("Mixxx");
        case kBrain:
            return QStringLiteral("Brain");
        case kStems:
            return QStringLiteral("Stems");
        case kVoice:
            return QStringLiteral("Voce");
        case kGrid:
            return QStringLiteral("Grilă");
        case kMixPoints:
            return QStringLiteral("Puncte mix");
        case kIndex:
            return QStringLiteral("Index");
        default:
            return QVariant();
        }
    }
    if (role == Qt::ToolTipRole) {
        switch (section) {
        case kMixxx:
            return QStringLiteral("BPM și tonalitate din analiza Mixxx");
        case kBrain:
            return QStringLiteral(
                    "Etapa analizei brain (analysis_queue / tracks; "
                    "analysis_progress când rulează serviciul)");
        case kStems:
            return QStringLiteral("Exportul .stem.mp4 (stem_exports)");
        case kVoice:
            return QStringLiteral("Harta vocii (vocal_maps)");
        case kGrid:
            return QStringLiteral("Corecția de grilă (grid_corrections, ADR 0019/0025)");
        case kMixPoints:
            return QStringLiteral("Punctele de tranziție (mix_points, ADR 0021/0023)");
        case kIndex:
            return QStringLiteral("Starea indexului (track_index, ADR 0026)");
        default:
            return QVariant();
        }
    }
    return QVariant();
}

DlgDJAppAnalysis::DlgDJAppAnalysis(
        WLibrary* pParent, UserSettingsPointer pConfig, Library* pLibrary)
        : QWidget(pParent),
          m_pConfig(pConfig),
          m_pLibrary(pLibrary),
          m_pStatusLabel(nullptr),
          m_pSummaryLabel(nullptr),
          m_pRefreshButton(nullptr),
          m_pTableView(nullptr),
          m_pModel(new DJAppAnalysisTableModel(this)),
          m_pProxyModel(new QSortFilterProxyModel(this)),
          m_bLoadedOnce(false) {
    djappui::setupView(this);

    auto* pLayout = new QVBoxLayout(this);
    pLayout->setContentsMargins(8, 6, 8, 6);
    pLayout->setSpacing(6);

    auto* pHeader = new QHBoxLayout();
    pHeader->addWidget(djappui::newTitle(QStringLiteral("Analiză"), this));
    m_pStatusLabel = djappui::newMutedLabel(QStringLiteral("brain.db: necitit"), this);
    pHeader->addWidget(m_pStatusLabel, 1);
    m_pRefreshButton = new QPushButton(QStringLiteral("Reîmprospătează"), this);
    m_pRefreshButton->setToolTip(QStringLiteral(
            "Recitește brain.db (doar citire) și lista pieselor din Mixxx"));
    pHeader->addWidget(m_pRefreshButton);
    pLayout->addLayout(pHeader);

    m_pSummaryLabel = new QLabel(QStringLiteral("—"), this);
    m_pSummaryLabel->setObjectName(QStringLiteral("DJAppSummary"));
    pLayout->addWidget(m_pSummaryLabel);

    m_pProxyModel->setSourceModel(m_pModel);
    m_pProxyModel->setFilterKeyColumn(-1);
    m_pProxyModel->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_pProxyModel->setSortCaseSensitivity(Qt::CaseInsensitive);
    m_pProxyModel->setSortLocaleAware(true);

    m_pTableView = new QTableView(this);
    m_pTableView->setModel(m_pProxyModel);
    m_pTableView->setSortingEnabled(true);
    m_pTableView->sortByColumn(DJAppAnalysisTableModel::kTrack, Qt::AscendingOrder);
    m_pTableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pTableView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_pTableView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pTableView->setAlternatingRowColors(true);
    m_pTableView->setWordWrap(false);
    m_pTableView->verticalHeader()->hide();
    m_pTableView->verticalHeader()->setDefaultSectionSize(22);
    m_pTableView->horizontalHeader()->setStretchLastSection(false);
    m_pTableView->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_pTableView->horizontalHeader()->setSectionResizeMode(
            DJAppAnalysisTableModel::kTrack, QHeaderView::Stretch);
    m_pTableView->horizontalHeader()->setSectionResizeMode(
            DJAppAnalysisTableModel::kGrid, QHeaderView::Stretch);
    m_pTableView->setToolTip(QString());
    pLayout->addWidget(m_pTableView, 1);

    pLayout->addWidget(djappui::newMutedLabel(
            QStringLiteral(
                    "Doar citire: Mixxx nu scrie niciodată în brain.db. Dublu-click pe o "
                    "piesă o încarcă pe un deck oprit. „Analizează acum”, „Pauză” și "
                    "progresul live vin cu serviciul brain (Etapa 2)."),
            this));

    connect(m_pRefreshButton, &QPushButton::clicked, this, &DlgDJAppAnalysis::refresh);
    connect(&m_watcher,
            &QFutureWatcher<DJAppAnalysisResult>::finished,
            this,
            &DlgDJAppAnalysis::slotReadFinished);
    connect(m_pTableView, &QTableView::doubleClicked, this, &DlgDJAppAnalysis::slotActivated);
}

DlgDJAppAnalysis::~DlgDJAppAnalysis() {
    // The worker owns copies of everything it uses; just stop listening.
    m_watcher.disconnect(this);
}

void DlgDJAppAnalysis::onShow() {
    // Every time the view is opened: a fresh, cheap, background read.
    refresh();
}

bool DlgDJAppAnalysis::hasFocus() const {
    return m_pTableView->hasFocus();
}

void DlgDJAppAnalysis::setFocus() {
    m_pTableView->setFocus();
}

void DlgDJAppAnalysis::onSearch(const QString& text) {
    m_search = text;
    m_pProxyModel->setFilterFixedString(text);
}

void DlgDJAppAnalysis::refresh() {
    if (m_watcher.isRunning()) {
        return; // one read at a time; the running one is fresh enough
    }
    m_pRefreshButton->setEnabled(false);
    m_pStatusLabel->setText(QStringLiteral("brain.db: se citește…"));
    const QString brainDbPath = BrainDbReader::configuredPath(
            m_pConfig->getValueString(ConfigKey(kConfigGroup, QStringLiteral("BrainDb"))));
    m_watcher.setFuture(QtConcurrent::run(&DlgDJAppAnalysis::readInBackground,
            brainDbPath,
            m_pLibrary->dbConnectionPool()));
}

// static
DJAppAnalysisResult DlgDJAppAnalysis::readInBackground(
        const QString& brainDbPath, mixxx::DbConnectionPoolPtr pDbConnectionPool) {
    DJAppAnalysisResult result;
    {
        // Mixxx's own library, on a thread-local pooled connection (SELECT only),
        // like the external library features do.
        const mixxx::DbConnectionPooler dbConnectionPooler(pDbConnectionPool);
        const QSqlDatabase mixxxDb = mixxx::DbConnectionPooled(pDbConnectionPool);
        QList<DJAppLibraryTrack> tracks;
        if (mixxxDb.isOpen()) {
            tracks = DJAppAnalysis::readLibraryTracks(mixxxDb, &result.libraryError);
        } else {
            result.libraryError = QStringLiteral("biblioteca Mixxx nu s-a putut deschide");
        }
        result.brain = BrainDbReader::read(brainDbPath);
        result.rows = DJAppAnalysis::buildRows(tracks, result.brain);
    }
    result.summary = DJAppAnalysis::summarize(result.rows);
    return result;
}

void DlgDJAppAnalysis::slotReadFinished() {
    const DJAppAnalysisResult result = m_watcher.result();
    m_pRefreshButton->setEnabled(true);
    m_bLoadedOnce = true;

    QString status = DJAppAnalysis::snapshotText(result.brain);
    if (!result.libraryError.isEmpty()) {
        status += QStringLiteral(" · biblioteca Mixxx: ") + result.libraryError;
    }
    m_pStatusLabel->setText(status);
    m_pStatusLabel->setToolTip(result.brain.dbPath);
    m_pSummaryLabel->setText(DJAppAnalysis::summaryText(result.summary));

    m_pModel->setRows(result.rows);
    m_pTableView->resizeColumnToContents(DJAppAnalysisTableModel::kMixxx);
    m_pTableView->resizeColumnToContents(DJAppAnalysisTableModel::kBrain);
    m_pTableView->resizeColumnToContents(DJAppAnalysisTableModel::kStems);
    m_pTableView->resizeColumnToContents(DJAppAnalysisTableModel::kVoice);
    m_pTableView->resizeColumnToContents(DJAppAnalysisTableModel::kMixPoints);
    m_pTableView->resizeColumnToContents(DJAppAnalysisTableModel::kIndex);

    emit waitingCountChanged(result.brain.ok ? result.summary.waiting() : -1);
}

void DlgDJAppAnalysis::slotActivated(const QModelIndex& index) {
    const QVariant id = index.data(DJAppAnalysisTableModel::kTrackIdRole);
    if (!id.isValid()) {
        return;
    }
    TrackPointer pTrack = m_pLibrary->trackCollectionManager()->getTrackById(TrackId(id));
    if (pTrack) {
        emit loadTrack(pTrack);
    }
}
