#pragma once

#include "utils/Meta.h"

#include <QDomDocument>
#include <QString>

class MovieSet;

namespace mediaelch {
namespace kodi {

/// \brief Reads a movie set's own record, `set.nfo`, into a MovieSet.
/// \details `set.nfo` lives in the movie set information folder, one per set, and holds
///          everything about a set except its membership, which is in the member movies'
///          NFOs and never here.
///
///          The element names are not the movie NFO's for the same data: a movie NFO
///          carries `<set><name>`, `set.nfo` carries `<title>` and `<originaltitle>`, and it
///          is `<originaltitle>` that has to equal the movie NFOs' `<set><name>` byte for
///          byte.  Confusing the two is silent -- the file parses and Kodi keys the set's
///          database row off a name no movie mentions.
///
/// \see docs/concepts/movie-sets.md
class MovieSetXmlReader
{
public:
    explicit MovieSetXmlReader(MovieSet& set);

    /// \brief Parses \p domDoc into the set.  Returns true if it was a `<set>` document.
    /// \details Fills the set's title, overview and TMDB id.  It does not rename the set:
    ///          the caller found this file by the set's name, which is its key.
    ELCH_NODISCARD bool parseNfoDom(const QDomDocument& domDoc);

    /// \brief The set name a `set.nfo` document names, or an empty string if it names none.
    /// \details The join key, `<originaltitle>`, falling back to `<title>`.  This is how a
    ///          set is identified from its file alone, before any MovieSet exists.
    ELCH_NODISCARD static QString setNameOf(const QDomDocument& domDoc);

private:
    MovieSet& m_set;
};

} // namespace kodi
} // namespace mediaelch
