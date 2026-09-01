#include "test/test_helpers.h"

#include "data/movie/MovieSet.h"
#include "globals/Manager.h"
#include "media/Path.h"
#include "media_center/MediaCenterInterface.h"
#include "media_center/kodi/MovieSetXmlReader.h"
#include "media_center/kodi/MovieSetXmlWriter.h"
#include "settings/Settings.h"
#include "test/helpers/resource_dir.h"

#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
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

namespace {

/// \brief Points the movie set information folder somewhere and puts it back afterwards.
/// \details Settings is a singleton shared by every test in this binary.
class MovieSetFolderGuard
{
public:
    MovieSetFolderGuard() :
        m_type{Settings::instance()->movieSetArtworkType()}, m_dir{Settings::instance()->movieSetArtworkDirectory()}
    {
    }
    ~MovieSetFolderGuard()
    {
        Settings::instance()->setMovieSetArtworkType(m_type);
        Settings::instance()->setMovieSetArtworkDirectory(m_dir);
    }
    MovieSetFolderGuard(const MovieSetFolderGuard&) = delete;
    MovieSetFolderGuard& operator=(const MovieSetFolderGuard&) = delete;

    static void useFolder(const QDir& dir)
    {
        Settings::instance()->setMovieSetArtworkType(MovieSetArtworkType::SeparateArtworkFolder);
        Settings::instance()->setMovieSetArtworkDirectory(mediaelch::DirectoryPath(dir));
    }

private:
    MovieSetArtworkType m_type;
    mediaelch::DirectoryPath m_dir;
};

/// \brief An empty temporary movie set information folder.
QDir emptyMsif(const QString& name)
{
    QDir dir = test::makeTempDir("movie_set/" + name);
    dir.removeRecursively();
    QDir().mkpath(dir.absolutePath());
    return dir;
}

} // namespace

TEST_CASE("Movie set records on disk", "[data][movie][movie_set][kodi][nfo]")
{
    const MovieSetFolderGuard guard;
    MediaCenterInterface* mediaCenter = Manager::instance()->mediaCenterInterface();

    SECTION("Records are off in the artwork-next-to-movies layout")
    {
        // There is no per-set folder in that layout, so there is nowhere a `set.nfo`
        // could go that Kodi would read.  Sets are read-only; that is the design.
        Settings::instance()->setMovieSetArtworkType(MovieSetArtworkType::ArtworkNextToMovies);
        CHECK_FALSE(mediaCenter->movieSetRecordsEnabled());
        CHECK(mediaCenter->movieSetsWithRecord().isEmpty());

        MovieSet set("Alien Collection");
        set.setOverview("Ripley versus the Alien.");
        CHECK_FALSE(mediaCenter->saveMovieSet(set));
    }

    SECTION("Records are off when the folder was never chosen")
    {
        // The hazard this guards.  DirectoryPath's default constructor leaves isValid()
        // false around a default QDir, whose absolutePath() is the process's current
        // working directory, and movieSetFileName() never asks.  Selecting the separate
        // folder without choosing one must not scatter files into whatever directory
        // MediaElch was started from.
        Settings::instance()->setMovieSetArtworkType(MovieSetArtworkType::SeparateArtworkFolder);
        Settings::instance()->setMovieSetArtworkDirectory(mediaelch::DirectoryPath());
        REQUIRE_FALSE(Settings::instance()->movieSetArtworkDirectory().isValid());

        CHECK_FALSE(mediaCenter->movieSetRecordsEnabled());

        MovieSet set("Alien Collection");
        CHECK_FALSE(mediaCenter->saveMovieSet(set));
        CHECK_FALSE(mediaCenter->loadMovieSet(set));
        CHECK(mediaCenter->movieSetsWithRecord().isEmpty());
        CHECK_FALSE(QFileInfo::exists(QDir::current().absoluteFilePath("Alien Collection/set.nfo")));
    }

    SECTION("A record is written, listed, read back and removed")
    {
        const QDir msif = emptyMsif("roundtrip");
        MovieSetFolderGuard::useFolder(msif);
        REQUIRE(mediaCenter->movieSetRecordsEnabled());

        MovieSet written("Alien Collection");
        written.setOverview("Ripley versus the Alien.");
        written.setTmdbId(TmdbId("8091"));
        REQUIRE(mediaCenter->saveMovieSet(written));
        CHECK(written.hasRecord());
        // Saving is the one moment at which a set and its file agree, so it is the
        // clearing edge for the flag nothing used to clear.
        CHECK_FALSE(written.hasChanged());
        CHECK(QFileInfo::exists(msif.absoluteFilePath("Alien Collection/set.nfo")));

        CHECK(mediaCenter->movieSetsWithRecord() == QStringList{"Alien Collection"});

        MovieSet read("Alien Collection");
        REQUIRE(mediaCenter->loadMovieSet(read));
        CHECK(read.overview() == "Ripley versus the Alien.");
        CHECK(read.tmdbId() == TmdbId("8091"));
        CHECK_FALSE(read.hasChanged());

        REQUIRE(mediaCenter->removeMovieSetRecord("Alien Collection"));
        CHECK_FALSE(QFileInfo::exists(msif.absoluteFilePath("Alien Collection/set.nfo")));
        CHECK(mediaCenter->movieSetsWithRecord().isEmpty());
        // Only the record.  The folder and its artwork are not MediaElch's to delete.
        CHECK(QFileInfo::exists(msif.absoluteFilePath("Alien Collection")));
    }

    SECTION("A folder with artwork but no record is not a set")
    {
        // The record is what makes a set exist in its own right.  Treating a pile of
        // images as one would resurrect every set a user ever deliberately removed.
        const QDir msif = emptyMsif("artonly");
        MovieSetFolderGuard::useFolder(msif);
        REQUIRE(QDir().mkpath(msif.absoluteFilePath("Predator Collection")));
        QFile poster(msif.absoluteFilePath("Predator Collection/poster.jpg"));
        REQUIRE(poster.open(QIODevice::WriteOnly));
        poster.close();

        CHECK(mediaCenter->movieSetsWithRecord().isEmpty());
    }

    SECTION("The folder name is legalised; the set's name is not")
    {
        // Kodi derives the folder from the set name with MakeLegalFileName, which is
        // lossy, so the name has to come back out of the file and not out of the folder
        // -- it must match the member NFOs' <set><name> byte for byte.
        const QDir msif = emptyMsif("legalise");
        MovieSetFolderGuard::useFolder(msif);

        MovieSet set("Mission: Impossible Collection");
        REQUIRE(mediaCenter->saveMovieSet(set));
        CHECK(QFileInfo::exists(msif.absoluteFilePath("Mission_ Impossible Collection/set.nfo")));
        CHECK(mediaCenter->movieSetsWithRecord() == QStringList{"Mission: Impossible Collection"});
    }
}
