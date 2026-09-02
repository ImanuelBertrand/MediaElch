#include "test/test_helpers.h"

#include "data/movie/Movie.h"
#include "data/movie/MovieSet.h"
#include "data/movie/MovieSetInfo.h"
#include "globals/Manager.h"
#include "media/Path.h"
#include "media_center/MediaCenterInterface.h"
#include "media_center/kodi/MovieSetXmlReader.h"
#include "media_center/kodi/MovieSetXmlWriter.h"
#include "model/MovieModel.h"
#include "model/MovieSetModel.h"
#include "settings/DataFile.h"
#include "settings/Settings.h"
#include "test/helpers/movie_set_settings.h"
#include "test/helpers/resource_dir.h"

#include <QApplication>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QImage>
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

    SECTION("<originaltitle> is the join key and equals <title> until a rename")
    {
        // Kodi 22 matches on <originaltitle>, which is the member NFOs' <set><name>.
        MovieSet set("Alien Collection");
        const kodi::MovieSetXmlWriter writer(set);
        const QString xml = QString::fromUtf8(writer.getMovieSetXml());

        CHECK_THAT(xml, Contains("<title>Alien Collection</title>"));
        CHECK_THAT(xml, Contains("<originaltitle>Alien Collection</originaltitle>"));
        // Not <name>, which is the movie NFO's spelling for the same thing.
        CHECK_THAT(xml, ContainsNot("<name>"));
    }

    SECTION("A set-file-only rename writes the two apart, key first")
    {
        // <title> is what Kodi 22 displays; <originaltitle> is still what it matches on.
        MovieSet set("Alien Collection");
        set.setTitle("The Alien Saga");
        const kodi::MovieSetXmlWriter writer(set);
        const QString xml = QString::fromUtf8(writer.getMovieSetXml());

        CHECK_THAT(xml, Contains("<title>The Alien Saga</title>"));
        CHECK_THAT(xml, Contains("<originaltitle>Alien Collection</originaltitle>"));
    }

    SECTION("An empty overview is never written")
    {
        // XMLUtils::GetString() returns true for an existing-but-empty element, so an empty
        // <overview> is a value to Kodi and blanks the set's stored overview.
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

    SECTION("Never moves the set's key, and keeps the display title beside it")
    {
        // A <title> that has moved away from <originaltitle> is a set-file-only rename, and
        // dropping the title would make every reload undo it.
        const auto set = parseSet(R"(<set>
            <title>The Alien Saga</title>
            <originaltitle>Alien Collection</originaltitle>
        </set>)");
        CHECK(set->name() == "Alien Collection");
        CHECK(set->title() == "The Alien Saga");
        CHECK(set->displayName() == "The Alien Saga");
    }

    SECTION("A record with no divergence has no display title of its own")
    {
        const auto set = parseSet(R"(<set>
            <title>Alien Collection</title>
            <originaltitle>Alien Collection</originaltitle>
        </set>)");
        CHECK(set->title().isEmpty());
        CHECK(set->displayName() == "Alien Collection");
    }

    SECTION("A record with only <title> is a name, not a rename")
    {
        // setNameOf() falls back to <title> here, so the key is already that string.
        const auto set = parseSet("<set><title>Alien Collection</title></set>");
        CHECK(set->name() == "Alien Collection");
        CHECK(set->title().isEmpty());
        CHECK(set->displayName() == "Alien Collection");
    }

    SECTION("The reader alone leaves the set marked as changed")
    {
        // The reader does not clear the flag its setters raise; KodiXml::loadMovieSet() does,
        // knowing the values came off the disk.  The fixture must carry a value, since
        // MovieSet's setters return early when nothing changes.
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

TEST_CASE("Movie set record rename detection", "[data][movie][movie_set][kodi][nfo]")
{
    SECTION("MediaElch's own record is never read as a rename")
    {
        // The writer emits both from one name unless there is a real divergence, so reading one
        // trimmed and the other not would make every whitespace-carrying name look renamed.
        MovieSet set(" Alien Collection");
        QDomDocument doc;
        doc.setContent(QString::fromUtf8(kodi::MovieSetXmlWriter(set).getMovieSetXml()));
        kodi::MovieSetXmlReader reader(set);
        REQUIRE(reader.parseNfoDom(doc));

        CHECK(set.title().isEmpty());
        CHECK(set.displayName() == " Alien Collection");
    }

    SECTION("A record that really was renamed in the set file still says so")
    {
        MovieSet set("Alien Collection");
        QDomDocument doc;
        doc.setContent(
            QStringLiteral("<set><title>The Alien Saga</title><originaltitle>Alien Collection</originaltitle></set>"));
        kodi::MovieSetXmlReader reader(set);
        REQUIRE(reader.parseNfoDom(doc));

        CHECK(set.name() == "Alien Collection");
        CHECK(set.title() == "The Alien Saga");
    }

    SECTION("Whitespace alone is a real divergence and survives untrimmed")
    {
        // Compared untrimmed, so this is a rename and the title keeps the space that makes it one.
        MovieSet set("Alien Collection");
        QDomDocument doc;
        doc.setContent(
            QStringLiteral("<set><title>Alien Collection </title><originaltitle>Alien Collection</originaltitle></set>"));
        kodi::MovieSetXmlReader reader(set);
        REQUIRE(reader.parseNfoDom(doc));

        CHECK(set.name() == "Alien Collection");
        CHECK(set.title() == "Alien Collection ");
    }

    SECTION("A set-file-only rename round-trips through the writer and back")
    {
        // The point of holding both strings: a reload must not undo the rename.
        MovieSet set("Alien Collection");
        set.setTitle("The Alien Saga");

        QDomDocument doc;
        doc.setContent(QString::fromUtf8(kodi::MovieSetXmlWriter(set).getMovieSetXml()));

        MovieSet readBack("Alien Collection");
        kodi::MovieSetXmlReader reader(readBack);
        REQUIRE(reader.parseNfoDom(doc));

        CHECK(readBack.name() == "Alien Collection");
        CHECK(readBack.title() == "The Alien Saga");
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

/// \brief The whole of the file at \p fileName as text.
QString readFile(const QString& fileName)
{
    QFile file(fileName);
    REQUIRE(file.open(QIODevice::ReadOnly));
    return QString::fromUtf8(file.readAll());
}

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
        // No per-set folder means nowhere a `set.nfo` could go that Kodi would read.
        Settings::instance()->setMovieSetArtworkType(MovieSetArtworkType::ArtworkNextToMovies);
        CHECK_FALSE(mediaCenter->movieSetRecordsEnabled());
        CHECK(mediaCenter->movieSetsWithRecord().isEmpty());

        MovieSet set("Alien Collection");
        set.setOverview("Ripley versus the Alien.");
        CHECK_FALSE(mediaCenter->saveMovieSet(set));
    }

    SECTION("Records are off when the folder was never chosen")
    {
        // A default DirectoryPath wraps a default QDir, whose absolutePath() is the process's
        // working directory, so selecting the separate folder without choosing one would
        // scatter files wherever MediaElch was started.  This predicate covers
        // movieSetsWithRecord(), which lists the folder rather than going through
        // movieSetFileName().
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
        // Saving is the one moment at which a set and its file agree, so it clears hasChanged().
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
        // Treating images as a record would resurrect every set a user deliberately removed.
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
        // Legalisation is lossy -- "Mission: Impossible" and "Mission_ Impossible" resolve to
        // one folder -- so every path has to agree on which of them owns the record.
        const QDir msif = emptyMsif("collision");
        MovieSetFolderGuard::useFolder(msif);

        MovieSet owner("Mission: Impossible Collection");
        owner.setOverview("Ethan Hunt runs.");
        REQUIRE(mediaCenter->saveMovieSet(owner));

        CHECK(mediaCenter->movieSetsWithRecord() == QStringList{"Mission: Impossible Collection"});

        // The other name resolves to the same file and must not be given it.
        MovieSet lodger("Mission_ Impossible Collection");
        CHECK_FALSE(mediaCenter->loadMovieSet(lodger));
        CHECK(lodger.overview().isEmpty());

        CHECK_FALSE(mediaCenter->removeMovieSetRecord("Mission_ Impossible Collection"));
        CHECK(QFileInfo::exists(msif.absoluteFilePath("Mission_ Impossible Collection/set.nfo")));

        CHECK(mediaCenter->removeMovieSetRecord("Mission: Impossible Collection"));
        CHECK_FALSE(QFileInfo::exists(msif.absoluteFilePath("Mission_ Impossible Collection/set.nfo")));
    }

    SECTION("Saving a set never overwrites another set's record")
    {
        // "Alien Collection" and "Alien Collection " share a folder, and the padded name is not
        // exotic: the movie NFO reader does not trim `<set><name>` either.
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

        MovieSet reread("Alien Collection");
        REQUIRE(mediaCenter->loadMovieSet(reread));
        CHECK(reread.overview() == "Ripley versus the Alien.");
        CHECK(reread.tmdbId() == TmdbId("8091"));
        CHECK(mediaCenter->movieSetsWithRecord() == QStringList{"Alien Collection"});
    }

    SECTION("Saving still creates a record where there is no file")
    {
        // The guard is "a file is there and it names another set"; demanding a positive match
        // would make the first record for any set impossible to write.
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
        // An unreadable `set.nfo` is still deletable -- unlinking needs the directory, not the
        // file -- so skipping the ownership check on a failed open deletes an unowned file.
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
            // The platform does not enforce the permissions (Windows, or as root).
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
        // Without QDir::Hidden a dot-folder is invisible to the listing while loadMovieSet()
        // opens its path directly and finds it, so the two answers disagree.
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
        // ".", "..." and a run of spaces are all accepted by the rename field and all legalise
        // to nothing, dropping the record where the listing never looks and the probe does.
        const QDir msif = emptyMsif("nameless");
        MovieSetFolderGuard::useFolder(msif);

        MovieSet set("...");
        CHECK_FALSE(mediaCenter->saveMovieSet(set));
        CHECK_FALSE(QFileInfo::exists(msif.absoluteFilePath("set.nfo")));
        CHECK_FALSE(mediaCenter->loadMovieSet(set));
        CHECK_FALSE(mediaCenter->removeMovieSetRecord("..."));
    }

    SECTION("A record that names nobody: the write takes it, the removal will not")
    {
        // The write and the removal disagree on purpose.  A `set.nfo` that names no set is
        // nobody's record, so the write has to claim it or that folder is permanently
        // unwritable; the removal must still refuse it, because deleting a file it cannot show
        // to belong to this set is the fail-open.
        const QDir msif = emptyMsif("nameless");
        MovieSetFolderGuard::useFolder(msif);
        REQUIRE(QDir().mkpath(msif.absoluteFilePath("Alien Collection")));
        const QString fileName = msif.absoluteFilePath("Alien Collection/set.nfo");
        QFile orphan(fileName);
        REQUIRE(orphan.open(QIODevice::WriteOnly));
        orphan.write("<set><overview>Belongs to no one.</overview></set>");
        orphan.close();

        // Nobody's record, so nobody may have it deleted.
        CHECK_FALSE(mediaCenter->removeMovieSetRecord("Alien Collection"));
        CHECK(QFileInfo::exists(fileName));

        // But a set whose folder this is may claim it.
        MovieSet set("Alien Collection");
        set.setOverview("Ripley versus the Alien.");
        CHECK(mediaCenter->saveMovieSet(set));
        CHECK(mediaCenter->movieSetsWithRecord() == QStringList{"Alien Collection"});
        CHECK(mediaCenter->removeMovieSetRecord("Alien Collection"));
    }

    SECTION("A record in a folder its own name does not resolve to is ignored")
    {
        // Reporting it would name a set whose every write path looks in a different folder.
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
        // The name is a join key that must be byte-identical to the member NFOs' <set><name>,
        // so trimming it would report the set under one spelling and look it up under another.
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
        // MakeLegalFileName is lossy, so the name comes out of the file, not the folder.
        const QDir msif = emptyMsif("legalise");
        MovieSetFolderGuard::useFolder(msif);

        MovieSet set("Mission: Impossible Collection");
        REQUIRE(mediaCenter->saveMovieSet(set));
        CHECK(QFileInfo::exists(msif.absoluteFilePath("Mission_ Impossible Collection/set.nfo")));
        CHECK(mediaCenter->movieSetsWithRecord() == QStringList{"Mission: Impossible Collection"});
    }
}

namespace {

/// \brief Puts one movie with a real file into the library, and takes it out again.
/// \details "Artwork next to movies" resolves through a member movie's folder, so that
///          layout needs a library movie with real files.
class LibraryMovieGuard
{
public:
    LibraryMovieGuard(const QDir& movieDir, const QString& setName)
    {
        REQUIRE(QDir().mkpath(movieDir.absolutePath()));
        QFile file(movieDir.absoluteFilePath("Alien.mkv"));
        REQUIRE(file.open(QIODevice::WriteOnly));
        file.write("not really a movie");
        file.close();

        auto* movie = new Movie(QStringList{file.fileName()}, nullptr);
        movie->setTitle("Alien");
        MovieSetInfo info;
        info.name = setName;
        movie->setSetInfo(info);
        movie->setChanged(false);
        Manager::instance()->movieModel()->addMovie(movie);
    }
    ~LibraryMovieGuard()
    {
        Manager::instance()->movieModel()->clear();
        qApp->processEvents();
        // The set model is a singleton; put it back the way the next test expects it.
        Manager::instance()->movieSetModel()->reload();
    }
    LibraryMovieGuard(const LibraryMovieGuard&) = delete;
    LibraryMovieGuard& operator=(const LibraryMovieGuard&) = delete;
};

} // namespace

TEST_CASE("Movie set artwork paths", "[data][movie][movie_set][kodi][image]")
{
    const MovieSetFolderGuard guard;
    const test::DataFileGuard dataFiles;
    MediaCenterInterface* mediaCenter = Manager::instance()->mediaCenterInterface();

    QImage poster(4, 4, QImage::Format_RGB32);
    poster.fill(Qt::red);

    SECTION("Artwork and records are different questions")
    {
        // Artwork resolves in both layouts; a record only in the movie set information folder.
        // Gating artwork on the record predicate would take it away from the shipping default.
        SECTION("Artwork next to movies: artwork yes, records no")
        {
            Settings::instance()->setMovieSetArtworkType(MovieSetArtworkType::ArtworkNextToMovies);
            CHECK(mediaCenter->movieSetArtworkEnabled());
            CHECK_FALSE(mediaCenter->movieSetRecordsEnabled());
        }

        SECTION("A configured separate folder: both yes")
        {
            MovieSetFolderGuard::useFolder(emptyMsif("artwork_both"));
            CHECK(mediaCenter->movieSetArtworkEnabled());
            CHECK(mediaCenter->movieSetRecordsEnabled());
        }

        SECTION("A separate folder that was never chosen: both no")
        {
            Settings::instance()->setMovieSetArtworkType(MovieSetArtworkType::SeparateArtworkFolder);
            Settings::instance()->setMovieSetArtworkDirectory(mediaelch::DirectoryPath());
            REQUIRE_FALSE(Settings::instance()->movieSetArtworkDirectory().isValid());
            CHECK_FALSE(mediaCenter->movieSetArtworkEnabled());
            CHECK_FALSE(mediaCenter->movieSetRecordsEnabled());
        }
    }

    SECTION("Artwork is not written into the working directory")
    {
        // QDir("").absolutePath() is the process's working directory, so a movieSetFileName()
        // that does not ask isValid() writes into wherever MediaElch was started -- and the
        // folder matters as much as the file, since saveMovieSetPoster() calls mkpath() first.
        const QDir cwd = QDir::current();
        QDir(cwd.absoluteFilePath("Alien Collection")).removeRecursively();

        Settings::instance()->setMovieSetArtworkType(MovieSetArtworkType::SeparateArtworkFolder);
        Settings::instance()->setMovieSetArtworkDirectory(mediaelch::DirectoryPath());
        REQUIRE_FALSE(Settings::instance()->movieSetArtworkDirectory().isValid());

        CHECK_FALSE(mediaCenter->saveMovieSetPoster("Alien Collection", poster));
        CHECK_FALSE(mediaCenter->saveMovieSetBackdrop("Alien Collection", poster));

        CHECK_FALSE(QFileInfo::exists(cwd.absoluteFilePath("Alien Collection")));
    }

    SECTION("Artwork is not read out of the working directory")
    {
        // The read half: such a poster would be displayed as this set's artwork.
        const QDir cwd = QDir::current();
        QDir(cwd.absoluteFilePath("Alien Collection")).removeRecursively();
        REQUIRE(QDir().mkpath(cwd.absoluteFilePath("Alien Collection")));

        // Under every name the reader probes, so the check cannot pass by guessing wrong.
        const QVector<DataFile> posterFiles = Settings::instance()->dataFiles(DataFileType::MovieSetPoster);
        REQUIRE_FALSE(posterFiles.isEmpty());
        for (DataFile dataFile : posterFiles) {
            const QString fileName =
                cwd.absoluteFilePath("Alien Collection/" + dataFile.saveFileName("Alien Collection"));
            REQUIRE(poster.save(fileName, "jpg", 100));
        }

        MovieSetFolderGuard::useFolder(cwd);
        REQUIRE_FALSE(mediaCenter->movieSetPoster("Alien Collection").isNull());

        // The same files must not be found with no folder configured.
        Settings::instance()->setMovieSetArtworkDirectory(mediaelch::DirectoryPath());
        REQUIRE_FALSE(Settings::instance()->movieSetArtworkDirectory().isValid());
        CHECK(mediaCenter->movieSetPoster("Alien Collection").isNull());

        QDir(cwd.absoluteFilePath("Alien Collection")).removeRecursively();
    }

    SECTION("Artwork next to movies still works")
    {
        // Why movieSetArtworkEnabled() is a question of its own: the shipping default resolves
        // through a member movie's folder, so refusing it would be the larger bug.
        QDir movieDir = test::makeTempDir("movie_set/next_to_movies");
        movieDir.removeRecursively();
        const LibraryMovieGuard movie(movieDir, "Alien Collection");

        Settings::instance()->setMovieSetArtworkType(MovieSetArtworkType::ArtworkNextToMovies);
        Settings::instance()->setMovieSetArtworkDirectory(mediaelch::DirectoryPath());
        REQUIRE(mediaCenter->movieSetArtworkEnabled());

        CHECK(mediaCenter->saveMovieSetPoster("Alien Collection", poster));
        CHECK_FALSE(mediaCenter->movieSetPoster("Alien Collection").isNull());
    }

    SECTION("A write that reached the disk says so, and one that did not says so")
    {
        // ELCH_NODISCARD is silent on a virtual under GCC 14.2, so nothing but a test holds
        // this return value; the caller that must listen is SetsWidget::saveSet().
        SECTION("Written")
        {
            MovieSetFolderGuard::useFolder(emptyMsif("artwork_reported"));
            CHECK(mediaCenter->saveMovieSetPoster("Alien Collection", poster));
            CHECK(mediaCenter->saveMovieSetBackdrop("Alien Collection", poster));
        }

        SECTION("Not written")
        {
            Settings::instance()->setMovieSetArtworkType(MovieSetArtworkType::SeparateArtworkFolder);
            Settings::instance()->setMovieSetArtworkDirectory(mediaelch::DirectoryPath());
            CHECK_FALSE(mediaCenter->saveMovieSetPoster("Alien Collection", poster));
            CHECK_FALSE(mediaCenter->saveMovieSetBackdrop("Alien Collection", poster));
        }
    }
}

TEST_CASE("Movie set rename on disk", "[data][movie][movie_set][kodi][nfo]")
{
    const MovieSetFolderGuard guard;
    const test::DataFileGuard dataFiles;
    MediaCenterInterface* mediaCenter = Manager::instance()->mediaCenterInterface();
    using MovieSetFileMove = MediaCenterInterface::MovieSetFileMove;

    QImage poster(4, 4, QImage::Format_RGB32);
    poster.fill(Qt::red);

    SECTION("The record follows the folder and still names the set afterwards")
    {
        // The key has three copies on disk -- every member's <set><name>, the folder name and
        // the record's <originaltitle> -- and a rename that moves only two of them leaves a
        // record no path will accept: unwritable by saveMovieSet(), invisible to
        // movieSetsWithRecord() and loadMovieSet(), and undeletable by removeMovieSetRecord().
        const QDir msif = emptyMsif("rename_record");
        MovieSetFolderGuard::useFolder(msif);

        MovieSet set("Alien Collection");
        set.setOverview("Ripley versus the Alien.");
        set.setTmdbId(TmdbId("8091"));
        REQUIRE(mediaCenter->saveMovieSet(set));
        REQUIRE(mediaCenter->saveMovieSetPoster("Alien Collection", poster));

        CHECK(mediaCenter->renameMovieSetFiles("Alien Collection", "Alien Anthology") == MovieSetFileMove::Moved);

        CHECK_FALSE(QFileInfo::exists(msif.absoluteFilePath("Alien Collection")));
        REQUIRE(QFileInfo::exists(msif.absoluteFilePath("Alien Anthology/set.nfo")));
        CHECK(QFileInfo::exists(msif.absoluteFilePath("Alien Anthology/Alien Anthology-poster.jpg")));

        CHECK(setNameOf(readFile(msif.absoluteFilePath("Alien Anthology/set.nfo"))) == "Alien Anthology");
        CHECK(mediaCenter->movieSetsWithRecord() == QStringList{"Alien Anthology"});

        MovieSet renamed("Alien Anthology");
        REQUIRE(mediaCenter->loadMovieSet(renamed));
        // Carried over rather than rewritten from an empty set: the record is the only place
        // the overview and the collection id live.
        CHECK(renamed.overview() == "Ripley versus the Alien.");
        CHECK(renamed.tmdbId() == TmdbId("8091"));

        // And the set can still be written and removed, which is what the stranded record
        // made impossible.
        CHECK(mediaCenter->saveMovieSet(renamed));
        CHECK(mediaCenter->removeMovieSetRecord("Alien Anthology"));
    }

    SECTION("A display title from an earlier set-file-only rename is re-unified with the key")
    {
        // MovieSet::setName() clears the display title, so the record has to lose it too --
        // otherwise the next reload would put the old displayed name back.
        const QDir msif = emptyMsif("rename_title");
        MovieSetFolderGuard::useFolder(msif);

        MovieSet set("Alien Collection");
        set.setTitle("The Alien Films");
        REQUIRE(mediaCenter->saveMovieSet(set));

        CHECK(mediaCenter->renameMovieSetFiles("Alien Collection", "Alien Anthology") == MovieSetFileMove::Moved);

        const QString written = readFile(msif.absoluteFilePath("Alien Anthology/set.nfo"));
        CHECK_THAT(written, Contains("<title>Alien Anthology</title>"));
        CHECK_THAT(written, Contains("<originaltitle>Alien Anthology</originaltitle>"));

        MovieSet renamed("Alien Anthology");
        REQUIRE(mediaCenter->loadMovieSet(renamed));
        CHECK(renamed.title().isEmpty());
        CHECK(renamed.displayName() == "Alien Anthology");
    }

    SECTION("A rename to the name the set already has leaves the record alone")
    {
        // The key does not move, so neither does the display title beside it: re-unifying the
        // two here would throw away a set-file-only rename for nothing.
        const QDir msif = emptyMsif("rename_samename");
        MovieSetFolderGuard::useFolder(msif);

        MovieSet set("Alien Collection");
        set.setTitle("The Alien Films");
        REQUIRE(mediaCenter->saveMovieSet(set));

        CHECK(mediaCenter->renameMovieSetFiles("Alien Collection", "Alien Collection") == MovieSetFileMove::Moved);

        MovieSet reread("Alien Collection");
        REQUIRE(mediaCenter->loadMovieSet(reread));
        CHECK(reread.title() == "The Alien Films");
    }

    SECTION("The record moves even when an artwork file cannot follow it")
    {
        // The folder is renamed first and the files in it afterwards, so a failure from there
        // is never "nothing moved": the record is already at the new name.
        const QDir msif = emptyMsif("rename_partly");
        MovieSetFolderGuard::useFolder(msif);

        MovieSet set("Alien Collection");
        REQUIRE(mediaCenter->saveMovieSet(set));
        REQUIRE(mediaCenter->saveMovieSetPoster("Alien Collection", poster));
        // Something already at the name the poster wants; renaming onto it would lose a file.
        QFile blocker(msif.absoluteFilePath("Alien Collection/Alien Anthology-poster.jpg"));
        REQUIRE(blocker.open(QIODevice::WriteOnly));
        blocker.close();

        CHECK(mediaCenter->renameMovieSetFiles("Alien Collection", "Alien Anthology") == MovieSetFileMove::PartlyMoved);

        MovieSet renamed("Alien Anthology");
        CHECK(mediaCenter->loadMovieSet(renamed));
        // The poster stayed behind under the old name, in the new folder.
        CHECK(QFileInfo::exists(msif.absoluteFilePath("Alien Anthology/Alien Collection-poster.jpg")));
    }

    SECTION("A folder with artwork but no record moves and gains none")
    {
        // A folder with no `set.nfo` is artwork alone and has no other claimant, so it moves
        // -- but a rename is not the moment at which a set earns a record.
        const QDir msif = emptyMsif("rename_artonly");
        MovieSetFolderGuard::useFolder(msif);
        REQUIRE(mediaCenter->saveMovieSetPoster("Alien Collection", poster));
        REQUIRE_FALSE(QFileInfo::exists(msif.absoluteFilePath("Alien Collection/set.nfo")));

        CHECK(mediaCenter->renameMovieSetFiles("Alien Collection", "Alien Anthology") == MovieSetFileMove::Moved);

        CHECK(QFileInfo::exists(msif.absoluteFilePath("Alien Anthology/Alien Anthology-poster.jpg")));
        CHECK_FALSE(QFileInfo::exists(msif.absoluteFilePath("Alien Anthology/set.nfo")));
        CHECK(mediaCenter->movieSetsWithRecord().isEmpty());
    }

    SECTION("Two names that share one folder still rename the record in it")
    {
        // Legalisation is lossy, so both names live in "Mission_ Impossible Collection" and
        // the directory stays where it is -- but the record inside it is keyed on the set's
        // own name, so it is stranded just the same if the rename passes it by.
        const QDir msif = emptyMsif("rename_samefolder");
        MovieSetFolderGuard::useFolder(msif);

        MovieSet set("Mission: Impossible Collection");
        set.setOverview("Ethan Hunt runs.");
        REQUIRE(mediaCenter->saveMovieSet(set));
        REQUIRE(mediaCenter->saveMovieSetPoster("Mission: Impossible Collection", poster));

        CHECK(mediaCenter->renameMovieSetFiles("Mission: Impossible Collection", "Mission? Impossible Collection")
              == MovieSetFileMove::Moved);

        const QDir folder(msif.absoluteFilePath("Mission_ Impossible Collection"));
        CHECK(mediaCenter->movieSetsWithRecord() == QStringList{"Mission? Impossible Collection"});

        MovieSet renamed("Mission? Impossible Collection");
        REQUIRE(mediaCenter->loadMovieSet(renamed));
        CHECK(renamed.overview() == "Ethan Hunt runs.");

        // The artwork keeps its name: MediaElch's file name sanitiser drops both ":" and "?",
        // so the two names resolve to one file here as well.
        CHECK(QFileInfo::exists(folder.absoluteFilePath("Mission Impossible Collection-poster.jpg")));
    }

    SECTION("A rename is refused when something is already at the new name")
    {
        // Refused rather than merged: folding two folders together would let this set's
        // artwork shadow another's with no way to tell afterwards which came from where.
        const QDir msif = emptyMsif("rename_occupied");
        MovieSetFolderGuard::useFolder(msif);

        MovieSet set("Alien Collection");
        set.setOverview("Ripley versus the Alien.");
        REQUIRE(mediaCenter->saveMovieSet(set));
        REQUIRE(QDir().mkpath(msif.absoluteFilePath("Alien Anthology")));

        CHECK(mediaCenter->renameMovieSetFiles("Alien Collection", "Alien Anthology") == MovieSetFileMove::NotMoved);

        // Nothing at all happened: the record is untouched and still names the old set.
        CHECK(QFileInfo::exists(msif.absoluteFilePath("Alien Collection/set.nfo")));
        CHECK_FALSE(QFileInfo::exists(msif.absoluteFilePath("Alien Anthology/set.nfo")));
        CHECK(mediaCenter->movieSetsWithRecord() == QStringList{"Alien Collection"});
    }

    SECTION("A rename is refused when the record in the folder names another set")
    {
        // Legalisation is lossy, so "Mission: Impossible Collection" and the name spelled the
        // way its folder is share that folder.  Only the set the record names may move it.
        const QDir msif = emptyMsif("rename_foreign");
        MovieSetFolderGuard::useFolder(msif);

        MovieSet owner("Mission: Impossible Collection");
        owner.setOverview("Ethan Hunt runs.");
        REQUIRE(mediaCenter->saveMovieSet(owner));

        CHECK(mediaCenter->renameMovieSetFiles("Mission_ Impossible Collection", "Ethan Hunt Collection")
              == MovieSetFileMove::NotMoved);

        CHECK_FALSE(QFileInfo::exists(msif.absoluteFilePath("Ethan Hunt Collection")));
        CHECK(mediaCenter->movieSetsWithRecord() == QStringList{"Mission: Impossible Collection"});
    }

    SECTION("There is nothing to move without a movie set information folder")
    {
        Settings::instance()->setMovieSetArtworkType(MovieSetArtworkType::SeparateArtworkFolder);
        Settings::instance()->setMovieSetArtworkDirectory(mediaelch::DirectoryPath());
        REQUIRE_FALSE(Settings::instance()->movieSetArtworkDirectory().isValid());

        CHECK(mediaCenter->renameMovieSetFiles("Alien Collection", "Alien Anthology") == MovieSetFileMove::Moved);
        CHECK_FALSE(QFileInfo::exists(QDir::current().absoluteFilePath("Alien Anthology")));
    }

    SECTION("Artwork next to movies is renamed where it lies")
    {
        // No per-set folder and no record here: the artwork's directory is found through a
        // member movie, so this is the layout the caller's ordering invariant exists for.
        QDir movieDir = test::makeTempDir("movie_set/rename_next_to_movies");
        movieDir.removeRecursively();
        const LibraryMovieGuard movie(movieDir, "Alien Collection");

        Settings::instance()->setMovieSetArtworkType(MovieSetArtworkType::ArtworkNextToMovies);
        Settings::instance()->setMovieSetArtworkDirectory(mediaelch::DirectoryPath());
        REQUIRE(mediaCenter->saveMovieSetPoster("Alien Collection", poster));
        REQUIRE(QFileInfo::exists(movieDir.absoluteFilePath("Alien Collection-poster.jpg")));

        CHECK(mediaCenter->renameMovieSetFiles("Alien Collection", "Alien Anthology") == MovieSetFileMove::Moved);

        CHECK(QFileInfo::exists(movieDir.absoluteFilePath("Alien Anthology-poster.jpg")));
        CHECK_FALSE(QFileInfo::exists(movieDir.absoluteFilePath("Alien Collection-poster.jpg")));
    }

    SECTION("Artwork next to movies is unreachable once the members carry the new name")
    {
        // Why SetsWidget moves the files before it reassigns the movies: afterwards no member
        // answers to the old name and the anchor that resolves the directory is gone.  The
        // rename then reports success because there was nothing left it could find to move.
        QDir movieDir = test::makeTempDir("movie_set/rename_reassigned_first");
        movieDir.removeRecursively();
        const LibraryMovieGuard movie(movieDir, "Alien Collection");

        Settings::instance()->setMovieSetArtworkType(MovieSetArtworkType::ArtworkNextToMovies);
        Settings::instance()->setMovieSetArtworkDirectory(mediaelch::DirectoryPath());
        REQUIRE(mediaCenter->saveMovieSetPoster("Alien Collection", poster));

        const QVector<Movie*> movies = Manager::instance()->movieModel()->movies();
        REQUIRE(movies.size() == 1);
        MovieSetInfo reassigned = movies.first()->set();
        reassigned.name = "Alien Anthology";
        movies.first()->setSetInfo(reassigned);

        CHECK(mediaCenter->renameMovieSetFiles("Alien Collection", "Alien Anthology") == MovieSetFileMove::Moved);
        CHECK(QFileInfo::exists(movieDir.absoluteFilePath("Alien Collection-poster.jpg")));
        CHECK_FALSE(QFileInfo::exists(movieDir.absoluteFilePath("Alien Anthology-poster.jpg")));
    }
}
