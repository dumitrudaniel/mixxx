#include "library/djapp/djappui.h"

#include <QLabel>
#include <QWidget>

namespace djappui {

QString styleSheet() {
    return QStringLiteral(
            "#DJAppView { background-color: #151517; }"
            // The QScrollArea added for the laptop-screen fix (2026-10-09,
            // DlgDJAppSuggestions/DlgDJAppAutomix) sits between #DJAppView
            // and its content; its viewport is a plain QWidget that is NOT
            // covered by the "#DJAppView { ... }" rule above (that rule only
            // paints the #DJAppView widget itself), so it fell back to the
            // default palette background -- white on this laptop, per Dan's
            // report. "qt_scrollarea_viewport" is Qt's own fixed internal
            // object name for that widget, so this targets it reliably.
            "#DJAppView QScrollArea { background-color: #151517; border: none; }"
            "#DJAppView QWidget#qt_scrollarea_viewport { background-color: #151517; }"
            "#DJAppView QWidget#DJAppViewContent { background-color: #151517; }"
            "#DJAppView QLabel { color: #c8c8c8; }"
            "#DJAppView QLabel#DJAppTitle { color: #eeeeee; font-weight: bold; font-size: 15px; }"
            "#DJAppView QLabel#DJAppMuted { color: #8a8a8a; }"
            "#DJAppView QLabel#DJAppSummary { color: #e0e0e0; font-weight: bold; }"
            "#DJAppView QLabel#DJAppBanner { color: #f0c870; background-color: #231d0f;"
            "  border: 1px solid #6a5420; padding: 6px 8px; }"
            "#DJAppView QTableView { background-color: #0f0f0f;"
            "  alternate-background-color: #0a0a0a; color: #d2d2d2; gridline-color: #1e1e1e;"
            "  selection-background-color: #2c454f; selection-color: #ffffff;"
            "  border: 1px solid #000000; }"
            "#DJAppView QHeaderView::section { background-color: #1c1c1e; color: #a8a8a8;"
            "  border: none; border-right: 1px solid #111111; padding: 3px 6px; }"
            "#DJAppView QTableCornerButton::section { background-color: #1c1c1e; border: none; }"
            "#DJAppView QPushButton { color: #d2d2d2; background-color: #2a2a2c;"
            "  border: 1px solid #3a3a3c; border-radius: 2px; padding: 3px 10px; }"
            "#DJAppView QPushButton:hover { background-color: #333336; }"
            "#DJAppView QPushButton:pressed { background-color: #257b82; }"
            "#DJAppView QPushButton:disabled { color: #555555; background-color: #1d1d1f;"
            "  border-color: #262628; }"
            "#DJAppView QCheckBox, #DJAppView QRadioButton { color: #c8c8c8; }"
            "#DJAppView QCheckBox:disabled, #DJAppView QRadioButton:disabled { color: #666666; }"
            "#DJAppView QComboBox, #DJAppView QSpinBox, #DJAppView QLineEdit {"
            "  color: #c8c8c8; background-color: #0f0f0f; border: 1px solid #2a2a2c;"
            "  padding: 1px 4px; }"
            "#DJAppView QComboBox:disabled, #DJAppView QSpinBox:disabled,"
            "  #DJAppView QLineEdit:disabled { color: #666666; }"
            "#DJAppView QGroupBox { color: #c8c8c8; border: 1px solid #2a2a2c;"
            "  margin-top: 14px; padding-top: 6px; }"
            "#DJAppView QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }"
            "#DJAppView QProgressBar { color: #c8c8c8; background-color: #0f0f0f;"
            "  border: 1px solid #2a2a2c; text-align: center; }");
}

void setupView(QWidget* pView) {
    pView->setObjectName(QStringLiteral("DJAppView"));
    pView->setAttribute(Qt::WA_StyledBackground, true);
    pView->setStyleSheet(styleSheet());
}

void setupScrollContent(QWidget* pContent) {
    pContent->setObjectName(QStringLiteral("DJAppViewContent"));
    pContent->setAttribute(Qt::WA_StyledBackground, true);
}

QLabel* newTitle(const QString& text, QWidget* pParent) {
    auto* pLabel = new QLabel(text, pParent);
    pLabel->setObjectName(QStringLiteral("DJAppTitle"));
    return pLabel;
}

QLabel* newMutedLabel(const QString& text, QWidget* pParent) {
    auto* pLabel = new QLabel(text, pParent);
    pLabel->setObjectName(QStringLiteral("DJAppMuted"));
    pLabel->setWordWrap(true);
    return pLabel;
}

QLabel* newWorkInProgressBanner(const QString& html, QWidget* pParent) {
    auto* pLabel = new QLabel(html, pParent);
    pLabel->setObjectName(QStringLiteral("DJAppBanner"));
    pLabel->setTextFormat(Qt::RichText);
    pLabel->setWordWrap(true);
    return pLabel;
}

} // namespace djappui
