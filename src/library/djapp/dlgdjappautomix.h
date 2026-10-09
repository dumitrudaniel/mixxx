#pragma once

#include <QList>
#include <QPointer>
#include <QWidget>

#include "library/djapp/djappautopilot.h" // DJAppAutopilotCandidate
#include "library/libraryview.h"
#include "track/track_decl.h"

class QLabel;
class QPushButton;
class QVBoxLayout;
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
    // Up to kMaxCandidates rows for the picker (change 3); empty hides it.
    void slotCandidatesChanged(const QList<DJAppAutopilotCandidate>& candidates);

  private:
    void updateToggleText(bool enabled);
    void rebuildCandidates(const QList<DJAppAutopilotCandidate>& candidates);

    QPointer<DJAppAutopilot> m_pAutopilot;
    QPushButton* m_pToggleButton;
    QLabel* m_pStatusLabel;
    QWidget* m_pCandidatesSection;
    QVBoxLayout* m_pCandidatesLayout;
};
