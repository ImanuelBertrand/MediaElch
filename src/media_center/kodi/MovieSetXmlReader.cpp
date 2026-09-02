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
/// \details The set's name is a join key that has to be byte-identical to the `<set><name>`
///          in every member movie's NFO, which MovieXmlReader does not trim either.
QString untrimmedChildText(const QDomElement& parent, const QString& tagName)
{
    const QDomElement child = parent.firstChildElement(tagName);
    return child.isNull() ? QString() : child.text();
}

/// \brief The TMDB collection id of a `<set>` element, or NoId.
/// \details Only `<uniqueid type="tmdb">`: unlike the movie NFO, this file is MediaElch's
///          own, so there are no historical spellings to read.  Kodi ignores the element.
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

    // Both read the same way: trimming only one of them would make every set whose name
    // carries stray whitespace look like a set-file-only rename.
    const QString originalTitle = untrimmedChildText(setElement, "originaltitle");
    const QString title = untrimmedChildText(setElement, "title");
    // Kodi 22 displays <title> and matches on <originaltitle>; keeping both is what stops a
    // reload from undoing the last set-file-only rename.  Only where there is something to
    // diverge from: a file with no <originaltitle> is named from its <title> by setNameOf().
    m_set.setTitle(originalTitle.isEmpty() ? QString() : title);

    m_set.setOverview(childText(setElement, "overview"));
    m_set.setTmdbId(tmdbIdOf(setElement));

    // <art> is deliberately not read: a set's artwork is the image files in the same folder,
    // which Kodi 19-22 all read first, so <art> would be a second source of truth for them.

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
    // A file with only <title> was not written by MediaElch; take it rather than ignoring
    // the set altogether.
    return untrimmedChildText(setElement, "title");
}

} // namespace kodi
} // namespace mediaelch
