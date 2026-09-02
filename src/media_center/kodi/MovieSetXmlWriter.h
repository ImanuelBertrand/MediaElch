#pragma once

#include "utils/Meta.h"

#include <QByteArray>

class MovieSet;

namespace mediaelch {
namespace kodi {

/// \brief Writes a movie set's own record, `set.nfo`.
/// \details The counterpart of MovieSetXmlReader; see there for why `<originaltitle>` and
///          not `<name>`.  The file does not vary by Kodi version: `set.nfo` first shipped
///          in Kodi 22, and on 19-21 it is simply an unread file next to the artwork.
class MovieSetXmlWriter
{
public:
    explicit MovieSetXmlWriter(const MovieSet& set);

    ELCH_NODISCARD QByteArray getMovieSetXml() const;

private:
    const MovieSet& m_set;
};

} // namespace kodi
} // namespace mediaelch
