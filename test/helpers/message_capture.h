#pragma once

#include <QStringList>
#include <QtGlobal>

namespace test {

/// \brief Captures Qt log output for as long as it is in scope.
/// \details Some behaviour has no observable other than a log line: a warning that a
///          set's unsaved record is being discarded, or the absence of a message
///          claiming a set was renamed.  A test that does not read the log cannot tell
///          such a message from its absence, and both directions matter -- a log line
///          that should not be there is as much a defect as a missing one.
///
///          Warnings and info are both captured, because the messages worth asserting
///          on are split across the two: qCWarning() for "something was discarded",
///          qCInfo() for "this file says something unexpected".
class MessageCapture
{
public:
    MessageCapture();
    ~MessageCapture();
    MessageCapture(const MessageCapture&) = delete;
    MessageCapture& operator=(const MessageCapture&) = delete;

    /// \brief Every warning and info message logged since this object was created.
    const QStringList& messages() const { return m_messages; }
    /// \brief Whether any captured message contains \p text.
    bool contains(const QString& text) const;

private:
    static void handle(QtMsgType type, const QMessageLogContext& context, const QString& message);

    QStringList m_messages;
    QtMessageHandler m_previous = nullptr;
    static QStringList* s_messages;
};

} // namespace test
