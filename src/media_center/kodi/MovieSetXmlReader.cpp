#include "media_center/kodi/MovieSetXmlReader.h"

#include "data/TmdbId.h"
#include "data/movie/MovieSet.h"

#include <QDomElement>

namespace mediaelch {
namespace kodi {

namespace {

/// \brief The text of \p parent's first \p tagName child, trimmed; empty if there is none.
QString childText(const QDomElement& parent, const QString& tagName)
{
    const QDomElement child = parent.firstChildElement(tagName);
    return child.isNull() ? QString() : child.text().trimmed();
}

/// \brief The text of \p parent's first \p tagName child, **not** trimmed.
/// \details Used for the set's name and nothing else.  The name is a join key that has
///          to be byte-identical to the `<set><name>` in every member movie's NFO, and
///          the movie NFO reader does not trim that either
///          (`MovieXmlReader::movieSet()`).  Normalising it here would silently fork the
///          two apart, and it would fork this reader from the folder name as well, since
///          Kodi's legalisation chops only *trailing* whitespace -- a name with a leading
///          space would be reported under one spelling and looked up under another.
QString untrimmedChildText(const QDomElement& parent, const QString& tagName)
{
    const QDomElement child = parent.firstChildElement(tagName);
    return child.isNull() ? QString() : child.text();
}

/// \brief The TMDB collection id of a `<set>` element, or NoId.
/// \details Only `<uniqueid type="tmdb">` is accepted.  Unlike the movie NFO, which has
///          to read three historical spellings because other tools wrote them, this file
///          is MediaElch's own: nothing else has ever put an id in it.  Kodi's
///          CSetInfoTag::ParseNative reads `<title>`, `<originaltitle>`, `<overview>` and
///          `<art>` and ignores every other child, so the id rides along unread.
TmdbId tmdbIdOf(const QDomElement& setElement)
{
    const QDomNodeList uniqueIds = setElement.elementsByTagName("uniqueid");
    for (int i = 0, n = uniqueIds.size(); i < n; ++i) {
        const QDomElement uniqueId = uniqueIds.at(i).toElement();
        if (uniqueId.attribute("type") == "tmdb") {
            const QString id = uniqueId.text().trimmed();
            if (!id.isEmpty()) {
                return TmdbId(id);
            }
        }
    }
    return TmdbId::NoId;
}

} // namespace

MovieSetXmlReader::MovieSetXmlReader(MovieSet& set) : m_set{set}
{
}

bool MovieSetXmlReader::parseNfoDom(const QDomDocument& domDoc)
{
    const QDomElement setElement = domDoc.documentElement();
    if (setElement.isNull() || setElement.tagName() != "set") {
        return false;
    }

    // Both read the same way.  Comparing an untrimmed <originaltitle> against a trimmed
    // <title> makes every set whose name carries leading or trailing whitespace look like
    // a set-file-only rename -- in MediaElch's *own* files, where the writer emits both
    // from the same name -- so a set would be shown under a spelling its own files do
    // not carry.
    const QString originalTitle = untrimmedChildText(setElement, "originaltitle");
    const QString title = untrimmedChildText(setElement, "title");
    // A set-file-only rename: Kodi 22 displays <title> and matches on <originaltitle>,
    // and this reader keeps both -- the key in the set's name and the display title
    // beside it (D-B).  Dropping the title, which is what this used to do, would make
    // every reload undo the last set-file-only rename.
    //
    // Only where there is something to diverge *from*: the set's name comes from
    // setNameOf(), which falls back to <title> for a file that has no <originaltitle>,
    // so for such a file the two are already one string and a display title would be a
    // duplicate of the name.  setTitle() normalises that away in any case; not reaching
    // for it here is what keeps the intent readable.
    m_set.setTitle(originalTitle.isEmpty() ? QString() : title);

    m_set.setOverview(childText(setElement, "overview"));
    m_set.setTmdbId(tmdbIdOf(setElement));

    // <art> is deliberately not read into the set.  A set's artwork is the image files
    // in the same folder, which is what Kodi 19-22 all read first; set.nfo's <art> is
    // only Kodi 22's fallback for a folder that has none (VideoInfoScanner.cpp:866-869).
    // MediaElch writes the files, so the fallback would be a second source of truth for
    // the same images.  See docs/concepts/movie-sets.md, D-A.

    return true;
}

QString MovieSetXmlReader::setNameOf(const QDomDocument& domDoc)
{
    const QDomElement setElement = domDoc.documentElement();
    if (setElement.isNull() || setElement.tagName() != "set") {
        return {};
    }
    const QString originalTitle = untrimmedChildText(setElement, "originaltitle");
    if (!originalTitle.isEmpty()) {
        return originalTitle;
    }
    // <originaltitle> is what a member NFO's <set><name> has to match, so it is the
    // name.  A file that has only <title> was not written by MediaElch; take it rather
    // than ignoring the set, since the alternative is a set the user cannot see at all.
    return untrimmedChildText(setElement, "title");
}

} // namespace kodi
} // namespace mediaelch
