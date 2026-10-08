#pragma once

#include <QWidget>

#include "library/libraryview.h"

class WLibrary;

// "Asistent set" and "Automix" (plan-ui-integrare §5.2–5.3): the layout from
// the plan, every control disabled, and an "în lucru" banner. Their data
// comes from brain.db tables whose contract is index-builder's / the
// automix driver's (set_plans / set_plan_items, [DJAppAutomix] controls),
// still to come; nothing here guesses that schema. See the TODO list in
// docs/decisions/0029. Sugestii is implemented for real: DlgDJAppSuggestions.
class DlgDJAppPlaceholder : public QWidget, public virtual LibraryView {
    Q_OBJECT
  public:
    enum class Kind {
        SetAssistant,
        Automix,
    };

    DlgDJAppPlaceholder(WLibrary* pParent, Kind kind);

    void onShow() override;
    bool hasFocus() const override;

  private:
    void buildSetAssistant();
    void buildAutomix();

    const Kind m_kind;
};
