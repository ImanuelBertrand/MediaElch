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
#include "ui/notifications/NotificationBox.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
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

/// \brief Hands \p widget a downloaded image for \p setName, as the download manager does.
/// \details The one production path that puts an image into the sets tab's own maps
///          without it existing on disk first.  A rename used to do it too, by reading
///          the set's artwork back off the disk and holding it -- which re-encoded a PNG
///          to JPEG and left the original behind; the files are moved on disk now, so a
///          rename no longer produces a pending image and cannot stand in for one here.
void seedDownloadedImage(SetsWidget& widget, const QString& setName, ImageType imageType, const QImage& image)
{
    DownloadManagerElement elem;
    // Owned by the slot, which deletes it.  The set's name travels as the movie's title;
    // that is how the download manager carries it for set artwork.
    elem.movie = new Movie(QStringList());
    elem.movie->setTitle(setName);
    elem.imageType = imageType;
    QBuffer buffer(&elem.data);
    REQUIRE(buffer.open(QIODevice::WriteOnly));
    REQUIRE(image.save(&buffer, "png"));
    buffer.close();
    REQUIRE(QMetaObject::invokeMethod(
        &widget, "onDownloadFinished", Qt::DirectConnection, Q_ARG(DownloadManagerElement, elem)));
}

/// \brief Pins the rename mode for the duration of a test, and puts it back.
class RenameModeGuard
{
public:
    explicit RenameModeGuard(MovieSetRenameMode mode) : m_mode{Settings::instance()->movieSetRenameMode()}
    {
        Settings::instance()->setMovieSetRenameMode(mode);
    }
    ~RenameModeGuard() { Settings::instance()->setMovieSetRenameMode(m_mode); }
    RenameModeGuard(const RenameModeGuard&) = delete;
    RenameModeGuard& operator=(const RenameModeGuard&) = delete;

private:
    MovieSetRenameMode m_mode;
};

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

/// \brief The text of every notification currently on screen, joined.
/// \details The user-facing half of a refusal, which the log lines beside it do not
///          stand in for: the three-state move exists so that the *message* is true for
///          the branch it describes, and a test that only greps the log cannot see the
///          sentence the user is shown.  NotificationBox keeps its messages as Message
///          widgets, each with one QLabel holding the text.
///
///          Nothing clears them between reads, and nothing should: NotificationBox owns
///          its Message widgets and hides them on a timer, so deleting them behind its
///          back leaves dangling pointers in its own list and the next showMessage()
///          walks them.  Within one test the messages are the ones that test caused, and
///          every assertion here is a substring match, so extra text is harmless.
QString notificationText()
{
    QStringList texts;
    for (const QLabel* label : NotificationBox::instance()->findChildren<QLabel*>()) {
        if (!label->text().isEmpty()) {
            texts << label->text();
        }
    }
    return texts.join("\n");
}

/// \brief Answers the next modal question box by clicking \p button.
/// \details Posted before the call that opens the box: QMessageBox::exec() spins a
///          nested event loop, and a zero timer queued beforehand runs as soon as it
///          starts.  Anything modal that is *not* a question box with that button is
///          closed instead, so a test whose expectation is wrong fails rather than hangs.
void answerNextQuestion(QMessageBox::StandardButton button)
{
    QTimer::singleShot(0, qApp, [button] {
        QWidget* modal = QApplication::activeModalWidget();
        auto* box = qobject_cast<QMessageBox*>(modal);
        if (box != nullptr && box->button(button) != nullptr) {
            box->button(button)->click();
        } else if (modal != nullptr) {
            modal->close();
        }
    });
}

/// \brief The one movie of \p setName in the library, or nullptr.
Movie* libraryMovie(const QString& title)
{
    for (Movie* movie : Manager::instance()->movieModel()->movies()) {
        if (movie->title() == title) {
            return movie;
        }
    }
    return nullptr;
}

/// \brief Renames the first row of \p widget's set table to \p newName.
void renameFirstSet(SetsWidget& widget, const QString& newName)
{
    auto* sets = widget.findChild<QTableWidget*>("sets");
    REQUIRE(sets != nullptr);
    REQUIRE(sets->rowCount() >= 1);
    sets->item(0, 0)->setText(newName);
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

    // A pending image is one that exists nowhere but this widget, which is what a
    // download leaves behind.  It used to be seeded by a rename instead, back when a
    // rename read the set's artwork off the disk and held it; the artwork moves on disk
    // now, so a rename produces nothing pending and this has to come from a download.
    QImage poster(4, 4, QImage::Format_RGB32);
    poster.fill(Qt::red);
    const QVector<DataFile> posterFiles = Settings::instance()->dataFiles(DataFileType::MovieSetPoster);
    REQUIRE_FALSE(posterFiles.isEmpty());

    SetsWidget widget;
    widget.loadSets();

    auto* sets = widget.findChild<QTableWidget*>("sets");
    REQUIRE(sets != nullptr);
    REQUIRE(sets->rowCount() == 1);
    seedDownloadedImage(widget, "Alien Collection", ImageType::MovieSetPoster, poster);

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
        guard.dir().absoluteFilePath("Alien Collection/" + posterFile.saveFileName("Alien Collection"))));
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
        //  - the queued lambda below, which would run inside that nested loop and close
        //    whatever modal it finds.  It is the one that would keep this honest if the
        //    crash ever went away -- which it does the day any unit test constructs a
        //    MainWindow.  **It has never executed, so nothing here establishes that it
        //    works: it is unverified insurance, not the mechanism.**  With the guards in
        //    place no modal is created and it finds nothing to close.
        //
        // It closes whatever modal is active, not one belonging to this widget, so each
        // one is drained immediately after the call it was posted for.  Left in the
        // queue it would be consumed by some later processEvents() and could close an
        // unrelated test's dialog -- which happens to be harmless today only because a
        // guard's destructor drains it, and that is luck rather than design.
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
        qApp->processEvents();
        closeAnyModal();
        REQUIRE(QMetaObject::invokeMethod(&widget, "chooseSetBackdrop"));
        qApp->processEvents();
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

    // A pending poster, exactly as in the test above.
    QImage poster(4, 4, QImage::Format_RGB32);
    poster.fill(Qt::red);

    SetsWidget widget;
    widget.loadSets();
    auto* sets = widget.findChild<QTableWidget*>("sets");
    REQUIRE(sets != nullptr);
    REQUIRE(sets->rowCount() == 1);
    seedDownloadedImage(widget, "Alien Collection", ImageType::MovieSetPoster, poster);

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

TEST_CASE("A set-file-only rename never touches a movie", "[ui][movie][set]")
{
    // The whole point of the mode: `set.nfo`'s <title> moves and the join key does not,
    // so Kodi 22 renames the set's row in place and keeps its artwork and its id.
    MovieSetFolderGuard guard;
    RenameModeGuard renameMode(MovieSetRenameMode::SetFileOnly);

    addLibraryMovie("Alien", "Alien Collection");

    SetsWidget widget;
    widget.loadSets();
    renameFirstSet(widget, "The Alien Saga");

    MovieSetModel* setModel = Manager::instance()->movieSetModel();

    SECTION("the key stays where the member movies put it")
    {
        // Looked up under the old name, because that is still the set's key -- and *not*
        // findable under the new one, which is a display title and not a name any file
        // carries.
        MovieSet* set = setModel->set("Alien Collection");
        REQUIRE(set != nullptr);
        CHECK(set->displayName() == "The Alien Saga");
        CHECK(setModel->set("The Alien Saga") == nullptr);
    }

    SECTION("no movie is rewritten")
    {
        Movie* alien = libraryMovie("Alien");
        REQUIRE(alien != nullptr);
        CHECK(alien->set().name == "Alien Collection");
        // Not marked for saving: a set-file-only rename has nothing to write into a
        // movie's NFO, so dirtying one would offer to rewrite a file for no reason.
        CHECK_FALSE(alien->hasChanged());
    }

    SECTION("the row shows the new name and still carries the old key")
    {
        auto* sets = widget.findChild<QTableWidget*>("sets");
        REQUIRE(sets->rowCount() == 1);
        CHECK(sets->item(0, 0)->text() == "The Alien Saga");
        CHECK(sets->item(0, 0)->data(Qt::UserRole).toString() == "Alien Collection");
    }

    SECTION("the heading shows the display title, not the key")
    {
        // loadSet() is always called with the match key, because that is what every
        // caller can look the set up by -- but the heading is read by a person.  Left as
        // the key it said "Alien Collection" in the big label above a row that said
        // "The Alien Saga".
        auto* heading = widget.findChild<QLabel*>("setName");
        REQUIRE(heading != nullptr);
        CHECK(heading->text() == "The Alien Saga");
    }

    SECTION("the divergence is explained at once, not at the next reload")
    {
        // loadSets() was the only place that set this, so the tooltip was missing for
        // exactly the rename that creates the divergence -- the moment a user would ask
        // what just happened.  D-B promises it is never hidden.
        auto* sets = widget.findChild<QTableWidget*>("sets");
        REQUIRE(sets->rowCount() == 1);
        CHECK_THAT(sets->item(0, 0)->toolTip(), Contains("Alien Collection"));

        // And it goes away again when the two names are re-unified.
        Settings::instance()->setMovieSetRenameMode(MovieSetRenameMode::AllMovieFiles);
        sets->item(0, 0)->setText("Alien Anthology");
        CHECK(sets->item(0, 0)->toolTip().isEmpty());
    }

    SECTION("the record is written under the key's folder, not the title's")
    {
        // Kodi derives the movie set information folder from the match key, before it
        // ever loads the record (VideoInfoScanner.cpp:839), so a record filed under the
        // display title is a record Kodi never looks at.
        widget.saveSet();
        CHECK(QFileInfo::exists(guard.dir().absoluteFilePath("Alien Collection/set.nfo")));
        CHECK_FALSE(QFileInfo::exists(guard.dir().absoluteFilePath("The Alien Saga/set.nfo")));
    }

    SECTION("it is rebuilt from its file, which is the failure the second string exists for")
    {
        // The members still name the old set and a rebuild derives from them, so without
        // the record being read back the rename evaporates here.
        //
        // A plain loadSets() does **not** test that, and this test used to do exactly
        // that and pass for the wrong reason.  reload() keeps the MovieSet objects it
        // already has and re-reads a record only on a false->true hasRecord transition
        // (MovieSetModel::reload(), MovieSetModel.cpp:407), and saveSet() has just set that
        // flag -- so the set is never rebuilt from disk and deleting the reader's setTitle()
        // leaves this green.  Measured: with the old body, mutating MovieSetXmlReader reddened
        // two record tests and not this one.
        //
        // So throw the objects away first, which is what a restart does.  The set that
        // comes back is built from the member NFO, gains its record from the listing,
        // and takes that false->true branch -- the one that reads `<title>` back.
        widget.saveSet();

        MovieSetModel* setModel = Manager::instance()->movieSetModel();
        setModel->clear();
        REQUIRE(setModel->set("Alien Collection") == nullptr);

        widget.loadSets();

        auto* sets = widget.findChild<QTableWidget*>("sets");
        REQUIRE(sets->rowCount() == 1);
        CHECK(sets->item(0, 0)->text() == "The Alien Saga");
        CHECK(sets->item(0, 0)->data(Qt::UserRole).toString() == "Alien Collection");
        // And through the model, not only the table, so a widget that happened to cache
        // the string cannot carry this.
        MovieSet* rebuilt = setModel->set("Alien Collection");
        REQUIRE(rebuilt != nullptr);
        CHECK(rebuilt->title() == "The Alien Saga");
    }
}

TEST_CASE("A set-file-only rename with nowhere to write it is refused", "[ui][movie][set]")
{
    // The shipping default has no `set.nfo` at all, so there is no file for the display
    // title to live in.  Refused rather than quietly turned into the all-movie-files
    // rename, which rewrites every member's NFO -- the heavier and irreversible
    // operation this user chose this setting to avoid.
    RenameModeGuard renameMode(MovieSetRenameMode::SetFileOnly);
    REQUIRE(Settings::instance()->movieSetArtworkType() == MovieSetArtworkType::ArtworkNextToMovies);

    addLibraryMovie("Alien", "Alien Collection");

    SetsWidget widget;
    widget.loadSets();

    test::MessageCapture messages;
    renameFirstSet(widget, "The Alien Saga");

    CHECK(messages.contains("was not renamed"));
    CHECK(messages.contains("movie set information folder"));

    // Nothing moved, in either direction.
    MovieSetModel* setModel = Manager::instance()->movieSetModel();
    MovieSet* set = setModel->set("Alien Collection");
    REQUIRE(set != nullptr);
    CHECK(set->displayName() == "Alien Collection");
    CHECK(setModel->set("The Alien Saga") == nullptr);

    Movie* alien = libraryMovie("Alien");
    REQUIRE(alien != nullptr);
    CHECK(alien->set().name == "Alien Collection");
    CHECK_FALSE(alien->hasChanged());

    // And the cell says what the set is called, rather than leaving the user looking at
    // a name nothing answers to.
    auto* sets = widget.findChild<QTableWidget*>("sets");
    CHECK(sets->item(0, 0)->text() == "Alien Collection");

    Manager::instance()->movieModel()->clear();
    qApp->processEvents();
    setModel->reload();
}

TEST_CASE("A rename onto a display title already in use is refused in both modes", "[ui][movie][set]")
{
    // Typing an existing set's *key* is a merge and is offered as one, whatever the mode.
    // Typing another set's *display title* is not: no set answers to that name, so the
    // merge check finds nothing -- correctly, since two sets with one display title are
    // still two sets to Kodi.  They may not be two rows the user cannot tell apart here.
    MovieSetFolderGuard guard;
    RenameModeGuard renameMode(MovieSetRenameMode::SetFileOnly);

    addLibraryMovie("Alien", "Alien Collection");
    addLibraryMovie("Predator", "Predator Collection");

    MovieSetModel* setModel = Manager::instance()->movieSetModel();

    SetsWidget widget;
    widget.loadSets();

    // Give the second set a display title of its own, then try to take it.
    MovieSet* predator = setModel->set("Predator Collection");
    REQUIRE(predator != nullptr);
    predator->setTitle("The Hunt");
    widget.loadSets();

    auto* sets = widget.findChild<QTableWidget*>("sets");
    REQUIRE(sets != nullptr);
    REQUIRE(sets->rowCount() == 2);
    // The rows are sorted by what the user reads, so find "Alien Collection" by its key
    // rather than assuming which row it landed in.
    int alienRow = -1;
    for (int i = 0; i < sets->rowCount(); ++i) {
        if (sets->item(i, 0)->data(Qt::UserRole).toString() == "Alien Collection") {
            alienRow = i;
        }
    }
    REQUIRE(alienRow >= 0);

    // Both modes, because both produce the same two indistinguishable rows.  Under all
    // movie files the typed name becomes this set's *key*, so the collision is permanent:
    // it is what the next reload rebuilds from.
    const MovieSetRenameMode mode =
        GENERATE(MovieSetRenameMode::SetFileOnly, MovieSetRenameMode::AllMovieFiles);
    Settings::instance()->setMovieSetRenameMode(mode);

    test::MessageCapture messages;
    sets->item(alienRow, 0)->setText("The Hunt");

    CHECK(messages.contains("was not renamed"));
    CHECK(messages.contains("already called"));

    MovieSet* set = setModel->set("Alien Collection");
    REQUIRE(set != nullptr);
    CHECK(set->displayName() == "Alien Collection");
    CHECK(sets->item(alienRow, 0)->text() == "Alien Collection");
    // The other set kept its own name either way.
    CHECK(setModel->set("The Hunt") == nullptr);
}

TEST_CASE("An all-movie-files rename moves what the set keeps on disk", "[ui][movie][set]")
{
    MovieSetFolderGuard guard;
    test::DataFileGuard dataFiles;
    RenameModeGuard renameMode(MovieSetRenameMode::AllMovieFiles);

    addLibraryMovie("Alien", "Alien Collection");
    writeRecord(guard.dir(), "Alien Collection");

    SECTION("the record follows, so the old name does not come back as a ghost")
    {
        // Left behind, movieSetsWithRecord() lists the old folder at the next reload and
        // reports a set nothing answers to, which reload() then resurrects with no
        // members -- a ghost in the sets tab, the set combo box and the set filter.
        SetsWidget widget;
        widget.loadSets();
        renameFirstSet(widget, "Alien Anthology");

        CHECK(QFileInfo::exists(guard.dir().absoluteFilePath("Alien Anthology/set.nfo")));
        CHECK_FALSE(QFileInfo::exists(guard.dir().absoluteFilePath("Alien Collection/set.nfo")));

        widget.loadSets();
        auto* sets = widget.findChild<QTableWidget*>("sets");
        CHECK(sets->rowCount() == 1);
        CHECK(sets->item(0, 0)->data(Qt::UserRole).toString() == "Alien Anthology");
    }

    SECTION("every file in the folder moves, not the two types this tab knows about")
    {
        // MovieSetImages::supportedImageTypes() is poster and backdrop, and the rename
        // used to carry exactly those two through memory.  Kodi reads six more, and a
        // user may have put anything in this folder.
        QFile extra(guard.dir().absoluteFilePath("Alien Collection/clearlogo.png"));
        REQUIRE(extra.open(QIODevice::WriteOnly));
        extra.write("not really a png");
        extra.close();

        SetsWidget widget;
        widget.loadSets();
        renameFirstSet(widget, "Alien Anthology");

        CHECK(QFileInfo::exists(guard.dir().absoluteFilePath("Alien Anthology/clearlogo.png")));
    }

    SECTION("artwork moves byte for byte, without a decode and re-encode")
    {
        // The carry-over this replaced read the poster as a QImage and wrote it back as
        // JPEG at quality 100, so a PNG came out a JPEG and a lossless original did not
        // survive its own rename.
        QImage poster(4, 4, QImage::Format_RGB32);
        poster.fill(Qt::red);
        QVector<DataFile> posterFiles = Settings::instance()->dataFiles(DataFileType::MovieSetPoster);
        REQUIRE_FALSE(posterFiles.isEmpty());
        DataFile posterFile = posterFiles.first();
        const QString posterPath =
            guard.dir().absoluteFilePath("Alien Collection/" + posterFile.saveFileName("Alien Collection"));
        REQUIRE(poster.save(posterPath, "png"));

        QFile before(posterPath);
        REQUIRE(before.open(QIODevice::ReadOnly));
        const QByteArray originalBytes = before.readAll();
        before.close();

        SetsWidget widget;
        widget.loadSets();
        renameFirstSet(widget, "Alien Anthology");
        widget.saveSet();

        QFile after(guard.dir().absoluteFilePath("Alien Anthology/" + posterFile.saveFileName("Alien Anthology")));
        REQUIRE(after.open(QIODevice::ReadOnly));
        CHECK(after.readAll() == originalBytes);
        after.close();
    }

    SECTION("the movies are rewritten, which is what makes this the other rename")
    {
        SetsWidget widget;
        widget.loadSets();
        renameFirstSet(widget, "Alien Anthology");

        Movie* alien = libraryMovie("Alien");
        REQUIRE(alien != nullptr);
        CHECK(alien->set().name == "Alien Anthology");
        CHECK(alien->hasChanged());
    }
}

TEST_CASE("An all-movie-files rename that cannot move its files says so", "[ui][movie][set]")
{
    MovieSetFolderGuard guard;
    RenameModeGuard renameMode(MovieSetRenameMode::AllMovieFiles);

    addLibraryMovie("Alien", "Alien Collection");
    writeRecord(guard.dir(), "Alien Collection");
    // Something is already standing where the set's folder would go.  QDir::rename()
    // refuses, and merging the two folders would let one set's artwork shadow another's.
    //
    // Artwork rather than a record: a folder holding a record is a *set*, so renaming
    // onto its name would be a merge and would never reach the move at all.
    REQUIRE(QDir().mkpath(guard.dir().absoluteFilePath("Alien Anthology")));
    QFile occupant(guard.dir().absoluteFilePath("Alien Anthology/folder.jpg"));
    REQUIRE(occupant.open(QIODevice::WriteOnly));
    occupant.write("someone else's artwork");
    occupant.close();

    SetsWidget widget;
    widget.loadSets();

    test::MessageCapture messages;
    renameFirstSet(widget, "Alien Anthology");

    CHECK(messages.contains("could not be moved"));
    // And the user is told they are still under the old name, which for this branch --
    // nothing moved at all -- is true.
    const QString shown = notificationText();
    CHECK_THAT(shown, Contains("could not be moved"));
    CHECK_THAT(shown, Contains("Alien Collection"));

    // The rename still happened: the movie NFOs are the set's identity, so undoing it
    // would mean rewriting every member again -- larger and riskier than a leftover the
    // message names.
    Movie* alien = libraryMovie("Alien");
    REQUIRE(alien != nullptr);
    CHECK(alien->set().name == "Alien Anthology");

    // And both folders are exactly as they were.
    CHECK(QFileInfo::exists(guard.dir().absoluteFilePath("Alien Collection/set.nfo")));
    CHECK(QFileInfo::exists(guard.dir().absoluteFilePath("Alien Anthology/folder.jpg")));
}

TEST_CASE("A rename whose folder moved but whose artwork did not says which", "[ui][movie][set]")
{
    // The separate-folder layout renames the directory and *then* renames the
    // set-name-derived files inside it, so the folder can move while a file in it does
    // not.  Telling that user their files are "still stored under the old name" sends
    // them to a folder that no longer exists -- the record really is at the new name.
    MovieSetFolderGuard guard;
    test::DataFileGuard dataFiles;
    RenameModeGuard renameMode(MovieSetRenameMode::AllMovieFiles);

    addLibraryMovie("Alien", "Alien Collection");
    writeRecord(guard.dir(), "Alien Collection");

    QVector<DataFile> posterFiles = Settings::instance()->dataFiles(DataFileType::MovieSetPoster);
    REQUIRE_FALSE(posterFiles.isEmpty());
    DataFile posterFile = posterFiles.first();
    // The shipped name embeds the set's name, so these two are different files.
    REQUIRE(posterFile.saveFileName("Alien Collection") != posterFile.saveFileName("Alien Anthology"));

    QImage poster(4, 4, QImage::Format_RGB32);
    poster.fill(Qt::red);
    REQUIRE(poster.save(
        guard.dir().absoluteFilePath("Alien Collection/" + posterFile.saveFileName("Alien Collection")), "jpg", 100));
    // A file already sitting under the name the poster wants after the move, so the
    // in-folder rename refuses while the directory rename has already succeeded.
    QFile blocker(guard.dir().absoluteFilePath("Alien Collection/" + posterFile.saveFileName("Alien Anthology")));
    REQUIRE(blocker.open(QIODevice::WriteOnly));
    blocker.write("in the way");
    blocker.close();

    SetsWidget widget;
    widget.loadSets();

    test::MessageCapture messages;
    renameFirstSet(widget, "Alien Anthology");

    CHECK(messages.contains("only some of its files moved"));
    // And *not* the message that would send the user to the old folder.
    CHECK_FALSE(messages.contains("could not be moved at all"));

    // The sentence the user is actually shown, which is the whole point of the three
    // states: the log line is not what tells them where their artwork went.
    const QString shown = notificationText();
    CHECK_THAT(shown, Contains("only some of its files could be moved"));
    CHECK_THAT(shown, Contains("still"));
    // It must not claim the set is stored under the old name -- the folder moved.
    CHECK_THAT(shown, ContainsNot("Alien Collection"));

    // The folder and the record did move, which is what makes the other message a lie.
    CHECK(QFileInfo::exists(guard.dir().absoluteFilePath("Alien Anthology/set.nfo")));
    CHECK_FALSE(QFileInfo::exists(guard.dir().absoluteFilePath("Alien Collection/set.nfo")));
}

TEST_CASE("An all-movie-files rename moves artwork next to the movies too", "[ui][movie][set]")
{
    // The other layout, where there is no per-set folder: the artwork sits beside a
    // member movie under a file name built from the set's name, and the anchor movie is
    // found through the *old* name -- so this only works because the files are moved
    // before the members are reassigned.
    test::DataFileGuard dataFiles;
    RenameModeGuard renameMode(MovieSetRenameMode::AllMovieFiles);
    REQUIRE(Settings::instance()->movieSetArtworkType() == MovieSetArtworkType::ArtworkNextToMovies);

    QTemporaryDir movieDir;
    REQUIRE(movieDir.isValid());
    const QString moviePath = QDir(movieDir.path()).absoluteFilePath("Alien.mkv");
    QFile movieFile(moviePath);
    REQUIRE(movieFile.open(QIODevice::WriteOnly));
    movieFile.close();

    auto* movie = new Movie(QStringList{moviePath}, nullptr);
    movie->setTitle("Alien");
    MovieSetInfo info;
    info.name = "Alien Collection";
    movie->setSetInfo(info);
    movie->setChanged(false);
    Manager::instance()->movieModel()->addMovie(movie);
    qApp->processEvents();

    QVector<DataFile> posterFiles = Settings::instance()->dataFiles(DataFileType::MovieSetPoster);
    REQUIRE_FALSE(posterFiles.isEmpty());
    DataFile posterFile = posterFiles.first();
    QImage poster(4, 4, QImage::Format_RGB32);
    poster.fill(Qt::red);
    const QString oldPoster = QDir(movieDir.path()).absoluteFilePath(posterFile.saveFileName("Alien Collection"));
    REQUIRE(poster.save(oldPoster, "jpg", 100));

    SetsWidget widget;
    widget.loadSets();
    renameFirstSet(widget, "Alien Anthology");

    CHECK_FALSE(QFileInfo::exists(oldPoster));
    CHECK(QFileInfo::exists(QDir(movieDir.path()).absoluteFilePath(posterFile.saveFileName("Alien Anthology"))));

    Manager::instance()->movieModel()->clear();
    qApp->processEvents();
    Manager::instance()->movieSetModel()->reload();
}

TEST_CASE("Renaming a set onto another set's name asks before merging", "[ui][movie][set]")
{
    // A merge moves movies between sets, is not undoable, and is one typo in a table
    // cell away.  It used to happen silently.
    MovieSetFolderGuard guard;

    addLibraryMovie("Alien", "Alien Collection");
    addLibraryMovie("Predator", "Predator Collection");

    SECTION("declining leaves both sets alone")
    {
        SetsWidget widget;
        widget.loadSets();

        answerNextQuestion(QMessageBox::No);
        renameFirstSet(widget, "Predator Collection");

        MovieSetModel* setModel = Manager::instance()->movieSetModel();
        REQUIRE(setModel->set("Alien Collection") != nullptr);
        REQUIRE(setModel->set("Predator Collection") != nullptr);
        CHECK(setModel->set("Alien Collection")->movies().size() == 1);
        CHECK(setModel->set("Predator Collection")->movies().size() == 1);

        Movie* alien = libraryMovie("Alien");
        REQUIRE(alien != nullptr);
        CHECK(alien->set().name == "Alien Collection");
    }

    SECTION("accepting merges, whatever the rename setting says")
    {
        // The setting does not govern a merge, because there is no other way to perform
        // one: membership lives in the member movies' NFOs, so moving a movie into
        // another set *is* rewriting its <set><name>.  A "set file only" merge would
        // write a display title and quietly not merge.
        RenameModeGuard renameMode(MovieSetRenameMode::SetFileOnly);

        SetsWidget widget;
        widget.loadSets();

        answerNextQuestion(QMessageBox::Yes);
        renameFirstSet(widget, "Predator Collection");

        MovieSetModel* setModel = Manager::instance()->movieSetModel();
        CHECK(setModel->set("Alien Collection") == nullptr);
        MovieSet* predator = setModel->set("Predator Collection");
        REQUIRE(predator != nullptr);
        CHECK(predator->movies().size() == 2);

        Movie* alien = libraryMovie("Alien");
        REQUIRE(alien != nullptr);
        CHECK(alien->set().name == "Predator Collection");
        CHECK(alien->hasChanged());
    }
}

TEST_CASE("Clearing a set name is refused and takes no movie with it", "[ui][movie][set]")
{
    // The worst defect in this feature and it was silent.  The all-movie-files rename
    // skipped its collision check and its setName() for an empty name -- and then
    // reassigned every member to "" anyway, detaching the whole set and marking each
    // movie changed, so the next save wrote an empty <set> into every member NFO while
    // the MovieSet kept its name and its row became unfindable.
    //
    // Both modes, because the guard is one precondition in onSetNameChanged() now rather
    // than a check inside each path -- which is how it came to exist on one of them only.
    MovieSetFolderGuard guard;
    const MovieSetRenameMode mode =
        GENERATE(MovieSetRenameMode::SetFileOnly, MovieSetRenameMode::AllMovieFiles);
    RenameModeGuard renameMode(mode);

    addLibraryMovie("Alien", "Alien Collection");
    addLibraryMovie("Aliens", "Alien Collection");

    SetsWidget widget;
    widget.loadSets();

    test::MessageCapture messages;
    renameFirstSet(widget, "");

    CHECK(messages.contains("was not renamed"));
    CHECK(messages.contains("empty name"));
    CHECK_THAT(notificationText(), Contains("cannot have an empty name"));

    // The set is intact and still findable by its key.
    MovieSetModel* setModel = Manager::instance()->movieSetModel();
    MovieSet* set = setModel->set("Alien Collection");
    REQUIRE(set != nullptr);
    CHECK(set->movies().size() == 2);
    CHECK(set->displayName() == "Alien Collection");

    // And no movie was detached or marked for rewriting -- the data loss itself.
    for (const QString& title : {QString("Alien"), QString("Aliens")}) {
        Movie* movie = libraryMovie(title);
        REQUIRE(movie != nullptr);
        CHECK(movie->set().name == "Alien Collection");
        CHECK_FALSE(movie->hasChanged());
    }

    // The row says what the set is called, and can still be looked up.
    auto* sets = widget.findChild<QTableWidget*>("sets");
    REQUIRE(sets->rowCount() == 1);
    CHECK(sets->item(0, 0)->text() == "Alien Collection");
    CHECK(sets->item(0, 0)->data(Qt::UserRole).toString() == "Alien Collection");
}

TEST_CASE("A merge leaves the row explaining the set it actually shows", "[ui][movie][set]")
{
    // performMerge() is the third place a row's identity changes, and the tooltip helper
    // was added to loadSets() and the two renames only.  A diverged source merged onto an
    // undiverged target left the target's row carrying the source's tooltip: a sentence
    // about a set that no longer exists.
    MovieSetFolderGuard guard;

    addLibraryMovie("Alien", "Alien Collection");
    addLibraryMovie("Predator", "Predator Collection");

    MovieSetModel* setModel = Manager::instance()->movieSetModel();

    SetsWidget widget;
    widget.loadSets();

    // Give the source set a display title of its own, so it has a tooltip to leave behind.
    MovieSet* alien = setModel->set("Alien Collection");
    REQUIRE(alien != nullptr);
    alien->setTitle("The Alien Saga");
    widget.loadSets();

    auto* sets = widget.findChild<QTableWidget*>("sets");
    REQUIRE(sets != nullptr);
    int alienRow = -1;
    for (int i = 0; i < sets->rowCount(); ++i) {
        if (sets->item(i, 0)->data(Qt::UserRole).toString() == "Alien Collection") {
            alienRow = i;
        }
    }
    REQUIRE(alienRow >= 0);
    REQUIRE_THAT(sets->item(alienRow, 0)->toolTip(), Contains("Alien Collection"));

    answerNextQuestion(QMessageBox::Yes);
    sets->item(alienRow, 0)->setText("Predator Collection");

    // The row is the target now, which has no divergence, so it explains none.
    REQUIRE(sets->rowCount() == 1);
    CHECK(sets->item(0, 0)->data(Qt::UserRole).toString() == "Predator Collection");
    CHECK(sets->item(0, 0)->toolTip().isEmpty());
}

TEST_CASE("Add Movie Set does not reuse a name a display title already holds", "[ui][movie][set]")
{
    // The uniquifier asked MovieSetModel::set(), which matches keys only, so a set whose
    // *display title* was already "New Movie Set" let this create a second row the user
    // cannot tell apart from it -- the state the rename guard exists to prevent, one path
    // over.  Both now go through the same predicate.
    MovieSetFolderGuard guard;

    addLibraryMovie("Alien", "Alien Collection");
    MovieSetModel* setModel = Manager::instance()->movieSetModel();
    MovieSet* alien = setModel->set("Alien Collection");
    REQUIRE(alien != nullptr);
    alien->setTitle("New Movie Set");

    SetsWidget widget;
    widget.loadSets();
    REQUIRE(QMetaObject::invokeMethod(&widget, "onAddMovieSet"));

    // The new set took the next free name rather than the taken one.
    CHECK(setModel->set("New Movie Set") == nullptr);
    CHECK(setModel->set("New Movie Set 1") != nullptr);

    // And no two rows read the same.
    auto* sets = widget.findChild<QTableWidget*>("sets");
    REQUIRE(sets != nullptr);
    QStringList shown;
    for (int i = 0; i < sets->rowCount(); ++i) {
        shown << sets->item(i, 0)->text();
    }
    QStringList unique = shown;
    unique.removeDuplicates();
    CHECK(unique.size() == shown.size());
}
