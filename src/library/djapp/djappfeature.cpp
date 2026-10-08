#include "library/djapp/djappfeature.h"

#include <QTimer>

#include "controllers/keyboard/keyboardeventfilter.h"
#include "library/djapp/dlgdjappanalysis.h"
#include "library/djapp/dlgdjappplaceholder.h"
#include "library/djapp/dlgdjappsuggestions.h"
#include "library/library.h"
#include "library/treeitem.h"
#include "library/treeitemmodel.h"
#include "moc_djappfeature.cpp"
#include "widget/wlibrary.h"

namespace {

// View names registered with WLibrary (must not clash with upstream ones).
const QString kViewSuggestions = QStringLiteral("DJApp.Sugestii");
const QString kViewSetAssistant = QStringLiteral("DJApp.AsistentSet");
const QString kViewAutomix = QStringLiteral("DJApp.Automix");
const QString kViewAnalysis = QStringLiteral("DJApp.Analiza");

const QString kAnalysisLabel = QStringLiteral("Analiză");

// First background read of brain.db, for the "Analiză N" badge, a moment
// after startup so it never competes with the skin and library loading.
constexpr int kInitialReadDelayMs = 3000;

} // namespace

DJAppFeature::DJAppFeature(Library* pLibrary, UserSettingsPointer pConfig)
        : LibraryFeature(pLibrary, pConfig, QStringLiteral("djapp")),
          m_pSidebarModel(make_parented<TreeItemModel>(this)),
          m_pAnalysisItem(nullptr) {
    std::unique_ptr<TreeItem> pRootItem = TreeItem::newRoot(this);
    pRootItem->appendChild(QStringLiteral("Sugestii"), kViewSuggestions);
    pRootItem->appendChild(QStringLiteral("Asistent set"), kViewSetAssistant);
    pRootItem->appendChild(QStringLiteral("Automix"), kViewAutomix);
    m_pAnalysisItem = pRootItem->appendChild(kAnalysisLabel, kViewAnalysis);
    m_pSidebarModel->setRootItem(std::move(pRootItem));
}

QVariant DJAppFeature::title() {
    return QStringLiteral("DJ App");
}

TreeItemModel* DJAppFeature::sidebarModel() const {
    return m_pSidebarModel;
}

void DJAppFeature::bindLibraryWidget(WLibrary* pLibraryWidget, KeyboardEventFilter* pKeyboard) {
    // WLibrary owns and deletes the views.
    const auto registerPlaceholder = [pLibraryWidget, pKeyboard](
                                             const QString& name,
                                             DlgDJAppPlaceholder::Kind kind) {
        auto* pView = new DlgDJAppPlaceholder(pLibraryWidget, kind);
        pView->installEventFilter(pKeyboard);
        pLibraryWidget->registerView(name, pView);
    };
    registerPlaceholder(kViewSetAssistant, DlgDJAppPlaceholder::Kind::SetAssistant);
    registerPlaceholder(kViewAutomix, DlgDJAppPlaceholder::Kind::Automix);

    m_pSuggestionsView = new DlgDJAppSuggestions(pLibraryWidget, m_pConfig, m_pLibrary);
    m_pSuggestionsView->installEventFilter(pKeyboard);
    pLibraryWidget->registerView(kViewSuggestions, m_pSuggestionsView);
    connect(m_pSuggestionsView.data(),
            &DlgDJAppSuggestions::loadTrackToPlayer,
            this,
            [this](TrackPointer pTrack, const QString& group, bool play) {
                emit loadTrackToPlayer(pTrack,
                        group,
#ifdef __STEM__
                        mixxx::StemChannelSelection(),
#endif
                        play);
            });

    m_pAnalysisView = new DlgDJAppAnalysis(pLibraryWidget, m_pConfig, m_pLibrary);
    m_pAnalysisView->installEventFilter(pKeyboard);
    pLibraryWidget->registerView(kViewAnalysis, m_pAnalysisView);
    connect(m_pAnalysisView.data(),
            &DlgDJAppAnalysis::waitingCountChanged,
            this,
            &DJAppFeature::slotWaitingCountChanged);
    connect(m_pAnalysisView.data(),
            &DlgDJAppAnalysis::loadTrack,
            this,
            &DJAppFeature::loadTrack);
    QTimer::singleShot(kInitialReadDelayMs,
            m_pAnalysisView.data(),
            &DlgDJAppAnalysis::refresh);
}

void DJAppFeature::activate() {
    switchToChildView(kViewSuggestions);
}

void DJAppFeature::activateChild(const QModelIndex& index) {
    const TreeItem* pItem = static_cast<TreeItem*>(index.internalPointer());
    if (!pItem) {
        return;
    }
    switchToChildView(pItem->getData().toString());
}

void DJAppFeature::switchToChildView(const QString& viewName) {
    emit switchToView(viewName);
    if (viewName == kViewAnalysis && m_pAnalysisView) {
        // The library search box filters the Analiză table.
        emit restoreSearch(m_pAnalysisView->currentSearch());
    } else {
        emit disableSearch();
    }
    emit enableCoverArtDisplay(true);
}

void DJAppFeature::slotWaitingCountChanged(int waiting) {
    if (!m_pAnalysisItem) {
        return;
    }
    const QString label = waiting > 0
            ? QStringLiteral("%1  %2").arg(kAnalysisLabel).arg(waiting)
            : kAnalysisLabel;
    const QModelIndex index = m_pSidebarModel->index(m_pAnalysisItem->parentRow(), 0);
    m_pSidebarModel->setData(index, label, Qt::DisplayRole);
}
