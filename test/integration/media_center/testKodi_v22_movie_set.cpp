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
#include "test/helpers/message_capture.h"
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

TEST_CASE("Movie set record rename detection", "[data][movie][movie_set][kodi][nfo]")
{
    // The detector's only observable is a log line, which is why it went unpinned for a
    // round.  It is assertable with the same fixture that pins the model's discard
    // warning, so there is no excuse: a message that should not be there is as much a
    // defect as a missing one.
    SECTION("MediaElch's own record is never reported as renamed")
    {
        // The writer emits <title> and <originaltitle> from one name, so they can only
        // differ if something else wrote the file.  Reading one trimmed and the other not
        // made every set whose name carries whitespace look renamed.
        test::MessageCapture messages;
        MovieSet set(" Alien Collection");
        QDomDocument doc;
        doc.setContent(QString::fromUtf8(kodi::MovieSetXmlWriter(set).getMovieSetXml()));
        kodi::MovieSetXmlReader reader(set);
        REQUIRE(reader.parseNfoDom(doc));

        CHECK_FALSE(messages.contains("is displayed as"));
    }

    SECTION("A record that really was renamed in the set file still says so")
    {
        // And the fix must not have bought that silence by blunting the signal D3a needs.
        test::MessageCapture messages;
        MovieSet set("Alien Collection");
        QDomDocument doc;
        doc.setContent(
            QStringLiteral("<set><title>The Alien Saga</title><originaltitle>Alien Collection</originaltitle></set>"));
        kodi::MovieSetXmlReader reader(set);
        REQUIRE(reader.parseNfoDom(doc));

        CHECK(messages.contains("is displayed as"));
        CHECK(set.name() == "Alien Collection");
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

    SECTION("A record that names nobody: the write takes it, the removal will not")
    {
        // The two paths disagree here **on purpose**, and nothing else says so.
        //
        // A readable `set.nfo` that names no set is nobody's record by this design's own
        // definition: the enumeration skips it and loadMovieSet() refuses it, so no set
        // can ever carry hasRecord() because of it.  The write may therefore claim it --
        // and has to, or that folder is permanently unwritable with nothing in the UI
        // able to clear it.  The removal must still refuse it, because deleting a file
        // it cannot show to belong to the set being deleted is exactly the fail-open it
        // was fixed for.
        //
        // Both directions of the writer's `!recordName.isEmpty() &&` conjunct are pinned
        // here: tighten the writer and the save below fails, loosen the remover and the
        // refusal above does.
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
        // And now that it names someone, the removal will take it.
        CHECK(mediaCenter->removeMovieSetRecord("Alien Collection"));
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

namespace {

/// \brief Puts one movie with a real file into the library, and takes it out again.
/// \details "Artwork next to movies" resolves a set's artwork path through a member
///          movie's folder (KodiXml::movieSetFileName()), so that layout cannot be
///          exercised at all without a movie in the library that has files.
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
        // The set model is a singleton and has just been told about a set; put it back
        // the way the next test in this binary expects to find it.
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
        // The truth table, in one place, because the difference between these two is
        // what this whole guard turns on.  Artwork resolves in *both* layouts; a record
        // resolves only in the movie set information folder.  So gating the artwork
        // paths on the record predicate would take set artwork away from every user who
        // has never opened the settings, "artwork next to movies" being the default.
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
        // The hazard the record paths closed, arriving through the other door.
        // movieSetFileName() called .dir().absolutePath() without asking isValid(), and
        // QDir("").absolutePath() is the *process's current working directory* -- so the
        // savers created a folder and wrote a poster into whatever directory MediaElch
        // happened to be started from.  The folder matters as much as the file:
        // saveMovieSetPoster() calls mkpath() before it writes.
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
        // The read half, and it is not decoration: a poster sitting next to wherever
        // MediaElch was launched from would be displayed as this set's artwork.  That is
        // the same split between "what the path resolves to" and "what the user
        // configured" that the record paths had to close, and a read is what decides
        // what the user is shown.
        const QDir cwd = QDir::current();
        QDir(cwd.absoluteFilePath("Alien Collection")).removeRecursively();
        REQUIRE(QDir().mkpath(cwd.absoluteFilePath("Alien Collection")));

        // Written under every name the reader probes, so the check below cannot pass
        // merely by having guessed the wrong file name.
        const QVector<DataFile> posterFiles = Settings::instance()->dataFiles(DataFileType::MovieSetPoster);
        REQUIRE_FALSE(posterFiles.isEmpty());
        for (DataFile dataFile : posterFiles) {
            const QString fileName =
                cwd.absoluteFilePath("Alien Collection/" + dataFile.saveFileName("Alien Collection"));
            REQUIRE(poster.save(fileName, "jpg", 100));
        }

        // With the folder configured, those files are exactly what the reader finds ...
        MovieSetFolderGuard::useFolder(cwd);
        REQUIRE_FALSE(mediaCenter->movieSetPoster("Alien Collection").isNull());

        // ... and with no folder configured, the very same files must not be found.
        Settings::instance()->setMovieSetArtworkDirectory(mediaelch::DirectoryPath());
        REQUIRE_FALSE(Settings::instance()->movieSetArtworkDirectory().isValid());
        CHECK(mediaCenter->movieSetPoster("Alien Collection").isNull());

        QDir(cwd.absoluteFilePath("Alien Collection")).removeRecursively();
    }

    SECTION("Artwork next to movies still works")
    {
        // The regression guard for this step's own guard, and the reason
        // movieSetArtworkEnabled() exists as a question of its own.  "Artwork next to
        // movies" is the shipping default; it resolves through a member movie's folder
        // and has nothing to do with the movie set information folder.  Refusing it
        // would have been a larger bug than the one being fixed.
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
        // ELCH_NODISCARD is silent on a virtual under GCC (reproduced, 14.2), so nothing
        // but a test can hold this refusal -- and the caller that has to listen is
        // SetsWidget::saveSet(), which loses the image if it does not.  Its side of this
        // is in test/unit/ui/testSetsWidget.cpp.
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
