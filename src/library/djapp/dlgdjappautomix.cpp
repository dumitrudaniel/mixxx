#include "library/djapp/dlgdjappautomix.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "library/djapp/djappautopilot.h"
#include "library/djapp/djappui.h"
#include "moc_dlgdjappautomix.cpp"
#include "widget/wlibrary.h"

DlgDJAppAutomix::DlgDJAppAutomix(WLibrary* pParent, DJAppAutopilot* pAutopilot)
        : QWidget(pParent),
          m_pAutopilot(pAutopilot),
          m_pToggleButton(nullptr),
          m_pStatusLabel(nullptr) {
    djappui::setupView(this);
    auto* pLayout = new QVBoxLayout(this);
    pLayout->setContentsMargins(8, 6, 8, 6);
    pLayout->setSpacing(6);
    pLayout->addWidget(djappui::newTitle(QStringLiteral("Automix"), this));
    pLayout->addWidget(djappui::newMutedLabel(
            QStringLiteral(
                    "Variantă minimă (testul de azi, 9 oct): urmărește deck-ul care cântă, "
                    "iar la punctul de ieșire (brain.db, <code>mix_points</code>) încarcă cea "
                    "mai bună sugestie pe deck-ul liber (fără play) și apasă MIX. Dacă ai pus tu "
                    "o piesă pe deck-ul liber, rămâne piesa ta. Dacă apeși tu MIX, nu intervine."),
            this));

    m_pToggleButton = new QPushButton(this);
    connect(m_pToggleButton, &QPushButton::clicked, this, &DlgDJAppAutomix::slotToggleClicked);
    auto* pToggleRow = new QHBoxLayout();
    pToggleRow->addWidget(m_pToggleButton);
    pToggleRow->addStretch(1);
    pLayout->addLayout(pToggleRow);

    m_pStatusLabel = djappui::newMutedLabel(QStringLiteral("oprit"), this);
    pLayout->addWidget(m_pStatusLabel);
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

void DlgDJAppAutomix::onShow() {
}

bool DlgDJAppAutomix::hasFocus() const {
    return QWidget::hasFocus();
}
