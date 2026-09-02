#pragma once

#include "utils/Meta.h"

#include <QDomDocument>
#include <QString>

class MovieSet;

namespace mediaelch {
namespace kodi {

/// \brief Reads a movie set's own record, `set.nfo`, into a MovieSet.
/// \details `set.nfo` lives in the movie set information folder, one per set, and is
///          the authoritative copy of everything about a set that is not its
///          membership (D-A, D1b).  Membership is not in this file and never will be:
///          a set exists because a movie's NFO names it, on every Kodi version.
///
///          The element names are **not** the ones the movie NFO uses for the same
///          data.  A movie NFO carries `<set><name>`; `set.nfo` carries `<title>` and
///          `<originaltitle>`, and it is `<originaltitle>` that has to equal the movie
///          NFOs' `<set><name>` byte for byte, because that string is what Kodi 22
///          stores in `strOriginalSet` and matches on.  Getting the two spellings
///          confused is silent: the file parses, and Kodi keys the set's database row
///          off a name no movie mentions.
///
/// \see docs/concepts/movie-sets.md, D-A.
class MovieSetXmlReader
{
public:
    explicit MovieSetXmlReader(MovieSet& set);

    /// \brief Parses \p domDoc into the set.  Returns true if it was a `<set>` document.
    /// \details Fills the set's overview and TMDB id and leaves everything else alone.
    ///          In particular it does **not** rename the set: the set's name is its
    ///          primary key (D-B), the caller found this file by that name, and a
    ///          `<title>` that has moved away from `<originaltitle>` is a set-file-only
    ///          rename, which is D3a's business and not yet implemented.  Such a file
    ///          is read for its other fields and the divergence is logged.
    ELCH_NODISCARD bool parseNfoDom(const QDomDocument& domDoc);

    /// \brief The set name a `set.nfo` document names, or an empty string if it names none.
    /// \details The join key, `<originaltitle>`, falling back to `<title>` for a file
    ///          some other tool wrote with only that.  This is how a set is identified
    ///          from its file alone, before any MovieSet exists to read it into.
    ELCH_NODISCARD static QString setNameOf(const QDomDocument& domDoc);

private:
    MovieSet& m_set;
};

} // namespace kodi
} // namespace mediaelch
