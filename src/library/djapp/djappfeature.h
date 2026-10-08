#pragma once

#include <QPointer>
#include <QVariant>

#include "library/libraryfeature.h"
#include "preferences/usersettings.h"
#include "util/parented_ptr.h"

class DJAppAutopilot;
class DlgDJAppAnalysis;
class DlgDJAppAutomix;
class DlgDJAppSuggestions;
class Library;
class TreeItem;
class WLibrary;

// DJ App (docs/plan-ui-integrare.md §5, docs/decisions/0029): the "DJ App"
// entry in the library sidebar, with the children Sugestii, Asistent set,
// Automix and Analiză. Same pattern as AutoDJFeature + DlgAutoDJ: plain Qt
// widgets registered as library views, no skin parser changes.
//
// Upstream code touched: one addFeature() line in Library, CMake. Everything
// else lives in src/library/djapp/.
class DJAppFeature : public LibraryFeature {
    Q_OBJECT
  public:
    DJAppFeature(Library* pLibrary, UserSettingsPointer pConfig);
    ~DJAppFeature() override = default;

    QVariant title() override;
    void bindLibraryWidget(WLibrary* pLibraryWidget, KeyboardEventFilter* pKeyboard) override;
    TreeItemModel* sidebarModel() const override;

  public slots:
    void activate() override;
    void activateChild(const QModelIndex& index) override;

  private slots:
    void slotWaitingCountChanged(int waiting);

  private:
    void switchToChildView(const QString& viewName);

    parented_ptr<TreeItemModel> m_pSidebarModel;
    TreeItem* m_pAnalysisItem;
    QPointer<DlgDJAppAnalysis> m_pAnalysisView;
    QPointer<DlgDJAppSuggestions> m_pSuggestionsView;
    QPointer<DlgDJAppAutomix> m_pAutomixView;
    parented_ptr<DJAppAutopilot> m_pAutopilot;
};
