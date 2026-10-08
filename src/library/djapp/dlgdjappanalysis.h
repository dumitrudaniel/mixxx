#pragma once

#include <QAbstractTableModel>
#include <QFutureWatcher>
#include <QList>
#include <QWidget>

#include "library/djapp/braindbreader.h"
#include "library/djapp/djappanalysis.h"
#include "library/libraryview.h"
#include "preferences/usersettings.h"
#include "track/track_decl.h"
#include "util/db/dbconnectionpool.h"

class Library;
class QLabel;
class QPushButton;
class QSortFilterProxyModel;
class QTableView;
class WLibrary;

// One brain.db read + Mixxx library list, done on a worker thread.
struct DJAppAnalysisResult {
    BrainSnapshot brain;
    QList<DJAppAnalysisRow> rows;
    DJAppAnalysisSummary summary;
    QString libraryError;
};

class DJAppAnalysisTableModel : public QAbstractTableModel {
    Q_OBJECT
  public:
    enum Column {
        kTrack,
        kMixxx,
        kBrain,
        kStems,
        kVoice,
        kGrid,
        kMixPoints,
        kIndex,
        kColumnCount
    };
    static constexpr int kTrackIdRole = Qt::UserRole + 1;

    explicit DJAppAnalysisTableModel(QObject* pParent);

    void setRows(QList<DJAppAnalysisRow> rows);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section,
            Qt::Orientation orientation,
            int role = Qt::DisplayRole) const override;

  private:
    QList<DJAppAnalysisRow> m_rows;
};

// "Analiză": what brain knows about each library track (plan-ui-integrare §5.4).
// Read only; the commands (analizează acum, pauză) come with the brain service.
class DlgDJAppAnalysis : public QWidget, public virtual LibraryView {
    Q_OBJECT
  public:
    DlgDJAppAnalysis(WLibrary* pParent, UserSettingsPointer pConfig, Library* pLibrary);
    ~DlgDJAppAnalysis() override;

    void onShow() override;
    bool hasFocus() const override;
    void setFocus() override;
    void onSearch(const QString& text) override;

    QString currentSearch() const {
        return m_search;
    }

  public slots:
    void refresh();

  signals:
    // Tracks still waiting for brain (sidebar "Analiză N"); -1 = unknown.
    void waitingCountChanged(int waiting);
    void loadTrack(TrackPointer pTrack);

  private slots:
    void slotReadFinished();
    void slotActivated(const QModelIndex& index);

  private:
    static DJAppAnalysisResult readInBackground(
            const QString& brainDbPath, mixxx::DbConnectionPoolPtr pDbConnectionPool);

    UserSettingsPointer m_pConfig;
    Library* m_pLibrary;
    QLabel* m_pStatusLabel;
    QLabel* m_pSummaryLabel;
    QPushButton* m_pRefreshButton;
    QTableView* m_pTableView;
    DJAppAnalysisTableModel* m_pModel;
    QSortFilterProxyModel* m_pProxyModel;
    QFutureWatcher<DJAppAnalysisResult> m_watcher;
    QString m_search;
    bool m_bLoadedOnce;
};
