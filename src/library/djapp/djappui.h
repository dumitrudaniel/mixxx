#pragma once

#include <QString>

class QLabel;
class QWidget;

// DJ App (docs/decisions/0029): small shared bits of the DJ App library views.
namespace djappui {

// Dark style scoped to "#DJAppView" so it never leaks into upstream widgets.
// Colors follow SeratoLike (PaleMoon): the skin QSS does not style generic
// Qt widgets inside WLibrary, and Mixxx's window palette is dark.
QString styleSheet();

// Root setup for a DJ App view: object name + scoped style.
void setupView(QWidget* pView);

// Bold view title ("Analiză", "Sugestii", ...).
QLabel* newTitle(const QString& text, QWidget* pParent);
// Secondary grey text.
QLabel* newMutedLabel(const QString& text, QWidget* pParent);
// The amber "în lucru" banner of a view whose data contract is not ready.
QLabel* newWorkInProgressBanner(const QString& html, QWidget* pParent);

} // namespace djappui
