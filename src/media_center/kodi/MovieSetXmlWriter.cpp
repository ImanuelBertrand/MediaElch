#include "media_center/kodi/MovieSetXmlWriter.h"

#include "data/movie/MovieSet.h"

#include <QXmlStreamWriter>

namespace mediaelch {
namespace kodi {

MovieSetXmlWriter::MovieSetXmlWriter(const MovieSet& set) : m_set{set}
{
}

QByteArray MovieSetXmlWriter::getMovieSetXml() const
{
    QByteArray xmlContent;
    QXmlStreamWriter xml(&xmlContent);
    xml.setAutoFormatting(true);
    xml.writeStartDocument("1.0", true);

    xml.writeStartElement("set");

    // <title> is what Kodi 22 displays, <originaltitle> what it matches on, and the latter
    // has to be byte-identical to the <set><name> in every member movie's NFO.  The two part
    // company after a set-file-only rename, and this file is the only place that can hold it.
    xml.writeTextElement("title", m_set.displayName());
    xml.writeTextElement("originaltitle", m_set.name());

    // Never written empty: XMLUtils::GetString() returns true for an existing-but-empty
    // element, so to Kodi an empty <overview> is a value and it blanks the set's overview.
    if (!m_set.overview().isEmpty()) {
        xml.writeTextElement("overview", m_set.overview());
    }

    // Kodi ignores unknown children of <set>, so the collection's id rides along here for
    // MediaElch to read back (#2012).
    if (m_set.tmdbId().isValid()) {
        xml.writeStartElement("uniqueid");
        xml.writeAttribute("type", "tmdb");
        xml.writeCharacters(m_set.tmdbId().toString());
        xml.writeEndElement();
    }

    // No <art>: the set's artwork is the image files in this same folder, which every Kodi
    // from 19 to 22 reads first.

    xml.writeEndElement();
    xml.writeEndDocument();

    return xmlContent;
}

} // namespace kodi
} // namespace mediaelch
