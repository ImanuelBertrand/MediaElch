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
#include <QSet>
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
        // A QTemporaryDir rather than the test temp root: the unit test binary is not given
        // --temp-dir, so that root is the working directory, i.e. the source tree.
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
        // The set model is a singleton and keeps whatever this test created; reload it so
        // the next test starts without records and without memberless sets.
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
/// \details The enumeration skips it, so a save must refuse rather than overwrite it.
void writeMisfiledRecord(const QDir& msif, const QString& folder, const QString& otherSetName)
{
    REQUIRE(QDir().mkpath(msif.absoluteFilePath(folder)));
    QFile record(msif.absoluteFilePath(folder + "/set.nfo"));
    REQUIRE(record.open(QIODevice::WriteOnly));
    record.write(QString("<set><originaltitle>%1</originaltitle></set>").arg(otherSetName).toUtf8());
    record.close();
}

/// \brief Hands \p widget a downloaded image for \p setName, as the download manager does.
/// \details The only production path that leaves an image in the tab's own maps unwritten.
void seedDownloadedImage(SetsWidget& widget, const QString& setName, ImageType imageType, const QImage& image)
{
    DownloadManagerElement elem;
    // Owned by the slot; the download manager carries a set's name as the movie's title.
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

/// \brief The notifications raised while this object was alive, and only those.
/// \details Scoped rather than read wholesale: NotificationBox is a singleton whose
///          messages expire on a timer that never fires here, so reading all of them returns
///          every message the process ever raised.  The old ones are skipped by pointer and
///          never deleted; NotificationBox owns those widgets and its list of them would dangle.
class NotificationWatcher
{
public:
    NotificationWatcher() : m_before{currentLabels()} {}
    NotificationWatcher(const NotificationWatcher&) = delete;
    NotificationWatcher& operator=(const NotificationWatcher&) = delete;

    /// \brief The text of every notification raised since construction, joined.
    ELCH_NODISCARD QString text() const
    {
        QStringList texts;
        for (const QLabel* label : NotificationBox::instance()->findChildren<QLabel*>()) {
            if (!m_before.contains(label) && !label->text().isEmpty()) {
                texts << label->text();
            }
        }
        return texts.join("\n");
    }

private:
    static QSet<const QLabel*> currentLabels()
    {
        QSet<const QLabel*> labels;
        for (const QLabel* label : NotificationBox::instance()->findChildren<QLabel*>()) {
            labels.insert(label);
        }
        return labels;
    }

    QSet<const QLabel*> m_before;
};

/// \brief Answers the next modal question box by clicking \p button.
/// \details Posted before the call that opens the box, because exec() spins a nested loop
///          that runs the timer at once.  Any other modal is closed, so a wrong expectation
///          fails rather than hangs.
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

/// \brief The library movie called \p title, or nullptr.
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

    // Read as a file rather than through loadMovieSet(): the record is misfiled, so the media
    // center resolves it to a path that never existed and answers "not found" either way.
    QFile record(guard.dir().absoluteFilePath("Alien Collection/set.nfo"));
    REQUIRE(record.open(QIODevice::ReadOnly));
    const QString onDisk = QString::fromUtf8(record.readAll());
    record.close();
    CHECK_THAT(onDisk, Contains("Something Else Entirely"));
}

TEST_CASE("The sets tab reports a save that succeeded", "[ui][movie][set]")
{
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
    // clearing that entry for a write that did not happen destroys the image.
    MovieSetFolderGuard guard;
    test::DataFileGuard dataFiles;

    addLibraryMovie("Alien", "Alien Collection");

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

    // With no folder there are no records either, which saveSet() tells apart from a record
    // it could not write, so this lands in the artwork-only branch of the three.
    Settings::instance()->setMovieSetArtworkDirectory(mediaelch::DirectoryPath());
    REQUIRE_FALSE(Manager::instance()->mediaCenterInterface()->movieSetArtworkEnabled());

    {
        test::MessageCapture messages;
        widget.saveSet();
        CHECK(messages.contains("its artwork could not be written"));
        // All three branches say "could not be written", so name which one this is.
        CHECK_FALSE(messages.contains("movie set file"));
    }

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
    // With no folder a set can have no `set.nfo`, and a set with neither members nor a record
    // is dropped by the next reload (third section), so this would offer a set that vanishes.
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
        // Both halves, so a future rearrangement of the menu cannot reopen this.
        Settings::instance()->setMovieSetArtworkType(MovieSetArtworkType::ArtworkNextToMovies);
        widget.loadSets();
        CHECK_FALSE(addSet->isEnabled());
        REQUIRE(QMetaObject::invokeMethod(&widget, "onAddMovieSet"));
        CHECK(setModel->set("New Movie Set") == nullptr);
    }

    SECTION("Because such a set does not survive the next reload")
    {
        Settings::instance()->setMovieSetArtworkType(MovieSetArtworkType::ArtworkNextToMovies);
        widget.loadSets();
        REQUIRE(setModel->addSet("New Movie Set") != nullptr);
        widget.loadSets();
        CHECK(setModel->set("New Movie Set") == nullptr);
    }
}

TEST_CASE("A set named on a movie is created without a movie set directory", "[ui][movie][set]")
{
    // Why the guard above is on *Add Movie Set* and not on set creation: a set named on a
    // movie has a member from the moment it exists, so nothing drops it.
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

        CHECK_THAT(notice->text(), Contains("Set artwork cannot be saved"));
        CHECK_THAT(notice->text(), Contains("no file of their own"));
        CHECK_THAT(notice->text(), Contains("no movies cannot be created"));

        // The tab is not read-only: renaming, moving movies in and out and the sort title
        // all write the member movies, which need no directory.
        CHECK_THAT(notice->text(), Contains("Renaming a set"));
        CHECK_THAT(notice->text(), !Contains("read-only"));
        // Neither has an editor yet, so naming them describes a loss the user cannot feel.
        CHECK_THAT(notice->text(), !Contains("TMDB"));
    }

    SECTION("Artwork next to movies is not a warning, because nothing is wrong")
    {
        // Every user who never opened the settings sees this line.
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
    // The directory can change while this tab is on screen, and sigSettingsSaved is the only
    // signal there is -- it fires for unrelated saves too, so the handler compares first.
    MovieSetFolderGuard guard;
    addLibraryMovie("Alien", "Alien Collection");

    SetsWidget widget;
    // Shown, off screen: the reload is conditional on isVisible(), and a widget that was
    // never shown reports false, so without this the reload half is untested.
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

        Settings::instance()->setMovieSetArtworkType(MovieSetArtworkType::SeparateArtworkFolder);
        announceSettingsSaved();
        CHECK(addSet->isEnabled());
        CHECK(frame->isHidden());
    }

    SECTION("Choosing a directory finds the sets only a record knows about")
    {
        // Why a reload and not just a re-enable: a set with a `set.nfo` and no member movie is
        // invisible until the folder has been listed, and only reload() lists it.
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
    // While records are off, isBacked() answers false for every set, so a reload() at that
    // moment would drop every memberless one.  Adding one there is what this test stops.
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

    // The flag survives, so turning the directory back on restores every set at once.
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
    // saveMovieSet() answers false both for "no records in this configuration" and for "the
    // record could not be written", so the caller has to tell them apart.
    MovieSetFolderGuard guard;
    Settings::instance()->setMovieSetArtworkType(MovieSetArtworkType::ArtworkNextToMovies);
    addLibraryMovie("Alien", "Alien Collection");

    SetsWidget widget;
    widget.loadSets();
    REQUIRE_FALSE(Manager::instance()->movieSetModel()->recordsAreConfigured());

    test::MessageCapture messages;
    widget.saveSet();

    CHECK_FALSE(messages.contains("could not be written"));
    CHECK_FALSE(messages.contains("Not saving the record"));
}

TEST_CASE("Set artwork is off only where it has nowhere to go", "[ui][movie][set]")
{
    // The artwork guard asks whether the layout resolves to a real path, not whether records
    // are configured: gating on records would turn artwork off in the shipping default.
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

        // The slots refuse on their own too, and log different lines, so removing one guard
        // and leaving the other cannot pass.  Removing a guard crashes rather than hangs: the
        // slot reaches ImageDialog, whose constructor dereferences the unit binary's null
        // MainWindow::instance().  The closer below is unverified insurance for the day a
        // unit test does build a MainWindow, and is drained after each call rather than left
        // in the queue for an unrelated test.
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
    // saveSet()'s third failure branch: records configured, and both writes fail.
    MovieSetFolderGuard guard;
    test::DataFileGuard dataFiles;

    addLibraryMovie("Alien", "Alien Collection");

    QImage poster(4, 4, QImage::Format_RGB32);
    poster.fill(Qt::red);

    SetsWidget widget;
    widget.loadSets();
    auto* sets = widget.findChild<QTableWidget*>("sets");
    REQUIRE(sets != nullptr);
    REQUIRE(sets->rowCount() == 1);
    seedDownloadedImage(widget, "Alien Collection", ImageType::MovieSetPoster, poster);

    // A plain file where the directory should be: still a valid DirectoryPath, so records stay
    // enabled, but nothing can be created under it.  Not a chmod, which proves nothing as root.
    const QString blockerPath = guard.dir().absoluteFilePath("blocker");
    QFile blocker(blockerPath);
    REQUIRE(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    Settings::instance()->setMovieSetArtworkDirectory(mediaelch::DirectoryPath(QDir(blockerPath)));
    REQUIRE(Manager::instance()->movieSetModel()->recordsAreConfigured());

    test::MessageCapture messages;
    widget.saveSet();

    // Named separately: all three branches say "could not be written", only this says both.
    CHECK(messages.contains("its artwork could not be written"));
    CHECK(messages.contains("neither could its movie set file"));
    CHECK(messages.contains("could not be written"));
}

TEST_CASE("A set-file-only rename never touches a movie", "[ui][movie][set]")
{
    // <title> moves and the match key does not; see docs/concepts/movie-sets.md.
    MovieSetFolderGuard guard;
    RenameModeGuard renameMode(MovieSetRenameMode::SetFileOnly);

    addLibraryMovie("Alien", "Alien Collection");

    SetsWidget widget;
    widget.loadSets();
    renameFirstSet(widget, "The Alien Saga");

    MovieSetModel* setModel = Manager::instance()->movieSetModel();

    SECTION("the key stays where the member movies put it")
    {
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
        // This rename writes nothing into a movie's NFO, so dirtying one would offer to
        // rewrite a file for no reason.
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
        // loadSet() takes the match key, which is what callers can look a set up by, but the
        // heading is read by a person.
        auto* heading = widget.findChild<QLabel*>("setName");
        REQUIRE(heading != nullptr);
        CHECK(heading->text() == "The Alien Saga");
    }

    SECTION("the divergence is explained at once, not at the next reload")
    {
        // The rename has to set the tooltip, or it is missing for exactly the rename that
        // creates the divergence.
        auto* sets = widget.findChild<QTableWidget*>("sets");
        REQUIRE(sets->rowCount() == 1);
        CHECK_THAT(sets->item(0, 0)->toolTip(), Contains("Alien Collection"));

        Settings::instance()->setMovieSetRenameMode(MovieSetRenameMode::AllMovieFiles);
        sets->item(0, 0)->setText("Alien Anthology");
        CHECK(sets->item(0, 0)->toolTip().isEmpty());
    }

    SECTION("the record is written under the key's folder, not the title's")
    {
        // Kodi derives the folder from the match key before it loads the record, so a record
        // filed under the display title is one it never looks at.
        widget.saveSet();
        CHECK(QFileInfo::exists(guard.dir().absoluteFilePath("Alien Collection/set.nfo")));
        CHECK_FALSE(QFileInfo::exists(guard.dir().absoluteFilePath("The Alien Saga/set.nfo")));
    }

    SECTION("it is rebuilt from its file, which is the failure the second string exists for")
    {
        // clear() rather than a plain loadSets(): reload() keeps the MovieSet objects it has
        // and re-reads a record only on a false->true hasRecord transition, which saveSet()
        // has just used up, so loadSets() alone would pass without the reader ever running.
        widget.saveSet();

        MovieSetModel* setModel = Manager::instance()->movieSetModel();
        setModel->clear();
        REQUIRE(setModel->set("Alien Collection") == nullptr);

        widget.loadSets();

        auto* sets = widget.findChild<QTableWidget*>("sets");
        REQUIRE(sets->rowCount() == 1);
        CHECK(sets->item(0, 0)->text() == "The Alien Saga");
        CHECK(sets->item(0, 0)->data(Qt::UserRole).toString() == "Alien Collection");
        MovieSet* rebuilt = setModel->set("Alien Collection");
        REQUIRE(rebuilt != nullptr);
        CHECK(rebuilt->title() == "The Alien Saga");
    }
}

TEST_CASE("A set-file-only rename with nowhere to write it is refused", "[ui][movie][set]")
{
    // No `set.nfo` for the display title to live in, and downgrading to the all-movie-files
    // rename would be the heavier operation this setting exists to avoid.
    RenameModeGuard renameMode(MovieSetRenameMode::SetFileOnly);
    REQUIRE(Settings::instance()->movieSetArtworkType() == MovieSetArtworkType::ArtworkNextToMovies);

    addLibraryMovie("Alien", "Alien Collection");

    SetsWidget widget;
    widget.loadSets();

    test::MessageCapture messages;
    renameFirstSet(widget, "The Alien Saga");

    CHECK(messages.contains("was not renamed"));
    CHECK(messages.contains("movie set information folder"));

    MovieSetModel* setModel = Manager::instance()->movieSetModel();
    MovieSet* set = setModel->set("Alien Collection");
    REQUIRE(set != nullptr);
    CHECK(set->displayName() == "Alien Collection");
    CHECK(setModel->set("The Alien Saga") == nullptr);

    Movie* alien = libraryMovie("Alien");
    REQUIRE(alien != nullptr);
    CHECK(alien->set().name == "Alien Collection");
    CHECK_FALSE(alien->hasChanged());

    // And the cell is put back, rather than showing a name nothing answers to.
    auto* sets = widget.findChild<QTableWidget*>("sets");
    CHECK(sets->item(0, 0)->text() == "Alien Collection");

    Manager::instance()->movieModel()->clear();
    qApp->processEvents();
    setModel->reload();
}

TEST_CASE("A rename onto a display title already in use is refused in both modes", "[ui][movie][set]")
{
    // Typing another set's key is a merge; typing its display title is not, because no set
    // answers to that name, but it would leave two rows the user cannot tell apart.
    MovieSetFolderGuard guard;
    RenameModeGuard renameMode(MovieSetRenameMode::SetFileOnly);

    addLibraryMovie("Alien", "Alien Collection");
    addLibraryMovie("Predator", "Predator Collection");

    MovieSetModel* setModel = Manager::instance()->movieSetModel();

    SetsWidget widget;
    widget.loadSets();

    MovieSet* predator = setModel->set("Predator Collection");
    REQUIRE(predator != nullptr);
    predator->setTitle("The Hunt");
    widget.loadSets();

    auto* sets = widget.findChild<QTableWidget*>("sets");
    REQUIRE(sets != nullptr);
    REQUIRE(sets->rowCount() == 2);
    // Rows are sorted by what the user reads, so find this one by its key.
    int alienRow = -1;
    for (int i = 0; i < sets->rowCount(); ++i) {
        if (sets->item(i, 0)->data(Qt::UserRole).toString() == "Alien Collection") {
            alienRow = i;
        }
    }
    REQUIRE(alienRow >= 0);

    // Both modes: under all movie files the typed name becomes the key, so the collision is
    // what the next reload rebuilds from.
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
        // Left behind, the old folder is listed at the next reload and resurrected as a ghost.
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
        // MovieSetImages knows two types; Kodi reads six more, and a user may have added any.
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
    // Something already stands where the set's folder would go, so QDir::rename() refuses.
    // Artwork rather than a record, because a folder holding a record would make this a merge.
    REQUIRE(QDir().mkpath(guard.dir().absoluteFilePath("Alien Anthology")));
    QFile occupant(guard.dir().absoluteFilePath("Alien Anthology/folder.jpg"));
    REQUIRE(occupant.open(QIODevice::WriteOnly));
    occupant.write("someone else's artwork");
    occupant.close();

    SetsWidget widget;
    widget.loadSets();

    const NotificationWatcher notifications;
    test::MessageCapture messages;
    renameFirstSet(widget, "Alien Anthology");

    CHECK(messages.contains("could not be moved"));
    // Nothing moved at all, so "still under the old name" is true for this branch.
    const QString shown = notifications.text();
    CHECK_THAT(shown, Contains("could not be moved"));
    CHECK_THAT(shown, Contains("Alien Collection"));

    // The rename still happened: undoing it would rewrite every member NFO again.
    Movie* alien = libraryMovie("Alien");
    REQUIRE(alien != nullptr);
    CHECK(alien->set().name == "Alien Anthology");

    CHECK(QFileInfo::exists(guard.dir().absoluteFilePath("Alien Collection/set.nfo")));
    CHECK(QFileInfo::exists(guard.dir().absoluteFilePath("Alien Anthology/folder.jpg")));
}

TEST_CASE("A rename whose folder moved but whose artwork did not says which", "[ui][movie][set]")
{
    // The directory is renamed first and the files inside it after, so the folder can move
    // while a file in it does not -- and "still under the old name" would then be a lie.
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
    // Already under the name the poster wants, so the in-folder rename refuses after the
    // directory rename has succeeded.
    QFile blocker(guard.dir().absoluteFilePath("Alien Collection/" + posterFile.saveFileName("Alien Anthology")));
    REQUIRE(blocker.open(QIODevice::WriteOnly));
    blocker.write("in the way");
    blocker.close();

    SetsWidget widget;
    widget.loadSets();

    const NotificationWatcher notifications;
    test::MessageCapture messages;
    renameFirstSet(widget, "Alien Anthology");

    CHECK(messages.contains("only some of its files moved"));
    // And *not* the message that would send the user to the old folder.
    CHECK_FALSE(messages.contains("could not be moved at all"));

    // The sentence the user is shown, not just the log line.
    const QString shown = notifications.text();
    CHECK_THAT(shown, Contains("only some of its files could be moved"));
    CHECK_THAT(shown, Contains("still"));
    // It must not claim the set is stored under the old name -- the folder moved.
    CHECK_THAT(shown, ContainsNot("Alien Collection"));

    CHECK(QFileInfo::exists(guard.dir().absoluteFilePath("Alien Anthology/set.nfo")));
    CHECK_FALSE(QFileInfo::exists(guard.dir().absoluteFilePath("Alien Collection/set.nfo")));
}

TEST_CASE("An all-movie-files rename moves artwork next to the movies too", "[ui][movie][set]")
{
    // With no per-set folder the anchor movie is found through the old name, so this works
    // only because the files move before the members are reassigned.
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
    // A merge moves movies between sets, cannot be undone, and is one typo away.
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
        // Membership lives in the members' NFOs, so a "set file only" merge would write a
        // display title and quietly not merge.
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
    // An empty name used to detach every member and dirty it, so the next save wrote an empty
    // <set> into every member NFO.  Both modes, because the guard is one shared precondition.
    MovieSetFolderGuard guard;
    const MovieSetRenameMode mode =
        GENERATE(MovieSetRenameMode::SetFileOnly, MovieSetRenameMode::AllMovieFiles);
    RenameModeGuard renameMode(mode);

    addLibraryMovie("Alien", "Alien Collection");
    addLibraryMovie("Aliens", "Alien Collection");

    SetsWidget widget;
    widget.loadSets();

    const NotificationWatcher notifications;
    test::MessageCapture messages;
    renameFirstSet(widget, "");

    CHECK(messages.contains("was not renamed"));
    CHECK(messages.contains("empty name"));
    CHECK_THAT(notifications.text(), Contains("cannot have an empty name"));

    MovieSetModel* setModel = Manager::instance()->movieSetModel();
    MovieSet* set = setModel->set("Alien Collection");
    REQUIRE(set != nullptr);
    CHECK(set->movies().size() == 2);
    CHECK(set->displayName() == "Alien Collection");

    // No movie detached or marked for rewriting -- the data loss itself.
    for (const QString& title : {QString("Alien"), QString("Aliens")}) {
        Movie* movie = libraryMovie(title);
        REQUIRE(movie != nullptr);
        CHECK(movie->set().name == "Alien Collection");
        CHECK_FALSE(movie->hasChanged());
    }

    auto* sets = widget.findChild<QTableWidget*>("sets");
    REQUIRE(sets->rowCount() == 1);
    CHECK(sets->item(0, 0)->text() == "Alien Collection");
    CHECK(sets->item(0, 0)->data(Qt::UserRole).toString() == "Alien Collection");
}

TEST_CASE("A merge leaves the row explaining the set it actually shows", "[ui][movie][set]")
{
    // performMerge() is the third place a row's identity changes, so it too redoes the tooltip.
    MovieSetFolderGuard guard;

    addLibraryMovie("Alien", "Alien Collection");
    addLibraryMovie("Predator", "Predator Collection");

    MovieSetModel* setModel = Manager::instance()->movieSetModel();

    SetsWidget widget;
    widget.loadSets();

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

    REQUIRE(sets->rowCount() == 1);
    CHECK(sets->item(0, 0)->data(Qt::UserRole).toString() == "Predator Collection");
    CHECK(sets->item(0, 0)->toolTip().isEmpty());
}

TEST_CASE("Add Movie Set does not reuse a name a display title already holds", "[ui][movie][set]")
{
    // The uniquifier has to see display titles too, or it creates the indistinguishable row
    // the rename guard exists to prevent.
    MovieSetFolderGuard guard;

    addLibraryMovie("Alien", "Alien Collection");
    MovieSetModel* setModel = Manager::instance()->movieSetModel();
    MovieSet* alien = setModel->set("Alien Collection");
    REQUIRE(alien != nullptr);
    alien->setTitle("New Movie Set");

    SetsWidget widget;
    widget.loadSets();
    REQUIRE(QMetaObject::invokeMethod(&widget, "onAddMovieSet"));

    CHECK(setModel->set("New Movie Set") == nullptr);
    CHECK(setModel->set("New Movie Set 1") != nullptr);

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
