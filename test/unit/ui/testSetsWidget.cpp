#include "test/test_helpers.h"

#include "data/movie/Movie.h"
#include "data/movie/MovieSet.h"
#include "globals/Manager.h"
#include "media/Path.h"
#include "media_center/MediaCenterInterface.h"
#include "model/MovieModel.h"
#include "model/MovieSetModel.h"
#include "settings/DataFile.h"
#include "settings/Settings.h"
#include "test/helpers/message_capture.h"
#include "test/helpers/movie_set_settings.h"
#include "ui/movie_sets/SetsWidget.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QImage>
#include <QLabel>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>

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

/// \brief Writes a `set.nfo` that really does belong to \p setName.
/// \details Such a set has a record and needs no member movie to exist.
void writeRecord(const QDir& msif, const QString& setName)
{
    REQUIRE(QDir().mkpath(msif.absoluteFilePath(setName)));
    QFile record(msif.absoluteFilePath(setName + "/set.nfo"));
    REQUIRE(record.open(QIODevice::WriteOnly));
    record.write(QString("<set><title>%1</title><originaltitle>%1</originaltitle></set>").arg(setName).toUtf8());
    record.close();
}

/// \brief Emits Settings::sigSettingsSaved without writing the user's real settings.
void announceSettingsSaved()
{
    REQUIRE(QMetaObject::invokeMethod(Settings::instance(), "sigSettingsSaved"));
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

    // And the other set's record was not written over.  Read as a *file*, not through
    // loadMovieSet(): that record is misfiled -- it names "Something Else Entirely" but
    // sits in "Alien Collection", which is the whole reason the save had to refuse --
    // so asking the media center for it resolves to a path that never existed and
    // answers "not found" whatever happened to the file this line is guarding.
    QFile record(guard.dir().absoluteFilePath("Alien Collection/set.nfo"));
    REQUIRE(record.open(QIODevice::ReadOnly));
    const QString onDisk = QString::fromUtf8(record.readAll());
    record.close();
    CHECK_THAT(onDisk, Contains("Something Else Entirely"));
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

TEST_CASE("The sets tab keeps artwork a refused save could not write", "[ui][movie][set]")
{
    // A set's poster exists nowhere but this widget's own map until it is written, so
    // clearing that entry for a write that did not happen destroys the image -- and this
    // used to clear it unconditionally, because the save returned void, and then report
    // "Saved".  The refusal became reachable when the artwork paths stopped resolving
    // into the process's working directory: what used to be written somewhere useless is
    // now not written at all, so the caller has to hold on to it.
    MovieSetFolderGuard guard;
    test::DataFileGuard dataFiles;

    addLibraryMovie("Alien", "Alien Collection");

    // The only way a pending image gets into the widget is a download or a rename that
    // carries one over.  A rename is the one a test can drive: it reads the set's poster
    // through the media center and keeps it under the new name until the next save.
    QImage poster(4, 4, QImage::Format_RGB32);
    poster.fill(Qt::red);
    const QVector<DataFile> posterFiles = Settings::instance()->dataFiles(DataFileType::MovieSetPoster);
    REQUIRE_FALSE(posterFiles.isEmpty());
    REQUIRE(QDir().mkpath(guard.dir().absoluteFilePath("Alien Collection")));
    for (DataFile dataFile : posterFiles) {
        const QString fileName =
            guard.dir().absoluteFilePath("Alien Collection/" + dataFile.saveFileName("Alien Collection"));
        REQUIRE(poster.save(fileName, "jpg", 100));
    }

    SetsWidget widget;
    widget.loadSets();

    auto* sets = widget.findChild<QTableWidget*>("sets");
    REQUIRE(sets != nullptr);
    REQUIRE(sets->rowCount() == 1);
    sets->item(0, 0)->setText("Alien Anthology");

    // Now take the movie set information folder away, so the pending poster has nowhere
    // to go.  The set's own record is *not* also a failure here: with no folder there
    // are no records at all, which saveSet() tells apart from a record that could not be
    // written, so this lands in the artwork-only branch of the three.
    Settings::instance()->setMovieSetArtworkDirectory(mediaelch::DirectoryPath());
    REQUIRE_FALSE(Manager::instance()->mediaCenterInterface()->movieSetArtworkEnabled());

    {
        test::MessageCapture messages;
        widget.saveSet();
        CHECK(messages.contains("its artwork could not be written"));
        // Which branch it is, not merely that something complained: all three failure
        // branches contain "could not be written", so a bare substring check would go
        // green for a save that failed in a different way than this test set up.
        CHECK_FALSE(messages.contains("movie set file"));
    }

    // And the image was kept rather than dropped: with the folder back, the next save
    // writes it, under the new name.
    Settings::instance()->setMovieSetArtworkDirectory(mediaelch::DirectoryPath(guard.dir()));
    {
        test::MessageCapture messages;
        widget.saveSet();
        CHECK_FALSE(messages.contains("could not be written"));
    }
    DataFile posterFile = posterFiles.first();
    CHECK(QFileInfo::exists(
        guard.dir().absoluteFilePath("Alien Anthology/" + posterFile.saveFileName("Alien Anthology"))));
}

TEST_CASE("Add Movie Set is off without a movie set directory", "[ui][movie][set]")
{
    // The load-bearing half of the read-only guard.  With no movie set information
    // folder a set can have no `set.nfo`, and a set with neither members nor a record is
    // dropped by the next reload -- so *Add Movie Set* would offer the user something
    // that silently disappears.  It is also the only path that can create such a set, so
    // disabling it is what keeps read-only mode from accumulating them, which the rest
    // of the design depends on.  Delete this guard and the third section below starts
    // describing what users see.
    MovieSetFolderGuard guard;
    addLibraryMovie("Alien", "Alien Collection");

    SetsWidget widget;
    auto* addSet = widget.findChild<QAction*>("actionAddMovieSet");
    REQUIRE(addSet != nullptr);
    MovieSetModel* setModel = Manager::instance()->movieSetModel();

    SECTION("With a directory it is offered, and it adds a set")
    {
        widget.loadSets();
        CHECK(addSet->isEnabled());
        REQUIRE(QMetaObject::invokeMethod(&widget, "onAddMovieSet"));
        CHECK(setModel->set("New Movie Set") != nullptr);
    }

    SECTION("Without one it is not offered, and it does not add a set")
    {
        // Both halves: the action the user can reach is disabled, and the slot behind it
        // refuses on its own, so a future rearrangement of the menu cannot reopen this.
        Settings::instance()->setMovieSetArtworkType(MovieSetArtworkType::ArtworkNextToMovies);
        widget.loadSets();
        CHECK_FALSE(addSet->isEnabled());
        REQUIRE(QMetaObject::invokeMethod(&widget, "onAddMovieSet"));
        CHECK(setModel->set("New Movie Set") == nullptr);
    }

    SECTION("Because such a set does not survive the next reload")
    {
        // The reason, stated as a fact rather than as a comment.  This is what the user
        // would be shown if the guard above were removed as over-cautious.
        Settings::instance()->setMovieSetArtworkType(MovieSetArtworkType::ArtworkNextToMovies);
        widget.loadSets();
        REQUIRE(setModel->addSet("New Movie Set") != nullptr);
        widget.loadSets();
        CHECK(setModel->set("New Movie Set") == nullptr);
    }
}

TEST_CASE("A set named on a movie is created without a movie set directory", "[ui][movie][set]")
{
    // The complement, and it is why the guard above is on *Add Movie Set* rather than on
    // set creation.  Naming a set on a movie -- the movie widget's set box, or Add Movie
    // here -- creates a set that has a member from the moment it exists, so nothing
    // drops it and it comes back from the movie's own NFO on every reload.  Membership
    // is authoritative in the movie files with or without a folder (D1a).
    //
    // Closing this "back door" would break assigning movies to sets in the shipping
    // default configuration, which is the larger bug by far.
    MovieSetFolderGuard guard;
    Settings::instance()->setMovieSetArtworkType(MovieSetArtworkType::ArtworkNextToMovies);

    addLibraryMovie("Alien", "");
    SetsWidget widget;
    widget.loadSets();

    MovieSetModel* setModel = Manager::instance()->movieSetModel();
    REQUIRE_FALSE(setModel->recordsAreConfigured());
    REQUIRE(setModel->set("Alien Collection") == nullptr);

    const QVector<Movie*> movies = Manager::instance()->movieModel()->movies();
    REQUIRE(movies.size() == 1);
    MovieSetInfo info;
    info.name = "Alien Collection";
    setModel->assign(movies.first(), info);
    REQUIRE(setModel->set("Alien Collection") != nullptr);

    widget.loadSets();
    CHECK(setModel->set("Alien Collection") != nullptr);
}

TEST_CASE("The sets tab says what is off and why", "[ui][movie][set]")
{
    MovieSetFolderGuard guard;

    SetsWidget widget;
    auto* frame = widget.findChild<QFrame*>("folderNoticeFrame");
    auto* notice = widget.findChild<QLabel*>("folderNotice");
    REQUIRE(frame != nullptr);
    REQUIRE(notice != nullptr);

    SECTION("With a directory configured there is nothing to say")
    {
        widget.applyWriteAccess();
        CHECK(frame->isHidden());
    }

    SECTION("A separate directory that was never chosen is a warning")
    {
        Settings::instance()->setMovieSetArtworkDirectory(mediaelch::DirectoryPath());
        widget.applyWriteAccess();
        CHECK_FALSE(frame->isHidden());
        CHECK(frame->frameShape() == QFrame::StyledPanel);
        CHECK_THAT(notice->text(), Contains("No movie set directory is configured"));

        // Exactly what is off, and no more.
        CHECK_THAT(notice->text(), Contains("Set artwork cannot be saved"));
        CHECK_THAT(notice->text(), Contains("no file of their own"));
        CHECK_THAT(notice->text(), Contains("no movies cannot be created"));

        // And what still works, which is the half that keeps this notice honest.  The
        // tab is *not* read-only: renaming a set, moving movies in and out of it and the
        // sort title all write, and they write the member movies, which need no
        // directory.  Saying otherwise would send the user off to rename a set by
        // retyping the name on each movie, which is the D3 fork the sets tab's own
        // rename exists to prevent.
        CHECK_THAT(notice->text(), Contains("Renaming a set"));
        CHECK_THAT(notice->text(), !Contains("read-only"));
        // Neither the overview nor the TMDB id has an editor anywhere yet, so naming
        // them here would describe a loss the user cannot feel.
        CHECK_THAT(notice->text(), !Contains("TMDB"));
    }

    SECTION("Artwork next to movies is not a warning, because nothing is wrong")
    {
        // The default layout exists so that nobody has to configure a directory before
        // using MediaElch, so this line is seen by every user who has never opened the
        // settings.  Warning them would be telling them off for using the default.
        Settings::instance()->setMovieSetArtworkType(MovieSetArtworkType::ArtworkNextToMovies);
        widget.applyWriteAccess();
        CHECK_FALSE(frame->isHidden());
        CHECK(frame->frameShape() == QFrame::NoFrame);
        CHECK_THAT(notice->text(), !Contains("No movie set directory is configured"));
        CHECK_THAT(notice->text(), Contains("next to your movies"));
    }
}

TEST_CASE("The sets tab follows the setting while it is open", "[ui][movie][set]")
{
    // The settings window is a separate window, so the movie set directory can be
    // changed while this tab is on screen.  There is no signal for that setting in
    // particular; sigSettingsSaved is the one the application has, and it fires for
    // unrelated saves too, which is why the handler compares before it acts.
    MovieSetFolderGuard guard;
    addLibraryMovie("Alien", "Alien Collection");

    SetsWidget widget;
    // Shown, off screen, because half of the rule is conditional on isVisible(): the
    // reload only happens for a user who is looking at this tab.  A widget that is never
    // shown reports false there, so a test that skips this cannot tell the reload from
    // its absence -- and that half was a live survivor until it did.
    widget.show();
    widget.loadSets();

    auto* addSet = widget.findChild<QAction*>("actionAddMovieSet");
    auto* frame = widget.findChild<QFrame*>("folderNoticeFrame");
    auto* sets = widget.findChild<QTableWidget*>("sets");
    REQUIRE(addSet != nullptr);
    REQUIRE(frame != nullptr);
    REQUIRE(sets != nullptr);
    REQUIRE(addSet->isEnabled());
    REQUIRE(frame->isHidden());

    SECTION("Taking the directory away disables writing at once")
    {
        Settings::instance()->setMovieSetArtworkType(MovieSetArtworkType::ArtworkNextToMovies);
        announceSettingsSaved();
        CHECK_FALSE(addSet->isEnabled());
        CHECK_FALSE(frame->isHidden());

        // And putting it back re-enables it, without waiting for the tab to be re-entered.
        Settings::instance()->setMovieSetArtworkType(MovieSetArtworkType::SeparateArtworkFolder);
        announceSettingsSaved();
        CHECK(addSet->isEnabled());
        CHECK(frame->isHidden());
    }

    SECTION("Choosing a directory finds the sets only a record knows about")
    {
        // The other half of the rule, and the reason it is a reload and not just a
        // re-enable: a set with a `set.nfo` and no member movie is invisible until the
        // folder has been listed, and nothing but reload() lists it.  Without that call
        // the user chooses a directory, is told everything is writable again, and their
        // curated empty sets are still missing until they leave the tab and come back.
        Settings::instance()->setMovieSetArtworkType(MovieSetArtworkType::ArtworkNextToMovies);
        announceSettingsSaved();
        REQUIRE(Manager::instance()->movieSetModel()->set("Curated Collection") == nullptr);

        writeRecord(guard.dir(), "Curated Collection");
        Settings::instance()->setMovieSetArtworkType(MovieSetArtworkType::SeparateArtworkFolder);
        announceSettingsSaved();

        CHECK(Manager::instance()->movieSetModel()->set("Curated Collection") != nullptr);
        CHECK(sets->rowCount() == 2);
    }
}

TEST_CASE("Turning the directory off does not destroy sets that have a record", "[ui][movie][set]")
{
    // A set with a `set.nfo` and no member movie is held up by its record alone.  While
    // records are off, MovieSetModel::isBacked() answers false for every set, so a
    // reload() at that moment would run dropEmptySets() over all of them and destroy it
    // -- which is why the settings handler re-applies the controls and does *not*
    // reload.  Reaching for a reload there is the obvious-looking change that this test
    // exists to stop.
    MovieSetFolderGuard guard;
    writeRecord(guard.dir(), "Alien Collection");

    SetsWidget widget;
    widget.loadSets();

    MovieSetModel* setModel = Manager::instance()->movieSetModel();
    MovieSet* movieSet = setModel->set("Alien Collection");
    REQUIRE(movieSet != nullptr);
    REQUIRE(movieSet->movies().isEmpty());
    REQUIRE(movieSet->hasRecord());

    Settings::instance()->setMovieSetArtworkType(MovieSetArtworkType::ArtworkNextToMovies);
    announceSettingsSaved();

    // Still here, and still knowing it has a record: the flag is what makes turning the
    // directory back on restore every set's answer at once instead of at the next
    // reload.  Re-deriving it from an empty answer would clear every one of them.
    movieSet = setModel->set("Alien Collection");
    REQUIRE(movieSet != nullptr);
    CHECK(movieSet->hasRecord());

    Settings::instance()->setMovieSetArtworkType(MovieSetArtworkType::SeparateArtworkFolder);
    announceSettingsSaved();
    movieSet = setModel->set("Alien Collection");
    REQUIRE(movieSet != nullptr);
    CHECK(movieSet->hasRecord());
}

TEST_CASE("Saving without a movie set directory is not a failure", "[ui][movie][set]")
{
    // "There are no records in this configuration" is not "the record could not be
    // written", but saveMovieSet() answers false for both -- correctly, because the
    // model's callers need to know a record was not written.  Asked without checking
    // first, every Save in the artwork-next-to-movies layout, which is the default,
    // reported a failure to write a file that the sets tab's own notice has just
    // finished explaining does not exist in that layout.
    MovieSetFolderGuard guard;
    Settings::instance()->setMovieSetArtworkType(MovieSetArtworkType::ArtworkNextToMovies);
    addLibraryMovie("Alien", "Alien Collection");

    SetsWidget widget;
    widget.loadSets();
    REQUIRE_FALSE(Manager::instance()->movieSetModel()->recordsAreConfigured());

    test::MessageCapture messages;
    widget.saveSet();

    CHECK_FALSE(messages.contains("could not be written"));
    // And the write was not even attempted, so there is nothing for the media center to
    // have complained about either.
    CHECK_FALSE(messages.contains("Not saving the record"));
}

TEST_CASE("Set artwork is off only where it has nowhere to go", "[ui][movie][set]")
{
    // The artwork guard asks the *other* question: whether the layout resolves to a real
    // path.  It is not the record question, and the middle section is why -- gating
    // artwork on records would turn it off in the shipping default, where it has always
    // worked.
    MovieSetFolderGuard guard;
    addLibraryMovie("Alien", "Alien Collection");

    SetsWidget widget;
    auto* poster = widget.findChild<QWidget*>("poster");
    auto* backdrop = widget.findChild<QWidget*>("backdrop");
    REQUIRE(poster != nullptr);
    REQUIRE(backdrop != nullptr);

    SECTION("A configured directory: on")
    {
        widget.loadSets();
        CHECK(poster->isEnabled());
        CHECK(backdrop->isEnabled());
    }

    SECTION("Artwork next to movies: on, because that layout resolves too")
    {
        Settings::instance()->setMovieSetArtworkType(MovieSetArtworkType::ArtworkNextToMovies);
        widget.loadSets();
        CHECK(poster->isEnabled());
        CHECK(backdrop->isEnabled());
    }

    SECTION("A separate directory that was never chosen: off")
    {
        Settings::instance()->setMovieSetArtworkDirectory(mediaelch::DirectoryPath());
        widget.loadSets();
        CHECK_FALSE(poster->isEnabled());
        CHECK_FALSE(backdrop->isEnabled());

        // And the slots behind those labels refuse as well, each one on its own: the two
        // log different lines, so removing one guard and leaving the other cannot pass.
        //
        // Removing a guard has to make this *fail*, not hang, because a hang reads like
        // a broken test rather than a broken guard.  Without the guard the slot reaches
        // ImageDialog, which enters a nested event loop and waits for a user who is not
        // there.  Two things stop that being a hang, and only one of them was designed:
        //
        //  - measured, and it is what actually happens in this binary: ImageDialog is
        //    constructed with MainWindow::instance(), which is null here, and the run
        //    dies with SIGSEGV.  Catch reports that as a failed assertion and the binary
        //    exits 139, so ctest fails.  Ugly, but not a hang, and not something this
        //    test can prevent -- the crash happens before any dialog exists.
        //  - the queued lambda below, which runs inside that nested loop and closes
        //    whatever modal it finds.  It is the one that would keep this honest if the
        //    crash ever went away, and with the guards in place it finds nothing to
        //    close and does nothing.  It has never been observed to fire; it is
        //    insurance, and is written down as insurance rather than as the mechanism.
        const auto closeAnyModal = [] {
            QTimer::singleShot(0, qApp, [] {
                if (QWidget* modal = QApplication::activeModalWidget()) {
                    modal->close();
                }
            });
        };

        test::MessageCapture messages;
        closeAnyModal();
        REQUIRE(QMetaObject::invokeMethod(&widget, "chooseSetPoster"));
        closeAnyModal();
        REQUIRE(QMetaObject::invokeMethod(&widget, "chooseSetBackdrop"));
        CHECK(messages.contains("Not choosing a set poster"));
        CHECK(messages.contains("Not choosing a set backdrop"));
    }
}

TEST_CASE("A save that fails on both halves says which", "[ui][movie][set]")
{
    // The third of saveSet()'s three failure branches, and the one with no test of its
    // own: records *are* configured, and both the artwork and the set's own file fail to
    // be written.  A read-only or broken mount does this.  The "no directory configured"
    // case used to cover it, and stopped once saveSet() learned to tell "there are no
    // records here" apart from "the record could not be written".
    MovieSetFolderGuard guard;
    test::DataFileGuard dataFiles;

    addLibraryMovie("Alien", "Alien Collection");

    // A pending poster, carried in by a rename, exactly as in the test above -- which
    // needs the directory to be usable, so it is broken afterwards rather than before.
    QImage poster(4, 4, QImage::Format_RGB32);
    poster.fill(Qt::red);
    const QVector<DataFile> posterFiles = Settings::instance()->dataFiles(DataFileType::MovieSetPoster);
    REQUIRE_FALSE(posterFiles.isEmpty());
    REQUIRE(QDir().mkpath(guard.dir().absoluteFilePath("Alien Collection")));
    for (DataFile dataFile : posterFiles) {
        REQUIRE(poster.save(
            guard.dir().absoluteFilePath("Alien Collection/" + dataFile.saveFileName("Alien Collection")), "jpg", 100));
    }

    SetsWidget widget;
    widget.loadSets();
    auto* sets = widget.findChild<QTableWidget*>("sets");
    REQUIRE(sets != nullptr);
    REQUIRE(sets->rowCount() == 1);
    sets->item(0, 0)->setText("Alien Anthology");

    // A plain file standing where the movie set information directory should be.  It is
    // a valid DirectoryPath, so records stay enabled and this is not the "no directory"
    // case -- but nothing can be created underneath it, so mkpath() fails for the
    // artwork and QFile::open() fails for the record.  A regular file rather than a
    // chmod, because a chmod proves nothing when the tests happen to run as root.
    const QString blockerPath = guard.dir().absoluteFilePath("blocker");
    QFile blocker(blockerPath);
    REQUIRE(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    Settings::instance()->setMovieSetArtworkDirectory(mediaelch::DirectoryPath(QDir(blockerPath)));
    REQUIRE(Manager::instance()->movieSetModel()->recordsAreConfigured());

    test::MessageCapture messages;
    widget.saveSet();

    // Named separately, because "something could not be written" is what all three
    // branches say and only this one says both.
    CHECK(messages.contains("its artwork could not be written"));
    CHECK(messages.contains("neither could its movie set file"));
    // The phrase an operator greps for is in this line too; it was the one branch of the
    // three that did not have it.
    CHECK(messages.contains("could not be written"));
}
