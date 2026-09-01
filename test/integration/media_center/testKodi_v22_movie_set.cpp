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
#include <QFileDevice>
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

    SECTION("The reader alone leaves the set marked as changed")
    {
        // Every setter the reader calls marks the set as needing to be saved, and the
        // reader does not undo that -- KodiXml::loadMovieSet() is what clears the flag,
        // because it is the one that knows the values came off the disk.  The fixture
        // has to carry a value for this to mean anything: MovieSet's setters return
        // early when the value does not change, so a record with neither an overview nor
        // an id raises the flag under no implementation at all.
        MovieSet set("Alien Collection");
        REQUIRE_FALSE(set.hasChanged());
        QDomDocument doc;
        doc.setContent(QStringLiteral(
            R"(<set><originaltitle>Alien Collection</originaltitle><overview>Ripley.</overview></set>)"));
        kodi::MovieSetXmlReader reader(set);
        REQUIRE(reader.parseNfoDom(doc));
        CHECK(set.overview() == "Ripley.");
        CHECK(set.hasChanged());
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

    SECTION("A record belongs to the set it names, not to the folder it sits in")
    {
        // The path is derived from the set name through Kodi's legalisation, which is
        // lossy: "Mission: Impossible" and "Mission_ Impossible" resolve to one folder.
        // Only one of them owns the record in it, and every path has to agree about
        // which -- otherwise a set flips between having a record and not having one from
        // one reload to the next, and Delete Movie Set removes another set's file.
        const QDir msif = emptyMsif("collision");
        MovieSetFolderGuard::useFolder(msif);

        MovieSet owner("Mission: Impossible Collection");
        owner.setOverview("Ethan Hunt runs.");
        REQUIRE(mediaCenter->saveMovieSet(owner));

        // The listing reports the set the file names, and only that one.
        CHECK(mediaCenter->movieSetsWithRecord() == QStringList{"Mission: Impossible Collection"});

        // The other name resolves to the same file and must not be given it.
        MovieSet lodger("Mission_ Impossible Collection");
        CHECK_FALSE(mediaCenter->loadMovieSet(lodger));
        CHECK(lodger.overview().isEmpty());

        // And must not be able to delete it.
        CHECK_FALSE(mediaCenter->removeMovieSetRecord("Mission_ Impossible Collection"));
        CHECK(QFileInfo::exists(msif.absoluteFilePath("Mission_ Impossible Collection/set.nfo")));

        // The set that does own it still can.
        CHECK(mediaCenter->removeMovieSetRecord("Mission: Impossible Collection"));
        CHECK_FALSE(QFileInfo::exists(msif.absoluteFilePath("Mission_ Impossible Collection/set.nfo")));
    }

    SECTION("Saving a set never overwrites another set's record")
    {
        // The writer is a path to this file too, and for two rounds it was the one that
        // was not asking the question.  "Alien Collection" and "Alien Collection " share
        // a folder, because legalisation chops the trailing space -- and the padded name
        // is not exotic: the movie NFO reader does not trim `<set><name>` either, so a
        // sloppy member NFO produces it as a set of its own.
        const QDir msif = emptyMsif("overwrite");
        MovieSetFolderGuard::useFolder(msif);

        MovieSet owner("Alien Collection");
        owner.setOverview("Ripley versus the Alien.");
        owner.setTmdbId(TmdbId("8091"));
        REQUIRE(mediaCenter->saveMovieSet(owner));

        MovieSet lodger("Alien Collection ");
        lodger.setOverview("Not Ripley at all.");
        CHECK_FALSE(mediaCenter->saveMovieSet(lodger));
        CHECK_FALSE(lodger.hasRecord());

        // The owner's record is untouched -- overview, id and all.
        MovieSet reread("Alien Collection");
        REQUIRE(mediaCenter->loadMovieSet(reread));
        CHECK(reread.overview() == "Ripley versus the Alien.");
        CHECK(reread.tmdbId() == TmdbId("8091"));
        CHECK(mediaCenter->movieSetsWithRecord() == QStringList{"Alien Collection"});
    }

    SECTION("Saving still creates a record where there is no file")
    {
        // The writer's guard is "a file is there and it names another set", not "the file
        // names this set".  Demanding a match would make the first record for any set
        // impossible to write, which would disable the feature rather than protect it.
        const QDir msif = emptyMsif("firstwrite");
        MovieSetFolderGuard::useFolder(msif);

        MovieSet set("Alien Collection");
        CHECK(mediaCenter->saveMovieSet(set));
        CHECK(QFileInfo::exists(msif.absoluteFilePath("Alien Collection/set.nfo")));
        // And saving the same set again is an update, not a collision.
        set.setOverview("Ripley versus the Alien.");
        CHECK(mediaCenter->saveMovieSet(set));
        MovieSet reread("Alien Collection");
        REQUIRE(mediaCenter->loadMovieSet(reread));
        CHECK(reread.overview() == "Ripley versus the Alien.");
    }

    SECTION("A record that cannot be read is not removed")
    {
        // Unlinking needs write permission on the *directory* and nothing on the file, so
        // an unreadable `set.nfo` is perfectly deletable.  Skipping the ownership check
        // when the open fails therefore deletes a file whose owner was never established
        // -- and reports success for it.
        const QDir msif = emptyMsif("unreadable");
        MovieSetFolderGuard::useFolder(msif);

        MovieSet set("Alien Collection");
        REQUIRE(mediaCenter->saveMovieSet(set));
        const QString fileName = msif.absoluteFilePath("Alien Collection/set.nfo");
        REQUIRE(QFileInfo::exists(fileName));

        QFile record(fileName);
        REQUIRE(record.setPermissions(QFileDevice::Permissions()));

        QFile probe(fileName);
        if (probe.open(QIODevice::ReadOnly)) {
            // The platform does not enforce the permissions -- Windows, or running as
            // root.  There is no unreadable file to test with, so there is nothing here
            // to assert; the guard is about the case where the open genuinely fails.
            probe.close();
            WARN("Skipped: this file system does not enforce the permission change");
        } else {
            CHECK_FALSE(mediaCenter->removeMovieSetRecord("Alien Collection"));
            CHECK(QFileInfo::exists(fileName));
        }

        // Restore, or the next run of emptyMsif() cannot clear the folder.
        record.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }

    SECTION("A record in a hidden folder is seen by the listing as well as the probe")
    {
        // QDir::NoDotAndDotDot drops only "." and ".."; without QDir::Hidden a set whose
        // legalised folder starts with a dot is invisible to the listing while
        // loadMovieSet() opens its path directly and finds it.  The set then alternates
        // between surviving its last member and not, which is the very flip-flop the one
        // question was supposed to close.
        const QDir msif = emptyMsif("hidden");
        MovieSetFolderGuard::useFolder(msif);

        MovieSet set(".hack Collection");
        set.setOverview("Bandai's franchise.");
        REQUIRE(mediaCenter->saveMovieSet(set));
        REQUIRE(QFileInfo::exists(msif.absoluteFilePath(".hack Collection/set.nfo")));

        CHECK(mediaCenter->movieSetsWithRecord() == QStringList{".hack Collection"});

        MovieSet reread(".hack Collection");
        CHECK(mediaCenter->loadMovieSet(reread));
    }

    SECTION("A set whose name legalises away to nothing gets no record at all")
    {
        // "." and "..." and a run of spaces are all accepted by the sets tab's rename
        // field and all legalise to the empty string, which would build "<msif>//set.nfo"
        // and drop the record into the folder's root -- where the listing, which descends
        // into subfolders, would never see it, while the direct probe would.  Same split
        // answer as the hidden folder, so the same refusal.
        const QDir msif = emptyMsif("nameless");
        MovieSetFolderGuard::useFolder(msif);

        MovieSet set("...");
        CHECK_FALSE(mediaCenter->saveMovieSet(set));
        CHECK_FALSE(QFileInfo::exists(msif.absoluteFilePath("set.nfo")));
        CHECK_FALSE(mediaCenter->loadMovieSet(set));
        CHECK_FALSE(mediaCenter->removeMovieSetRecord("..."));
    }

    SECTION("A record in a folder its own name does not resolve to is ignored")
    {
        // Reporting it would name a set whose every write path looks somewhere else:
        // loadMovieSet() would not find it, saveMovieSet() would write a second file in
        // the right folder and removeMovieSetRecord() would remove neither.
        const QDir msif = emptyMsif("misfiled");
        MovieSetFolderGuard::useFolder(msif);
        REQUIRE(QDir().mkpath(msif.absoluteFilePath("Alien")));
        QFile record(msif.absoluteFilePath("Alien/set.nfo"));
        REQUIRE(record.open(QIODevice::WriteOnly));
        record.write(R"(<set><originaltitle>Alien Collection</originaltitle></set>)");
        record.close();

        CHECK(mediaCenter->movieSetsWithRecord().isEmpty());
    }

    SECTION("A name whose whitespace matters round-trips")
    {
        // The name is a join key that has to be byte-identical to the member NFOs'
        // <set><name>, so the reader does not trim it.  Trimming would report this set
        // under one spelling and look it up under another, because Kodi's legalisation
        // chops only *trailing* whitespace.
        const QDir msif = emptyMsif("whitespace");
        MovieSetFolderGuard::useFolder(msif);

        MovieSet set(" Alien Collection");
        REQUIRE(mediaCenter->saveMovieSet(set));
        CHECK(mediaCenter->movieSetsWithRecord() == QStringList{" Alien Collection"});

        MovieSet read(" Alien Collection");
        CHECK(mediaCenter->loadMovieSet(read));
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
