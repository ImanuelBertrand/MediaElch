#include "test/test_helpers.h"

#include "data/movie/MovieSet.h"
#include "media_center/kodi/MovieSetXmlReader.h"
#include "media_center/kodi/MovieSetXmlWriter.h"
#include "test/helpers/resource_dir.h"

#include <QDomDocument>
#include <memory>

using namespace mediaelch;

namespace {

/// \brief Parses \p xml into a set called \p name.  The name is not read from the file.
std::unique_ptr<MovieSet> parseSet(const QString& xml, const QString& name = "Alien Collection")
{
    auto set = std::make_unique<MovieSet>(name);
    QDomDocument doc;
    doc.setContent(xml);
    kodi::MovieSetXmlReader reader(*set);
    REQUIRE(reader.parseNfoDom(doc));
    return set;
}

QString setNameOf(const QString& xml)
{
    QDomDocument doc;
    doc.setContent(xml);
    return kodi::MovieSetXmlReader::setNameOf(doc);
}

} // namespace

TEST_CASE("Movie set record round trip", "[data][movie][movie_set][kodi][nfo]")
{
    SECTION("Reads and writes back a set.nfo unchanged")
    {
        const QString filename = "movie_set/kodi_v22_set_alien_collection.nfo";
        CAPTURE(filename);

        MovieSet set("Alien Collection");
        QDomDocument doc;
        doc.setContent(test::readResourceFile(filename));
        kodi::MovieSetXmlReader reader(set);
        REQUIRE(reader.parseNfoDom(doc));

        CHECK(set.overview() == "A science fiction horror film franchise, focusing on Lieutenant Ellen Ripley.");
        CHECK(set.tmdbId() == TmdbId("8091"));

        const kodi::MovieSetXmlWriter writer(set);
        test::compareXmlAgainstResourceFile(QString::fromUtf8(writer.getMovieSetXml()).trimmed(), filename);
    }

    SECTION("<originaltitle> is the join key and equals <title> on write")
    {
        // The member NFOs carry <set><name>; this file carries <title> and
        // <originaltitle>, and Kodi 22 matches on <originaltitle>.  Write them apart
        // and Kodi keys the set's row off a name no movie NFO mentions.
        MovieSet set("Alien Collection");
        const kodi::MovieSetXmlWriter writer(set);
        const QString xml = QString::fromUtf8(writer.getMovieSetXml());

        CHECK_THAT(xml, Contains("<title>Alien Collection</title>"));
        CHECK_THAT(xml, Contains("<originaltitle>Alien Collection</originaltitle>"));
        // Not <name>, which is the movie NFO's spelling for the same thing.
        CHECK_THAT(xml, ContainsNot("<name>"));
    }

    SECTION("An empty overview is never written")
    {
        // D2a: XMLUtils::GetString() returns true for an existing-but-empty element, so
        // an empty <overview> is a value to Kodi and blanks the set's stored overview.
        MovieSet set("Alien Collection");
        const kodi::MovieSetXmlWriter writer(set);
        CHECK_THAT(QString::fromUtf8(writer.getMovieSetXml()), ContainsNot("<overview>"));
    }

    SECTION("An absent TMDB id is not written")
    {
        MovieSet set("Alien Collection");
        const kodi::MovieSetXmlWriter writer(set);
        CHECK_THAT(QString::fromUtf8(writer.getMovieSetXml()), ContainsNot("uniqueid"));
    }
}

TEST_CASE("Movie set record reader", "[data][movie][movie_set][kodi][nfo]")
{
    SECTION("Reads the overview and the TMDB id")
    {
        const auto set = parseSet(R"(<set>
            <title>Alien Collection</title>
            <originaltitle>Alien Collection</originaltitle>
            <overview>Ripley versus the Alien.</overview>
            <uniqueid type="tmdb">8091</uniqueid>
        </set>)");
        CHECK(set->overview() == "Ripley versus the Alien.");
        CHECK(set->tmdbId() == TmdbId("8091"));
    }

    SECTION("Ignores an id of another type")
    {
        const auto set = parseSet(R"(<set>
            <originaltitle>Alien Collection</originaltitle>
            <uniqueid type="imdb">tt0078748</uniqueid>
        </set>)");
        CHECK(set->tmdbId() == TmdbId::NoId);
    }

    SECTION("Never renames the set")
    {
        // A <title> that has moved away from <originaltitle> is a set-file-only rename,
        // which is D3a's business.  MediaElch has one name per set, and it is the join
        // key the member movies use.
        const auto set = parseSet(R"(<set>
            <title>The Alien Saga</title>
            <originaltitle>Alien Collection</originaltitle>
        </set>)");
        CHECK(set->name() == "Alien Collection");
    }

    SECTION("Reading is not an edit")
    {
        // Every setter the reader calls marks the set as needing to be saved, but what
        // it read *is* what is on disk.  KodiXml::loadMovieSet() clears the flag; a
        // reader used on its own leaves it set, which is what this pins.
        MovieSet set("Alien Collection");
        QDomDocument doc;
        doc.setContent(QStringLiteral("<set><originaltitle>Alien Collection</originaltitle></set>"));
        kodi::MovieSetXmlReader reader(set);
        REQUIRE(reader.parseNfoDom(doc));
        CHECK(set.movies().isEmpty());
    }

    SECTION("Rejects a document that is not a <set>")
    {
        MovieSet set("Alien Collection");
        QDomDocument doc;
        doc.setContent(QStringLiteral("<movie><title>Alien</title></movie>"));
        kodi::MovieSetXmlReader reader(set);
        CHECK_FALSE(reader.parseNfoDom(doc));
    }
}

TEST_CASE("Movie set record names the set it belongs to", "[data][movie][movie_set][kodi][nfo]")
{
    SECTION("Takes the join key, not the displayed title")
    {
        CHECK(setNameOf(R"(<set><title>The Alien Saga</title>
                           <originaltitle>Alien Collection</originaltitle></set>)")
              == "Alien Collection");
    }

    SECTION("Falls back to <title> for a file that has no join key")
    {
        CHECK(setNameOf("<set><title>Alien Collection</title></set>") == "Alien Collection");
    }

    SECTION("Names no set for a document that is not a <set>")
    {
        CHECK(setNameOf("<movie><title>Alien</title></movie>").isEmpty());
    }
}
