#include "ui/UiUtils.h"

#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QString>
#include <QVariant>
#include <QWidget>

namespace mediaelch {
namespace ui {

void setButtonStyle(QPushButton* button, ButtonStyle style)
{
    switch (style) {
    case ButtonStyle::Danger: button->setProperty("buttonstyle", QStringLiteral("danger")); break;
    case ButtonStyle::Primary: button->setProperty("buttonstyle", QStringLiteral("primary")); break;
    case ButtonStyle::Info: button->setProperty("buttonstyle", QStringLiteral("info")); break;
    case ButtonStyle::Warning: button->setProperty("buttonstyle", QStringLiteral("warning")); break;
    case ButtonStyle::Success: button->setProperty("buttonstyle", QStringLiteral("success")); break;
    }
}

bool isOwnPopupOpen(const QComboBox* comboBox)
{
    if (comboBox == nullptr) {
        return false;
    }
    const QWidget* popup = QApplication::activePopupWidget();
    return popup != nullptr && popup->parentWidget() == comboBox;
}

bool shouldCommitOnFocusOut(const QComboBox* comboBox, const QObject* watched, const QEvent* event)
{
    if (comboBox == nullptr || event == nullptr) {
        return false;
    }
    return watched == comboBox && event->type() == QEvent::FocusOut && !comboBox->signalsBlocked()
           && !isOwnPopupOpen(comboBox);
}

} // namespace ui
} // namespace mediaelch
