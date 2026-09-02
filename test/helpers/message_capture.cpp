#include "test/helpers/message_capture.h"

namespace test {

QStringList* MessageCapture::s_messages = nullptr;

MessageCapture::MessageCapture() : m_previous{qInstallMessageHandler(&MessageCapture::handle)}
{
    s_messages = &m_messages;
}

MessageCapture::~MessageCapture()
{
    qInstallMessageHandler(m_previous);
    s_messages = nullptr;
}

bool MessageCapture::contains(const QString& text) const
{
    for (const QString& message : m_messages) {
        if (message.contains(text)) {
            return true;
        }
    }
    return false;
}

void MessageCapture::handle(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    Q_UNUSED(context)
    if ((type == QtWarningMsg || type == QtInfoMsg) && s_messages != nullptr) {
        s_messages->append(message);
    }
}

} // namespace test
