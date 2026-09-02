#pragma once

#include "utils/Meta.h"

#include <QPushButton>
#include <QString>
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
/// \details A widget that commits an edit on QEvent::FocusOut needs this, because opening
///          the drop-down delivers a focus-out that no focus reason tells apart from a real
///          one.  QLineEdit's own suppression of editingFinished() does not cover it: the
///          popup is a child of the combo box, not of the line edit.
/// \return false for a null \p comboBox, and false for a popup belonging to another widget.
ELCH_NODISCARD bool isOwnPopupOpen(const QComboBox* comboBox);

/// \brief Whether an event filter should commit \p comboBox's typed text now.
/// \details All four terms must hold: the event belongs to this combo box, it is a
///          QEvent::FocusOut, the combo box is not mid-repopulate, and the focus did not go
///          to its own drop-down.  The signalsBlocked() term reads a blockSignals() bracket
///          as "the widget is filling this in, not the user", since events are delivered
///          regardless; it does not propagate to the line edit inside the combo box.
/// \return false if \p comboBox or \p event is null.
ELCH_NODISCARD bool shouldCommitOnFocusOut(const QComboBox* comboBox, const QObject* watched, const QEvent* event);

/// \brief Whether \p comboBox holds an edit that has not reached the value it edits.
/// \details A widget whose save commits its editable combo box -- because the navbar's save
///          buttons are QToolButtons, take no focus, and so never end the edit -- cannot
///          commit unconditionally: the box is refilled only when the widget loads its
///          subject, so a value changed elsewhere in the meantime would be written back over
///          it.  \p committedText is what the box was last filled with or last committed, so
///          a box that still shows it holds no edit, whatever the subject says now.
/// \return false for a null \p comboBox.
ELCH_NODISCARD bool hasUncommittedEdit(const QComboBox* comboBox, const QString& committedText);

/// \brief Returns \p entries, with \p current added if it is not in them already.
/// \details For a combo box filled with setCurrentIndex(list.indexOf(current)): a missing
///          entry means setCurrentIndex(-1), which leaves an editable box displaying
///          nothing, and for a box that commits on focus-out the next focus loss then
///          writes that emptiness over the value it was meant to show.
/// \return \p entries with \p current in it, so that indexOf() cannot return -1.  A list
///         that already contains it is returned untouched.
ELCH_NODISCARD QStringList withCurrentValue(QStringList entries, const QString& current);

} // namespace ui
} // namespace mediaelch
