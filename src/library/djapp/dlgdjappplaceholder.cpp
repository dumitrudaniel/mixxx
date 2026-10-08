#include "library/djapp/dlgdjappplaceholder.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include "library/djapp/djappui.h"
#include "moc_dlgdjappplaceholder.cpp"
#include "widget/wlibrary.h"

namespace {

QPushButton* disabledButton(const QString& text, QWidget* pParent) {
    auto* pButton = new QPushButton(text, pParent);
    pButton->setEnabled(false);
    return pButton;
}

QCheckBox* disabledCheckBox(const QString& text, bool checked, QWidget* pParent) {
    auto* pCheckBox = new QCheckBox(text, pParent);
    pCheckBox->setChecked(checked);
    pCheckBox->setEnabled(false);
    return pCheckBox;
}

QComboBox* disabledComboBox(const QStringList& items, QWidget* pParent) {
    auto* pComboBox = new QComboBox(pParent);
    pComboBox->addItems(items);
    pComboBox->setEnabled(false);
    return pComboBox;
}

QTableWidget* emptyTable(const QStringList& headers, int stretchColumn, QWidget* pParent) {
    auto* pTable = new QTableWidget(0, static_cast<int>(headers.size()), pParent);
    pTable->setHorizontalHeaderLabels(headers);
    pTable->verticalHeader()->hide();
    pTable->setAlternatingRowColors(true);
    pTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    pTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    pTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    pTable->horizontalHeader()->setSectionResizeMode(stretchColumn, QHeaderView::Stretch);
    return pTable;
}

QHBoxLayout* row(std::initializer_list<QWidget*> widgets, bool stretchAtEnd = true) {
    auto* pLayout = new QHBoxLayout();
    pLayout->setSpacing(8);
    for (QWidget* pWidget : widgets) {
        pLayout->addWidget(pWidget);
    }
    if (stretchAtEnd) {
        pLayout->addStretch(1);
    }
    return pLayout;
}

} // namespace

DlgDJAppPlaceholder::DlgDJAppPlaceholder(WLibrary* pParent, Kind kind)
        : QWidget(pParent),
          m_kind(kind) {
    djappui::setupView(this);
    auto* pLayout = new QVBoxLayout(this);
    pLayout->setContentsMargins(8, 6, 8, 6);
    pLayout->setSpacing(6);
    switch (m_kind) {
    case Kind::SetAssistant:
        buildSetAssistant();
        break;
    case Kind::Automix:
        buildAutomix();
        break;
    }
}

void DlgDJAppPlaceholder::buildSetAssistant() {
    auto* pLayout = static_cast<QVBoxLayout*>(layout());
    pLayout->addWidget(djappui::newTitle(QStringLiteral("Asistent set"), this));
    pLayout->addWidget(djappui::newWorkInProgressBanner(
            QStringLiteral(
                    "<b>În lucru (Etapa 4).</b> Planul se generează în brain (<code>POST "
                    "/plan</code>, căutare „anytime”), iar Mixxx citește rezultatul din "
                    "brain.db (<code>set_plans</code> / <code>set_plan_items</code>, contract "
                    "încă nescris). „Aprobă” va crea un playlist Mixxx obișnuit."),
            this));

    auto* pWaves = new QRadioButton(QStringLiteral("Valuri de party"), this);
    pWaves->setChecked(true);
    pWaves->setEnabled(false);
    auto* pBest = new QRadioButton(QStringLiteral("Cele mai bune tranziții"), this);
    pBest->setEnabled(false);
    pLayout->addLayout(row({new QLabel(QStringLiteral("Mod:"), this), pWaves, pBest}));

    auto* pMinutes = new QSpinBox(this);
    pMinutes->setRange(5, 600);
    pMinutes->setValue(60);
    pMinutes->setSuffix(QStringLiteral(" min"));
    pMinutes->setEnabled(false);
    auto* pTracks = new QSpinBox(this);
    pTracks->setRange(0, 200);
    pTracks->setSpecialValueText(QStringLiteral("—"));
    pTracks->setSuffix(QStringLiteral(" piese"));
    pTracks->setEnabled(false);
    pLayout->addLayout(row({new QLabel(QStringLiteral("Durată:"), this),
            pMinutes,
            new QLabel(QStringLiteral("sau"), this),
            pTracks,
            new QLabel(QStringLiteral("Pornește de la:"), this),
            disabledComboBox({QStringLiteral("piesa care cântă acum"),
                                     QStringLiteral("o piesă aleasă"),
                                     QStringLiteral("automat")},
                    this)}));

    auto* pTitleFilter = new QLineEdit(this);
    pTitleFilter->setPlaceholderText(QStringLiteral("text din titlu"));
    pTitleFilter->setEnabled(false);
    pLayout->addLayout(row({new QLabel(QStringLiteral("Din:"), this),
            disabledComboBox({QStringLiteral("Toată biblioteca"),
                                     QStringLiteral("un crate"),
                                     QStringLiteral("un playlist")},
                    this),
            new QLabel(QStringLiteral("Exclude:"), this),
            disabledCheckBox(QStringLiteral("cântate azi"), true, this),
            disabledComboBox({QStringLiteral("crate „Nu acum”")}, this),
            pTitleFilter}));
    pLayout->addWidget(djappui::newMutedLabel(
            QStringLiteral("Avansat ▸ prag 0,75 · rețetă Standard 8 · end_start permis"), this));

    auto* pProgress = new QProgressBar(this);
    pProgress->setRange(0, 100);
    pProgress->setValue(0);
    pProgress->setFormat(QStringLiteral("—"));
    pProgress->setEnabled(false);
    pLayout->addLayout(row({disabledButton(QStringLiteral("Generează"), this),
            pProgress,
            disabledButton(QStringLiteral("Stop"), this)}));

    pLayout->addWidget(emptyTable({QStringLiteral("#"),
                                          QStringLiteral("Piesa"),
                                          QStringLiteral("BPM"),
                                          QStringLiteral("Key"),
                                          QStringLiteral("Intrare"),
                                          QStringLiteral("Tranziție (scor · motiv)"),
                                          QStringLiteral("Lock")},
                               5, // Tranziție
                               this),
            1);
    pLayout->addWidget(djappui::newMutedLabel(
            QStringLiteral("Pe dinafară: — · durată — · scor mediu — · minim —"), this));
    pLayout->addLayout(row({disabledButton(QStringLiteral("Înlocuiește…"), this),
            disabledButton(QStringLiteral("Scoate"), this),
            disabledButton(QStringLiteral("Re-planifică restul"), this),
            disabledButton(QStringLiteral("Randează offline"), this),
            disabledButton(QStringLiteral("Aprobă → playlist"), this)}));
}

void DlgDJAppPlaceholder::buildAutomix() {
    auto* pLayout = static_cast<QVBoxLayout*>(layout());
    pLayout->addWidget(djappui::newTitle(QStringLiteral("Automix · niciun set ales"), this));
    pLayout->addWidget(djappui::newWorkInProgressBanner(
            QStringLiteral(
                    "<b>În lucru (Etapa 5).</b> Automix pe un playlist Mixxx obișnuit, condus "
                    "de un driver nou care apasă MIX-ul existent (fără AutoDJ, fără "
                    "crossfader). Tranzițiile vin din planul aprobat (<code>set_plan_items</code>) "
                    "sau, cu brain oprit, din <code>mix_points</code> + Standard 8. Butoanele de "
                    "mai jos vor fi controalele <code>[DJAppAutomix]</code>."),
            this));
    pLayout->addLayout(row({new QLabel(QStringLiteral("Acum (A): —"), this),
            new QLabel(QStringLiteral("Urmează (B): —"), this)}));

    auto* pNext = new QGroupBox(QStringLiteral("Tranziția următoare"), this);
    auto* pNextLayout = new QGridLayout(pNext);
    const QStringList lines = {
            QStringLiteral("Rețeta: — · pornește la — (bara —), peste —"),
            QStringLiteral("Ieșirea: —"),
            QStringLiteral("Intrarea: —"),
            QStringLiteral("Tempo: — (întâlnire la mijloc, apoi înapoi la tempo-ul propriu)"),
            QStringLiteral("GARDĂ TON: — · GARDĂ VOCE: —"),
            QStringLiteral("Scor: —"),
    };
    for (int i = 0; i < lines.size(); ++i) {
        pNextLayout->addWidget(new QLabel(lines.at(i), pNext), i, 0);
    }
    pNextLayout->addWidget(disabledButton(QStringLiteral("LOCK"), pNext), 0, 1, Qt::AlignRight);
    pNextLayout->setColumnStretch(0, 1);
    pLayout->addWidget(pNext);

    pLayout->addLayout(row({new QLabel(QStringLiteral("Coada: —"), this),
            disabledButton(QStringLiteral("Sari"), this),
            disabledButton(QStringLiteral("Editează în Asistent"), this)}));
    pLayout->addLayout(row({new QLabel(QStringLiteral("AUTOMIX"), this),
            disabledButton(QStringLiteral("PORNIT"), this),
            new QLabel(QStringLiteral("MIX în —"), this),
            disabledButton(QStringLiteral("SARI"), this),
            disabledButton(QStringLiteral("+FRAZĂ"), this),
            disabledButton(QStringLiteral("PAUZĂ DUPĂ"), this),
            disabledButton(QStringLiteral("STOP"), this),
            disabledButton(QStringLiteral("Eu apăs MIX"), this)}));
    pLayout->addStretch(1);
}

void DlgDJAppPlaceholder::onShow() {
}

bool DlgDJAppPlaceholder::hasFocus() const {
    return QWidget::hasFocus();
}
