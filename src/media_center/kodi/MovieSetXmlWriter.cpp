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

    // <title> is what Kodi 22 displays, <originaltitle> is what it matches on, and it
    // has to be byte-identical to the <set><name> in every member movie's NFO.  Both
    // are the set's one name until D3a makes a set-file-only rename possible.
    xml.writeTextElement("title", m_set.name());
    xml.writeTextElement("originaltitle", m_set.name());

    // Never written empty (D2a).  XMLUtils::GetString() returns true for an
    // existing-but-empty element, so an empty <overview> is a value to Kodi, not an
    // absence, and it blanks a populated set overview in the database.
    if (!m_set.overview().isEmpty()) {
        xml.writeTextElement("overview", m_set.overview());
    }

    // Kodi ignores unknown children of <set>, so the collection's id rides along here
    // for MediaElch to read back (#2012).  CSetInfoTag has no id field at all.
    if (m_set.tmdbId().isValid()) {
        xml.writeStartElement("uniqueid");
        xml.writeAttribute("type", "tmdb");
        xml.writeCharacters(m_set.tmdbId().toString());
        xml.writeEndElement();
    }

    // No <art>: the set's artwork is the image files in this same folder, which every
    // Kodi from 19 to 22 reads first.  See MovieSetXmlReader::parseNfoDom().

    xml.writeEndElement();
    xml.writeEndDocument();

    return xmlContent;
}

} // namespace kodi
} // namespace mediaelch
