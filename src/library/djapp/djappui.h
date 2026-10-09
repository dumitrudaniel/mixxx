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

// Dan, 2026-10-09 (laptop report): a QScrollArea's own inner content widget
// (the one passed to QScrollArea::setWidget(), not the scroll area itself
// and not Qt's internal "qt_scrollarea_viewport") paints its own default
// palette background - white on that machine - which sat in front of
// #DJAppView's background and made the whole panel look unstyled (title
// text still took its dark-theme color via the descendant selector, so it
// read as near-invisible white-on-white). Call this right after creating
// that content widget, in every DJ App view that wraps itself in a
// QScrollArea.
void setupScrollContent(QWidget* pContent);

// Bold view title ("Analiză", "Sugestii", ...).
QLabel* newTitle(const QString& text, QWidget* pParent);
// Secondary grey text.
QLabel* newMutedLabel(const QString& text, QWidget* pParent);
// The amber "în lucru" banner of a view whose data contract is not ready.
QLabel* newWorkInProgressBanner(const QString& html, QWidget* pParent);

} // namespace djappui
