#include "library/djapp/dlgdjappautomix.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "library/djapp/djappautopilot.h"
#include "library/djapp/djappsuggestions.h"
#include "library/djapp/djappui.h"
#include "moc_dlgdjappautomix.cpp"
#include "widget/wlibrary.h"

DlgDJAppAutomix::DlgDJAppAutomix(WLibrary* pParent, DJAppAutopilot* pAutopilot)
        : QWidget(pParent),
          m_pAutopilot(pAutopilot),
          m_pToggleButton(nullptr),
          m_pStatusLabel(nullptr),
          m_pCandidatesSection(nullptr),
          m_pCandidatesLayout(nullptr) {
    djappui::setupView(this);

    // Same laptop-screen fix as DlgDJAppSuggestions (Dan reported this
    // class of problem for the DJ App panel generally, not just one view):
    // wrap the content in a QScrollArea so a short library panel scrolls
    // instead of clipping the toggle/status/candidate list. Invisible on a
    // tall-enough screen.
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
    pLayout->addWidget(djappui::newTitle(QStringLiteral("Automix"), pContent));
    pLayout->addWidget(djappui::newMutedLabel(
            QStringLiteral(
                    "Variantă minimă (testul de azi, 9 oct, extinsă cu feedback-ul lui Dan): "
                    "urmărește deck-ul care cântă; cu %1 s înainte de punctul de ieșire "
                    "(brain.db, <code>mix_points</code>) arată până la 3 variante mai jos -- "
                    "alege tu una, sau dacă nu alegi nimic, la ieșire încarcă automat cea mai "
                    "bună (sau cea mai apropiată, dacă nu e nici una ideală) și apasă MIX. "
                    "Dacă ai pus tu o piesă pe deck-ul liber, rămâne piesa ta și nu arată "
                    "variante. Dacă apeși tu MIX, nu intervine.")
                    .arg(static_cast<int>(djapp::autopilot::kLookaheadSec)),
            pContent));

    m_pToggleButton = new QPushButton(pContent);
    connect(m_pToggleButton, &QPushButton::clicked, this, &DlgDJAppAutomix::slotToggleClicked);
    auto* pToggleRow = new QHBoxLayout();
    pToggleRow->addWidget(m_pToggleButton);
    pToggleRow->addStretch(1);
    pLayout->addLayout(pToggleRow);

    m_pStatusLabel = djappui::newMutedLabel(QStringLiteral("oprit"), pContent);
    pLayout->addWidget(m_pStatusLabel);

    m_pCandidatesSection = new QWidget(pContent);
    m_pCandidatesLayout = new QVBoxLayout(m_pCandidatesSection);
    m_pCandidatesLayout->setContentsMargins(0, 0, 0, 0);
    m_pCandidatesLayout->setSpacing(4);
    m_pCandidatesSection->setVisible(false);
    pLayout->addWidget(m_pCandidatesSection);

    pLayout->addStretch(1);

    const bool enabled = m_pAutopilot ? m_pAutopilot->isEnabled() : false;
    updateToggleText(enabled);

    if (m_pAutopilot) {
        connect(m_pAutopilot.data(),
                &DJAppAutopilot::enabledChanged,
                this,
                &DlgDJAppAutomix::slotEnabledChanged);
        connect(m_pAutopilot.data(),
                &DJAppAutopilot::statusTextChanged,
                this,
                &DlgDJAppAutomix::slotStatusTextChanged);
        connect(m_pAutopilot.data(),
                &DJAppAutopilot::candidatesChanged,
                this,
                &DlgDJAppAutomix::slotCandidatesChanged);
    } else {
        m_pToggleButton->setEnabled(false);
    }
}

void DlgDJAppAutomix::updateToggleText(bool enabled) {
    m_pToggleButton->setText(enabled ? QStringLiteral("OPREȘTE AUTOMIX")
                                      : QStringLiteral("PORNEȘTE AUTOMIX"));
}

void DlgDJAppAutomix::slotToggleClicked() {
    if (!m_pAutopilot) {
        return;
    }
    m_pAutopilot->setEnabled(!m_pAutopilot->isEnabled());
}

void DlgDJAppAutomix::slotEnabledChanged(bool enabled) {
    updateToggleText(enabled);
}

void DlgDJAppAutomix::slotStatusTextChanged(const QString& text) {
    m_pStatusLabel->setText(text);
}

void DlgDJAppAutomix::slotCandidatesChanged(const QList<DJAppAutopilotCandidate>& candidates) {
    rebuildCandidates(candidates);
}

void DlgDJAppAutomix::rebuildCandidates(const QList<DJAppAutopilotCandidate>& candidates) {
    QLayoutItem* pItem;
    while ((pItem = m_pCandidatesLayout->takeAt(0)) != nullptr) {
        delete pItem->widget();
        delete pItem;
    }
    m_pCandidatesSection->setVisible(!candidates.isEmpty());
    if (candidates.isEmpty()) {
        return;
    }
    // Same allowed/risky split dlgdjappsuggestions.cpp uses (two sections,
    // the risky one introduced by a muted separator label), just a 1-3 row
    // list instead of a full table (change 3).
    bool shownRiskySeparator = false;
    for (int i = 0; i < candidates.size(); ++i) {
        const DJAppAutopilotCandidate& candidate = candidates.at(i);
        if (!candidate.allowed && !shownRiskySeparator) {
            m_pCandidatesLayout->addWidget(djappui::newMutedLabel(
                    QStringLiteral("── riscant (nicio opțiune ideală) ──"),
                    m_pCandidatesSection));
            shownRiskySeparator = true;
        }
        // Dan, 2026-10-09: flag when this pair's recipe_hint is fade_curat -
        // the autopilot will switch off his selected beatmatched recipe for
        // just this one transition (tempo step too big or an unreliable
        // grid), same as if he'd picked a different recipe row himself.
        const QString recipeFlag = candidate.recipeHint == QLatin1String("fade_curat")
                ? QStringLiteral("  ·  ⚠ %1")
                          .arg(DJAppSuggestions::recipeText(candidate.recipeHint))
                : QString();
        const QString text = QStringLiteral("%1  ·  %2  ·  %3 BPM  ·  %4%5")
                                      .arg(candidate.label,
                                              DJAppSuggestions::scoreText(candidate.score),
                                              DJAppSuggestions::stepText(candidate.stepBpm),
                                              candidate.keyVerdict,
                                              recipeFlag);
        auto* pButton = new QPushButton(text, m_pCandidatesSection);
        pButton->setFlat(true);
        pButton->setStyleSheet(QStringLiteral("text-align: left; padding: 3px 6px;"));
        connect(pButton, &QPushButton::clicked, this, [this, i]() {
            if (m_pAutopilot) {
                m_pAutopilot->chooseCandidate(i);
            }
        });
        m_pCandidatesLayout->addWidget(pButton);
    }
}

void DlgDJAppAutomix::onShow() {
}

bool DlgDJAppAutomix::hasFocus() const {
    return QWidget::hasFocus();
}
