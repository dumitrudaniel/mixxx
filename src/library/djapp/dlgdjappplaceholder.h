#pragma once

#include <QWidget>

#include "library/libraryview.h"
#include "track/track_decl.h"

class QLabel;
class WLibrary;

// "Sugestii", "Asistent set" and "Automix" (plan-ui-integrare §5.1–5.3): the
// layout from the plan, every control disabled, and an "în lucru" banner.
// Their data comes from brain.db tables whose contract is index-builder's
// (ADR 0026: pair_scores; set_plans / set_plan_items still to come); nothing
// here guesses that schema. See the TODO list in docs/decisions/0029.
class DlgDJAppPlaceholder : public QWidget, public virtual LibraryView {
    Q_OBJECT
  public:
    enum class Kind {
        Suggestions,
        SetAssistant,
        Automix,
    };

    DlgDJAppPlaceholder(WLibrary* pParent, Kind kind);

    void onShow() override;
    bool hasFocus() const override;

  private slots:
    void slotCurrentPlayingTrackChanged(TrackPointer pTrack);

  private:
    void buildSuggestions();
    void buildSetAssistant();
    void buildAutomix();

    const Kind m_kind;
    // Sugestii: "Sugestii după: <piesa care cântă>" (PlayerInfo, read only).
    QLabel* m_pNowPlayingLabel;
};
