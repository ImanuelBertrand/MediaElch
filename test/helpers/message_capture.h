#pragma once

#include <QStringList>
#include <QtGlobal>

namespace test {

/// \brief Captures Qt log output for as long as it is in scope.
/// \details Some behaviour has no observable but a log line -- a warning that an unsaved
///          record is being discarded, or the absence of one claiming a set was renamed --
///          and both directions matter.  Warnings and info are both captured, because the
///          messages worth asserting on are split across the two.
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
