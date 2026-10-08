#pragma once

#include <QAbstractTableModel>
#include <QFutureWatcher>
#include <QHash>
#include <QList>
#include <QSet>
#include <QWidget>

#include "library/djapp/braindbreader.h"
#include "library/djapp/djappsuggestions.h"
#include "library/libraryview.h"
#include "preferences/usersettings.h"
#include "track/track_decl.h"
#include "util/db/dbconnectionpool.h"

class Library;
class QCheckBox;
class QLabel;
class QPushButton;
class QTableView;
class QTimer;
class WLibrary;

// One brain.db query + Mixxx library/history lookup, done on a worker thread.
struct DJAppSuggestionsQueryResult {
    QString sourceLocation; // requested (deck track's exact Mixxx location)
    bool sourceInLibrary = false;
    DJAppLibraryTrack sourceLibraryTrack;
    QString sourceDeckGroup; // "[Channel1]", or "" if from "most recently loaded"
    DJAppSuggestionResult brain;
    QString libraryError;
};

// Suggestion rows for one table (allowed or risky), with the source's key for
// DJAppSuggestions::keyText / whyLines.
class DJAppSuggestionsTableModel : public QAbstractTableModel {
    Q_OBJECT
  public:
    enum Column {
        kScore,
        kTrack,
        kBpm,
        kKey,
        kRecipe,
        kReason,
        kColumnCount
    };

    explicit DJAppSuggestionsTableModel(QObject* pParent);

    void setRows(QList<DJAppSuggestion> rows, const QString& sourceCamelot);
    const DJAppSuggestion* suggestionAt(int row) const;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section,
            Qt::Orientation orientation,
            int role = Qt::DisplayRole) const override;

  private:
    QList<DJAppSuggestion> m_rows;
    QString m_sourceCamelot;
};

// "Sugestii" (plan-ui-integrare.md §5.1, docs/decisions/0029): top suggestions
// for the track that is playing, or the most recently loaded one. Data comes
// from brain.db's index (index-builder, ADR 0026: track_index + pair_scores),
// read-only, on a worker thread.
class DlgDJAppSuggestions : public QWidget, public virtual LibraryView {
    Q_OBJECT
  public:
    DlgDJAppSuggestions(WLibrary* pParent, UserSettingsPointer pConfig, Library* pLibrary);
    ~DlgDJAppSuggestions() override;

    void onShow() override;
    bool hasFocus() const override;
    void setFocus() override;

  signals:
    void loadTrackToPlayer(TrackPointer pTrack, const QString& group, bool play);

  private slots:
    void slotTrackChanged(const QString& group, TrackPointer pNewTrack, TrackPointer pOldTrack);
    void slotCurrentPlayingTrackChanged(TrackPointer pTrack);
    void slotReadFinished();
    void slotLoadToFreeDeck();
    void slotPreview();
    void slotAddToQueue();
    void slotNotNow();
    void slotToggleWhy();
    void slotOnlyMatchingKeysToggled(bool checked);

  private:
    void refresh();
    void updateSourceLabel();
    // The track to suggest after: the playing deck's track, else the most
    // recently loaded deck track. "" if no deck has a track.
    QString chooseSourceLocation(QString* pDeckGroup) const;
    static DJAppSuggestionsQueryResult readInBackground(QString brainDbPath,
            QString sourceLocation,
            QString sourceDeckGroup,
            QStringList excludeOnDecks,
            QStringList dismissed,
            mixxx::DbConnectionPoolPtr pDbConnectionPool);
    std::optional<DJAppSuggestion> selectedSuggestion() const;
    void selectRow(DJAppSuggestionsTableModel* pModel, int row);
    void updateWhyPanel();

    UserSettingsPointer m_pConfig;
    Library* m_pLibrary;

    QLabel* m_pSourceLabel;
    QLabel* m_pStatusLabel;
    QLabel* m_pIndexBanner;
    QCheckBox* m_pOnlyMatchingKeysCheck;
    QPushButton* m_pRefreshButton;
    QTableView* m_pAllowedView;
    DJAppSuggestionsTableModel* m_pAllowedModel;
    QLabel* m_pRiskyLabel;
    QTableView* m_pRiskyView;
    DJAppSuggestionsTableModel* m_pRiskyModel;
    QLabel* m_pWhyLabel;
    QPushButton* m_pLoadButton;
    QPushButton* m_pPreviewButton;
    QPushButton* m_pAddButton;
    QPushButton* m_pNotNowButton;
    QPushButton* m_pWhyButton;

    QFutureWatcher<DJAppSuggestionsQueryResult> m_watcher;
    QTimer* m_pDebounceTimer;

    // Deck group -> currently loaded track's location ("" entries dropped).
    QHash<QString, QString> m_deckLocations;
    // Deck group -> the TrackPointer itself, for an immediate label before
    // the worker thread returns.
    QHash<QString, TrackPointer> m_deckTracks;
    // Deck group -> when it was last loaded (for "most recently loaded").
    QHash<QString, qint64> m_deckLoadOrder;
    qint64 m_loadCounter = 0;

    DJAppSuggestionsTableModel* m_pSelectedModel = nullptr;
    int m_selectedRow = -1;
    bool m_pendingRefresh = false;

    // "Nu acum", this session only (not persisted, not sent until BrainClient
    // exists); keyed by BrainDbReader::locationKey.
    QSet<QString> m_dismissed;

    QString m_currentSourceLocation;
    QString m_currentSourceDeckGroup;
    QString m_currentSourceCamelot;
};
