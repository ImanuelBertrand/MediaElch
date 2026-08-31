#pragma once

#include "utils/Meta.h"

#include <QPushButton>
#include <QStringList>

class QComboBox;
class QEvent;

namespace mediaelch {
namespace ui {

enum class ButtonStyle
{
    Primary,
    Info,
    Danger,
    Success,
    Warning
};

void setButtonStyle(QPushButton* button, ButtonStyle style);

/// \brief Whether \p comboBox currently has its own drop-down list open.
/// \details An editable QComboBox makes *itself* the focus proxy of the line edit it
///          creates, so the combo box is the focus widget and QEvent::FocusOut is
///          delivered to it -- including the one that opening the drop-down causes,
///          which cannot be told apart by focus reason: showPopup() delivers a
///          FocusOut with Qt::PopupFocusReason and then a second one with
///          Qt::OtherFocusReason while the popup is up, and Qt::OtherFocusReason is
///          also what disabling the widget and switching QStackedWidget page produce
///          (all measured on Qt 6.8.2).  So a widget that commits an edit on
///          focus-out has to ask whether the drop-down is the reason, and this is
///          that question.
///
///          The popup container is a child of the *combo box*, not of its line edit,
///          which is why QLineEdit's own suppression of editingFinished() -- it tests
///          QApplication::activePopupWidget()->parentWidget() against the line edit --
///          does not cover this case.
/// \return false for a null \p comboBox, and false for any popup that belongs to some
///         other widget.
ELCH_NODISCARD bool isOwnPopupOpen(const QComboBox* comboBox);

/// \brief Whether an event filter should commit \p comboBox's typed text now.
/// \details The commit rule for an editable combo box that saves on focus-out, in one
///          place so it can be tested without building the widget that uses it.  All
///          four terms must hold: the event belongs to this combo box, it is a
///          QEvent::FocusOut, the combo box is not mid-repopulate, and the focus did
///          not go to this combo box's own drop-down.
///
///          The signalsBlocked() term reads a blockSignals() bracket -- the usual way a
///          widget marks "I am filling this in, the user is not" -- as "not an edit",
///          because events are delivered regardless of blockSignals().  It covers the
///          combo box only: blockSignals() does not propagate to the line edit inside
///          it, so anything wired to that line edit is outside the bracket.
/// \return false if \p comboBox or \p event is null.
ELCH_NODISCARD bool shouldCommitOnFocusOut(const QComboBox* comboBox, const QObject* watched, const QEvent* event);

/// \brief Returns \p entries, with \p current added if it is not in them already.
/// \details A combo box is filled from a list and then told to show one of its
///          entries with setCurrentIndex(list.indexOf(current)).  If the list does
///          not happen to contain \p current, indexOf() returns -1 and
///          setCurrentIndex(-1) leaves an *editable* combo box displaying nothing --
///          which, for a box that commits its text when it loses focus, is not a
///          cosmetic defect: the next focus loss commits the empty string over the
///          value the box was supposed to be showing.
///
///          So a list that has to be able to display a value has to contain it, and
///          this is the one place that guarantees it.  An entry added here is one the
///          list's own source did not know about, which means that source is stale;
///          the box still shows the truth.
/// \return \p entries with \p current guaranteed to be in it, so that indexOf() on the
///         result cannot return -1.  A list that already contains it is returned
///         untouched.
ELCH_NODISCARD QStringList withCurrentValue(QStringList entries, const QString& current);

} // namespace ui
} // namespace mediaelch
