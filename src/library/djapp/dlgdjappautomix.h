#pragma once

#include <QPointer>
#include <QWidget>

#include "library/libraryview.h"
#include "track/track_decl.h"

class DJAppAutopilot;
class QLabel;
class QPushButton;
class WLibrary;

// "Automix" (plan-ui-integrare.md §6 Etapa 5, cut down for a quick live
// test, 2026-10-09): a single toggle and a status line driven by
// DJAppAutopilot. No skin XML, plain Qt widget code (djappui.h style), like
// DlgDJAppSuggestions / DlgDJAppAnalysis. Replaces the Automix
// DlgDJAppPlaceholder in DJAppFeature::bindLibraryWidget.
class DlgDJAppAutomix : public QWidget, public virtual LibraryView {
    Q_OBJECT
  public:
    DlgDJAppAutomix(WLibrary* pParent, DJAppAutopilot* pAutopilot);

    void onShow() override;
    bool hasFocus() const override;

  private slots:
    void slotToggleClicked();
    void slotEnabledChanged(bool enabled);
    void slotStatusTextChanged(const QString& text);

  private:
    void updateToggleText(bool enabled);

    QPointer<DJAppAutopilot> m_pAutopilot;
    QPushButton* m_pToggleButton;
    QLabel* m_pStatusLabel;
};
