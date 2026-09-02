#include "media_center/kodi/MovieSetXmlReader.h"

#include "data/TmdbId.h"
#include "data/movie/MovieSet.h"
#include "log/Log.h"

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
    // from the same name -- and log that Kodi displays something else, which is false.
    const QString originalTitle = untrimmedChildText(setElement, "originaltitle");
    const QString title = untrimmedChildText(setElement, "title");
    if (!originalTitle.isEmpty() && !title.isEmpty() && originalTitle != title) {
        // A set-file-only rename: Kodi 22 displays <title> and matches on
        // <originaltitle>.  MediaElch has one name per set and no way to hold both
        // (D-B), so the join key wins and the display name is dropped.  D3a is the
        // step that makes this a user-facing choice.
        qCInfo(generic) << "[MovieSetXmlReader] Movie set" << originalTitle << "is displayed as" << title
                        << "by Kodi; MediaElch keeps the name the member movies use.";
    }

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
