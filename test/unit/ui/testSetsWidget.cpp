#include "test/test_helpers.h"

#include "data/movie/Movie.h"
#include "data/movie/MovieSet.h"
#include "globals/Manager.h"
#include "media/Path.h"
#include "media_center/MediaCenterInterface.h"
#include "model/MovieModel.h"
#include "model/MovieSetModel.h"
#include "settings/Settings.h"
#include "test/helpers/message_capture.h"
#include "ui/movie_sets/SetsWidget.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

namespace {

/// \brief Points the movie set information folder at an empty temporary one, and back.
class MovieSetFolderGuard
{
public:
    MovieSetFolderGuard() :
        m_type{Settings::instance()->movieSetArtworkType()}, m_dir{Settings::instance()->movieSetArtworkDirectory()}
    {
        // A QTemporaryDir rather than the test temp root: this runs in the unit test
        // binary, which is not given --temp-dir, so the temp root is the working
        // directory -- and writing a movie set information folder into the source tree
        // is not something a test should do.  It cleans itself up too.
        REQUIRE(m_msif.isValid());
        Settings::instance()->setMovieSetArtworkType(MovieSetArtworkType::SeparateArtworkFolder);
        Settings::instance()->setMovieSetArtworkDirectory(mediaelch::DirectoryPath(dir()));
    }
    ~MovieSetFolderGuard()
    {
        Settings::instance()->setMovieSetArtworkType(m_type);
        Settings::instance()->setMovieSetArtworkDirectory(m_dir);
        Manager::instance()->movieModel()->clear();
        qApp->processEvents();
        // The model is a singleton and keeps whatever this test created, so put it back
        // in the state the next test expects: no folder, therefore no records, therefore
        // no sets that survive having no movies.
        Manager::instance()->movieSetModel()->reload();
    }
    MovieSetFolderGuard(const MovieSetFolderGuard&) = delete;
    MovieSetFolderGuard& operator=(const MovieSetFolderGuard&) = delete;

    QDir dir() const { return QDir(m_msif.path()); }

private:
    MovieSetArtworkType m_type;
    mediaelch::DirectoryPath m_dir;
    QTemporaryDir m_msif;
};

/// \brief Puts a movie belonging to \p setName into the library.
void addLibraryMovie(const QString& title, const QString& setName)
{
    auto* movie = new Movie({}, nullptr);
    movie->setTitle(title);
    MovieSetInfo info;
    info.name = setName;
    movie->setSetInfo(info);
    movie->setChanged(false);
    Manager::instance()->movieModel()->addMovie(movie);
}

/// \brief Writes a `set.nfo` into \p folder that names some other set.
/// \details Such a record is skipped by the enumeration -- the name in it does not
///          resolve back to this folder -- so the set it sits on top of is derived from
///          its movies alone and has no record of its own.  Which is exactly the state
///          in which a save has to refuse: the file is there and it is somebody else's.
void writeMisfiledRecord(const QDir& msif, const QString& folder, const QString& otherSetName)
{
    REQUIRE(QDir().mkpath(msif.absoluteFilePath(folder)));
    QFile record(msif.absoluteFilePath(folder + "/set.nfo"));
    REQUIRE(record.open(QIODevice::WriteOnly));
    record.write(QString("<set><originaltitle>%1</originaltitle></set>").arg(otherSetName).toUtf8());
    record.close();
}

} // namespace

TEST_CASE("The sets tab does not report a save that was refused", "[ui][movie][set]")
{
    // saveMovieSet() had one refusal reason when this call site was written and has four
    // now, and the result was discarded.  The user edits a set, presses Save, is told
    // "<set> Saved", and nothing was written -- nor did the set gain the record that
    // would let it survive losing its movies.  They are told the opposite of what
    // happened, twice.
    MovieSetFolderGuard guard;

    // A folder that already holds another set's record, so the write must refuse.
    writeMisfiledRecord(guard.dir(), "Alien Collection", "Something Else Entirely");
    addLibraryMovie("Alien", "Alien Collection");

    SetsWidget widget;
    widget.loadSets();
    REQUIRE(Manager::instance()->movieSetModel()->set("Alien Collection") != nullptr);

    test::MessageCapture messages;
    widget.saveSet();

    CHECK(messages.contains("Alien Collection"));
    CHECK(messages.contains("could not be written"));
    // And the other set's record is still its own.
    MovieSet reread("Something Else Entirely");
    CHECK_FALSE(Manager::instance()->mediaCenterInterface()->loadMovieSet(reread));
}

TEST_CASE("The sets tab reports a save that succeeded", "[ui][movie][set]")
{
    // The other direction, so that the check above cannot be satisfied by a widget that
    // complains about every save.
    MovieSetFolderGuard guard;

    addLibraryMovie("Alien", "Alien Collection");

    SetsWidget widget;
    widget.loadSets();

    test::MessageCapture messages;
    widget.saveSet();

    CHECK_FALSE(messages.contains("could not be written"));
    CHECK(QFileInfo::exists(guard.dir().absoluteFilePath("Alien Collection/set.nfo")));
}
