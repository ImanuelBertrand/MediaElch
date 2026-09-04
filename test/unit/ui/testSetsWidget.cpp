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
#include "ui/main/MainWindow.h"
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
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
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

/// \brief Selects the first set and returns its movie table, populated.
QTableWidget* selectFirstSet(SetsWidget& widget)
{
    auto* sets = widget.findChild<QTableWidget*>("sets");
    REQUIRE(sets != nullptr);
    REQUIRE(sets->rowCount() >= 1);
    sets->setCurrentCell(0, 0);
    auto* movies = widget.findChild<QTableWidget*>("movies");
    REQUIRE(movies != nullptr);
    return movies;
}

/// \brief Takes \p title out of the set shown in \p widget, as the Remove button does.
void removeMovieFromSet(SetsWidget& widget, const QString& title)
{
    QTableWidget* movies = selectFirstSet(widget);
    int row = -1;
    for (int i = 0; i < movies->rowCount(); ++i) {
        if (movies->item(i, 0)->text() == title) {
            row = i;
        }
    }
    REQUIRE(row >= 0);
    movies->setCurrentCell(row, 0);
    REQUIRE(QMetaObject::invokeMethod(&widget, "onRemoveMovie", Qt::DirectConnection));
}

/// \brief Puts a movie with a file of its own into the library and returns it.
/// \details addLibraryMovie()'s movies have no files, which KodiXml refuses to write at all;
///          a save that is measured on disk needs a movie it accepts.
Movie* addLibraryMovieWithFile(const QDir& dir, const QString& title, const QString& setName)
{
    QFile file(dir.absoluteFilePath(title + ".mkv"));
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.close();
    auto* movie = new Movie(QStringList{file.fileName()}, nullptr);
    movie->setTitle(title);
    MovieSetInfo info;
    info.name = setName;
    movie->setSetInfo(info);
    movie->setChanged(false);
    Manager::instance()->movieModel()->addMovie(movie);
    return movie;
}

/// \brief Types a sort title into the first movie of the set in row \p row.
/// \details One of the edits that queue a movie for saving under its set's name, and the
///          only one that leaves the movie in the set it is in.
void queueSortTitleEdit(SetsWidget& widget, int row, const QString& sortTitle)
{
    auto* sets = widget.findChild<QTableWidget*>("sets");
    REQUIRE(sets != nullptr);
    REQUIRE(row < sets->rowCount());
    sets->setCurrentCell(row, 0);
    auto* movies = widget.findChild<QTableWidget*>("movies");
    REQUIRE(movies != nullptr);
    REQUIRE(movies->rowCount() >= 1);
    movies->item(0, 1)->setText(sortTitle);
}

/// \brief The contents of the file \p path.
QString fileContents(const QString& path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    return QString::fromUtf8(file.readAll());
}

/// \brief Emits Settings::sigSettingsSaved without writing the user's real settings.
void announceSettingsSaved()
{
    REQUIRE(QMetaObject::invokeMethod(Settings::instance(), "sigSettingsSaved"));
}

/// \brief The sets tab's overview editor.
QPlainTextEdit* overviewEditor(SetsWidget& widget)
{
    auto* overview = widget.findChild<QPlainTextEdit*>("overview");
    REQUIRE(overview != nullptr);
    return overview;
}

/// \brief The sets tab's collection id field.
QLineEdit* tmdbIdEditor(SetsWidget& widget)
{
    auto* tmdbId = widget.findChild<QLineEdit*>("tmdbId");
    REQUIRE(tmdbId != nullptr);
    return tmdbId;
}

/// \brief Types \p text into \p lineEdit as a user does, one key at a time.
/// \details setText() would not do: QLineEdit::textEdited is a user-input signal and is
///          deliberately not emitted for a programmatic change, which is exactly what lets
///          loadSet() fill the field without writing it straight back.
void typeInto(QLineEdit* lineEdit, const QString& text)
{
    QTest::keyClicks(lineEdit, text);
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
        // This tab holds two types; Kodi reads six more, and a user may have added any.
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


TEST_CASE("Deleting a movie set asks before it deletes anything", "[ui][movie][set]")
{
    // The entry sits under Add Movie Set, removes a file from disk and takes every movie out
    // of the set; the far less destructive merge already asks.
    MovieSetFolderGuard guard;
    writeRecord(guard.dir(), "Alien Collection");
    addLibraryMovie("Alien", "Alien Collection");

    SetsWidget widget;
    widget.loadSets();
    auto* sets = widget.findChild<QTableWidget*>("sets");
    REQUIRE(sets != nullptr);
    REQUIRE(sets->rowCount() == 1);
    sets->setCurrentCell(0, 0);

    MovieSetModel* setModel = Manager::instance()->movieSetModel();
    Movie* alien = libraryMovie("Alien");
    REQUIRE(alien != nullptr);

    SECTION("declining leaves the set, its record and its movies alone")
    {
        answerNextQuestion(QMessageBox::No);
        REQUIRE(QMetaObject::invokeMethod(&widget, "onRemoveMovieSet", Qt::DirectConnection));

        MovieSet* movieSet = setModel->set("Alien Collection");
        REQUIRE(movieSet != nullptr);
        CHECK(movieSet->movies().size() == 1);
        CHECK(alien->set().name == "Alien Collection");
        CHECK_FALSE(alien->hasChanged());
        CHECK(QFileInfo::exists(guard.dir().absoluteFilePath("Alien Collection/set.nfo")));
        CHECK(sets->rowCount() == 1);
    }

    SECTION("accepting deletes the set and its record")
    {
        answerNextQuestion(QMessageBox::Yes);
        REQUIRE(QMetaObject::invokeMethod(&widget, "onRemoveMovieSet", Qt::DirectConnection));

        CHECK(setModel->set("Alien Collection") == nullptr);
        CHECK(alien->set().name.isEmpty());
        CHECK_FALSE(QFileInfo::exists(guard.dir().absoluteFilePath("Alien Collection/set.nfo")));
        CHECK(sets->rowCount() == 0);
    }
}

TEST_CASE("Deleting a movie set writes the movies it took out of it", "[ui][movie][set]")
{
    // The record goes from disk at once, so the other half -- clearing <set><name> in each
    // member -- cannot be left to a tab the user may never open: this one's Save is gone with
    // the row.
    MovieSetFolderGuard guard;
    addLibraryMovie("Alien", "Alien Collection");
    addLibraryMovie("Aliens", "Alien Collection");

    SetsWidget widget;
    widget.loadSets();
    auto* sets = widget.findChild<QTableWidget*>("sets");
    REQUIRE(sets != nullptr);
    REQUIRE(sets->rowCount() == 1);
    sets->setCurrentCell(0, 0);

    // Queued under the set's name and no longer a member: the removal has to write it too.
    removeMovieFromSet(widget, "Aliens");
    Movie* aliens = libraryMovie("Aliens");
    REQUIRE(aliens != nullptr);
    REQUIRE(aliens->hasChanged());

    answerNextQuestion(QMessageBox::Yes);
    REQUIRE(QMetaObject::invokeMethod(&widget, "onRemoveMovieSet", Qt::DirectConnection));

    // saveData() clears the flag whatever the write itself did, so this is "the tab saved it".
    for (const QString& title : {QString("Alien"), QString("Aliens")}) {
        Movie* movie = libraryMovie(title);
        REQUIRE(movie != nullptr);
        CHECK(movie->set().name.isEmpty());
        CHECK(movie->sortTitle().isEmpty());
        CHECK_FALSE(movie->hasChanged());
    }
}

TEST_CASE("Deleting a movie set reports a movie whose NFO could not be written", "[ui][movie][set]")
{
    // The set's file is already gone and saveData() clears the changed flag whether it wrote
    // anything or not, so a failed write would otherwise leave no trace anywhere.
    MovieSetFolderGuard guard;
    // A movie with no files is one KodiXml refuses to write, which is the failure under test.
    addLibraryMovie("Alien", "Alien Collection");

    SetsWidget widget;
    widget.loadSets();
    auto* sets = widget.findChild<QTableWidget*>("sets");
    REQUIRE(sets != nullptr);
    REQUIRE(sets->rowCount() == 1);
    sets->setCurrentCell(0, 0);

    NotificationWatcher notifications;
    test::MessageCapture messages;
    answerNextQuestion(QMessageBox::Yes);
    REQUIRE(QMetaObject::invokeMethod(&widget, "onRemoveMovieSet", Qt::DirectConnection));

    CHECK(sets->rowCount() == 0);
    CHECK(notifications.text().contains("not every movie could be written"));
    CHECK(messages.contains("was deleted but not every movie could be written"));
}

TEST_CASE("A rename keeps the movies queued for saving under the old name", "[ui][movie][set]")
{
    // A movie taken out of the set is queued under the old name and is not a member any more,
    // so carrying the members over does not carry it and clearing the old queue drops its
    // write -- leaving the set name it was taken out of in its NFO.
    MovieSetFolderGuard guard;
    addLibraryMovie("Alien", "Alien Collection");
    addLibraryMovie("Aliens", "Alien Collection");
    addLibraryMovie("Predator", "Predator Collection");

    SetsWidget widget;
    widget.loadSets();
    removeMovieFromSet(widget, "Aliens");

    Movie* aliens = libraryMovie("Aliens");
    REQUIRE(aliens != nullptr);
    REQUIRE(aliens->set().name.isEmpty());
    REQUIRE(aliens->hasChanged());

    SECTION("an all-movie-files rename")
    {
        RenameModeGuard renameMode(MovieSetRenameMode::AllMovieFiles);

        renameFirstSet(widget, "Alien Anthology");
        widget.saveSet();

        CHECK_FALSE(aliens->hasChanged());
    }

    SECTION("a merge")
    {
        answerNextQuestion(QMessageBox::Yes);
        renameFirstSet(widget, "Predator Collection");
        widget.saveSet();

        CHECK_FALSE(aliens->hasChanged());
    }

    Movie* alien = libraryMovie("Alien");
    REQUIRE(alien != nullptr);
    CHECK_FALSE(alien->hasChanged());
}

TEST_CASE("Save All writes the movies queued in every movie set", "[ui][movie][set]")
{
    // The point of Save All: the per-set Save reaches the selected row only.
    MovieSetFolderGuard guard;
    test::DataFileGuard dataFiles;
    QTemporaryDir movieFiles;
    REQUIRE(movieFiles.isValid());
    const QDir movieDir(movieFiles.path());

    addLibraryMovieWithFile(movieDir, "Alien", "Alien Collection");
    addLibraryMovieWithFile(movieDir, "Predator", "Predator Collection");

    SetsWidget widget;
    widget.loadSets();
    auto* sets = widget.findChild<QTableWidget*>("sets");
    REQUIRE(sets != nullptr);
    REQUIRE(sets->rowCount() == 2);

    queueSortTitleEdit(widget, 0, "Alien 1");
    queueSortTitleEdit(widget, 1, "Predator 1");

    widget.saveAllSets();

    // Both NFOs, and each naming its own set: the movie the selected row does not hold is
    // exactly the one a single-set save leaves behind.
    REQUIRE(QFileInfo::exists(movieDir.absoluteFilePath("Alien.nfo")));
    CHECK_THAT(fileContents(movieDir.absoluteFilePath("Alien.nfo")), Contains("<name>Alien Collection</name>"));
    REQUIRE(QFileInfo::exists(movieDir.absoluteFilePath("Predator.nfo")));
    CHECK_THAT(fileContents(movieDir.absoluteFilePath("Predator.nfo")), Contains("<name>Predator Collection</name>"));
}

TEST_CASE("Save All does not give an untouched movie set a record", "[ui][movie][set]")
{
    // A set with a `set.nfo` is never dropped for having no members, so writing one for every
    // set would make every name the library has ever grouped by permanent.  This is why Save
    // All is deliberately not the sum of the per-set Saves.
    MovieSetFolderGuard guard;
    addLibraryMovie("Alien", "Alien Collection");

    SetsWidget widget;
    widget.loadSets();

    MovieSetModel* setModel = Manager::instance()->movieSetModel();
    REQUIRE(setModel->recordsAreConfigured());
    MovieSet* movieSet = setModel->set("Alien Collection");
    REQUIRE(movieSet != nullptr);
    REQUIRE_FALSE(movieSet->hasChanged());
    REQUIRE_FALSE(movieSet->hasRecord());

    widget.saveAllSets();

    CHECK_FALSE(QFileInfo::exists(guard.dir().absoluteFilePath("Alien Collection/set.nfo")));
    CHECK_FALSE(movieSet->hasRecord());
}

TEST_CASE("Save All writes the record of a movie set that was edited", "[ui][movie][set]")
{
    // The other half of the condition, and the half that has nowhere else to go: an edit to a
    // set that has no `set.nfo` yet lives in the MovieSet object alone until one is written.
    MovieSetFolderGuard guard;
    addLibraryMovie("Alien", "Alien Collection");

    SetsWidget widget;
    widget.loadSets();

    MovieSet* movieSet = Manager::instance()->movieSetModel()->set("Alien Collection");
    REQUIRE(movieSet != nullptr);
    REQUIRE_FALSE(movieSet->hasRecord());
    // Through the entity's own setter, which is what marks a set changed everywhere in the
    // application; the flag itself is never written by hand.
    movieSet->setOverview("Ellen Ripley versus the xenomorphs.");
    REQUIRE(movieSet->hasChanged());

    widget.saveAllSets();

    REQUIRE(QFileInfo::exists(guard.dir().absoluteFilePath("Alien Collection/set.nfo")));
    CHECK_THAT(fileContents(guard.dir().absoluteFilePath("Alien Collection/set.nfo")),
        Contains("Ellen Ripley versus the xenomorphs."));
    // The set and its file agree now, which is what makes the edit safe to forget.
    CHECK(movieSet->hasRecord());
    CHECK_FALSE(movieSet->hasChanged());
}

TEST_CASE("Save All rewrites the record a movie set already has", "[ui][movie][set]")
{
    // The other half of the rule: a set that is backed already has nothing left to lose by
    // being written, and its overview and id must not be left behind by a Save All.
    MovieSetFolderGuard guard;
    writeRecord(guard.dir(), "Alien Collection");
    addLibraryMovie("Alien", "Alien Collection");

    SetsWidget widget;
    widget.loadSets();

    MovieSet* movieSet = Manager::instance()->movieSetModel()->set("Alien Collection");
    REQUIRE(movieSet != nullptr);
    REQUIRE(movieSet->hasRecord());
    REQUIRE_FALSE(movieSet->hasChanged());

    widget.saveAllSets();

    // The seed record is a single hand-written line; only MovieSetXmlWriter writes the
    // declaration, so its presence is the write having happened.
    CHECK_THAT(fileContents(guard.dir().absoluteFilePath("Alien Collection/set.nfo")), Contains("<?xml"));
}

TEST_CASE("The sets tab is granted the Save All action", "[ui][movie][set]")
{
    // MainWindow cannot be built here -- instance() is null in this binary and the widgets
    // dereference it -- so this pins the decision that onSetSaveEnabled() asks for and not
    // the wiring around it.
    CHECK(MainWindow::hasSaveAllAction(MainWidgets::MovieSets));
    // The neighbours, so that "always true" would not pass: Certifications is the one tab
    // whose Save All flag the same call withholds.
    CHECK(MainWindow::hasSaveAllAction(MainWidgets::Movies));
    CHECK_FALSE(MainWindow::hasSaveAllAction(MainWidgets::Certifications));
}

TEST_CASE("An edited set overview reaches every movie of the set", "[ui][movie][set]")
{
    // Kodi 19 to 21 never read `set.nfo`, so the overview has to reach every member NFO with
    // identical text -- and for a set that has no record they are the only place it is kept.
    MovieSetFolderGuard guard;

    addLibraryMovie("Alien", "Alien Collection");
    addLibraryMovie("Aliens", "Alien Collection");
    // A movie the set holds although its own NFO names another collection.  Possible through
    // the public MovieSet::addMovie(), and the member MovieSetModel::seedFromMembers() refuses
    // to read; the mirror has to refuse it by the same condition or it would write this set's
    // overview into a movie that belongs to another one.
    addLibraryMovie("Predator", "Predator Collection");

    SetsWidget widget;
    widget.loadSets();

    // After loadSets(), which reloads the model and regroups every movie by the name its own
    // NFO carries -- an odd membership made before it would simply be undone.
    MovieSetModel* setModel = Manager::instance()->movieSetModel();
    MovieSet* alienSet = setModel->set("Alien Collection");
    REQUIRE(alienSet != nullptr);
    Movie* predator = libraryMovie("Predator");
    REQUIRE(predator != nullptr);
    alienSet->addMovie(predator);
    REQUIRE(alienSet->movies().contains(predator));

    // Rows are sorted by display name, so "Alien Collection" comes before "Predator Collection".
    REQUIRE(selectFirstSet(widget) != nullptr);

    overviewEditor(widget)->setPlainText("Ellen Ripley versus the xenomorphs.");

    SECTION("the set itself carries it")
    {
        CHECK(alienSet->overview() == "Ellen Ripley versus the xenomorphs.");
        // Which is what makes Save All write a `set.nfo` for a set that has none yet.
        CHECK(alienSet->hasChanged());
    }

    SECTION("every member carries the same text and is offered for saving")
    {
        for (const QString& title : {QString("Alien"), QString("Aliens")}) {
            Movie* movie = libraryMovie(title);
            REQUIRE(movie != nullptr);
            INFO("movie: " << title.toStdString());
            CHECK(movie->set().overview == "Ellen Ripley versus the xenomorphs.");
            // assign() marking the movie changed is what makes the edit survive a Save issued
            // from the movies tab or the navbar rather than from this one.
            CHECK(movie->hasChanged());
        }
    }

    SECTION("a member that names another collection is left alone")
    {
        CHECK(predator->set().overview.isEmpty());
        CHECK(predator->set().name == "Predator Collection");
        CHECK_FALSE(predator->hasChanged());
    }

    SECTION("the divergence between members is overwritten, not preserved")
    {
        // The field is the *set's* overview.  seedFromMembers() is first-wins over disagreeing
        // members, so leaving the others alone would keep the set showing one text and its
        // movies holding another.
        Movie* aliens = libraryMovie("Aliens");
        REQUIRE(aliens != nullptr);
        MovieSetInfo diverging = aliens->set();
        diverging.overview = "Something else entirely.";
        setModel->assign(aliens, diverging);
        REQUIRE(aliens->set().overview == "Something else entirely.");

        overviewEditor(widget)->setPlainText("Ellen Ripley versus the xenomorphs, again.");
        CHECK(aliens->set().overview == "Ellen Ripley versus the xenomorphs, again.");
    }
}

TEST_CASE("An edited collection id reaches every movie of the set", "[ui][movie][set]")
{
    // #2012: the scraped collection id had nowhere to be edited and nothing pushed it into the
    // movies.  Same rule as the overview -- identical text in every member.
    MovieSetFolderGuard guard;

    addLibraryMovie("Alien", "Alien Collection");
    addLibraryMovie("Aliens", "Alien Collection");

    MovieSet* alienSet = Manager::instance()->movieSetModel()->set("Alien Collection");
    REQUIRE(alienSet != nullptr);

    SetsWidget widget;
    widget.loadSets();
    REQUIRE(selectFirstSet(widget) != nullptr);

    SECTION("a typed id reaches the set and every movie")
    {
        typeInto(tmdbIdEditor(widget), "8091");

        CHECK(alienSet->tmdbId() == TmdbId(8091));
        CHECK(alienSet->hasChanged());
        for (const QString& title : {QString("Alien"), QString("Aliens")}) {
            Movie* movie = libraryMovie(title);
            REQUIRE(movie != nullptr);
            INFO("movie: " << title.toStdString());
            CHECK(movie->set().tmdbId == TmdbId(8091));
            CHECK(movie->hasChanged());
        }
    }

    SECTION("an id that is not a number is held but never becomes valid")
    {
        // Defined rather than refused, as MovieWidget does with the movie's own id: TmdbId keeps
        // whatever string it was given, and neither NFO writer writes one that is not valid, so
        // a half-typed id is shown back to the user and reaches no file.
        typeInto(tmdbIdEditor(widget), "not a number");

        CHECK(alienSet->tmdbId().toString() == "not a number");
        CHECK_FALSE(alienSet->tmdbId().isValid());
        Movie* alien = libraryMovie("Alien");
        REQUIRE(alien != nullptr);
        CHECK(alien->set().tmdbId.toString() == "not a number");
        CHECK_FALSE(alien->set().tmdbId.isValid());
    }

    SECTION("emptying the field takes the id off the set and its movies")
    {
        typeInto(tmdbIdEditor(widget), "8");
        REQUIRE(alienSet->tmdbId() == TmdbId(8));
        QTest::keyClick(tmdbIdEditor(widget), Qt::Key_Backspace);

        CHECK(alienSet->tmdbId() == TmdbId::NoId);
        Movie* alien = libraryMovie("Alien");
        REQUIRE(alien != nullptr);
        CHECK(alien->set().tmdbId == TmdbId::NoId);
    }
}

TEST_CASE("A set edited in the panel is written into its movies' NFO files", "[ui][movie][set]")
{
    // The whole point of mirroring at edit time: a set with no `set.nfo` keeps its overview and
    // its id nowhere but in the movies, so an edit that never reaches them is an edit lost.
    MovieSetFolderGuard guard;
    test::DataFileGuard dataFiles;
    QTemporaryDir movieFiles;
    REQUIRE(movieFiles.isValid());
    const QDir movieDir(movieFiles.path());

    addLibraryMovieWithFile(movieDir, "Alien", "Alien Collection");

    SetsWidget widget;
    widget.loadSets();

    MovieSet* alienSet = Manager::instance()->movieSetModel()->set("Alien Collection");
    REQUIRE(alienSet != nullptr);
    REQUIRE_FALSE(alienSet->hasRecord());

    REQUIRE(selectFirstSet(widget) != nullptr);
    overviewEditor(widget)->setPlainText("Ellen Ripley versus the xenomorphs.");
    typeInto(tmdbIdEditor(widget), "8091");

    widget.saveSet();

    // The movie's NFO, which is the half Kodi 19 to 21 read.  Queued as well as assigned: an
    // edit that only marked the movie changed would not be written by this tab's own Save.
    REQUIRE(QFileInfo::exists(movieDir.absoluteFilePath("Alien.nfo")));
    const QString movieNfo = fileContents(movieDir.absoluteFilePath("Alien.nfo"));
    CHECK_THAT(movieNfo, Contains("<overview>Ellen Ripley versus the xenomorphs.</overview>"));
    CHECK_THAT(movieNfo, Contains("<tmdbcolid>8091</tmdbcolid>"));

    // And the set's own record, which is where Kodi 22 reads them.
    REQUIRE(QFileInfo::exists(guard.dir().absoluteFilePath("Alien Collection/set.nfo")));
    const QString record = fileContents(guard.dir().absoluteFilePath("Alien Collection/set.nfo"));
    CHECK_THAT(record, Contains("<overview>Ellen Ripley versus the xenomorphs.</overview>"));
    CHECK_THAT(record, Contains("8091"));
}

TEST_CASE("The sets panel shows the selected set's overview and id", "[ui][movie][set]")
{
    MovieSetFolderGuard guard;

    addLibraryMovie("Alien", "Alien Collection");
    addLibraryMovie("Predator", "Predator Collection");

    MovieSetModel* setModel = Manager::instance()->movieSetModel();
    MovieSet* alienSet = setModel->set("Alien Collection");
    MovieSet* predatorSet = setModel->set("Predator Collection");
    REQUIRE(alienSet != nullptr);
    REQUIRE(predatorSet != nullptr);
    alienSet->setOverview("Ellen Ripley versus the xenomorphs.");
    alienSet->setTmdbId(TmdbId(8091));
    predatorSet->setOverview("The Yautja hunt.");

    SetsWidget widget;
    widget.loadSets();
    auto* sets = widget.findChild<QTableWidget*>("sets");
    REQUIRE(sets != nullptr);
    REQUIRE(sets->rowCount() == 2);
    sets->setCurrentCell(0, 0);

    SECTION("selecting a set fills both fields")
    {
        CHECK(overviewEditor(widget)->toPlainText() == "Ellen Ripley versus the xenomorphs.");
        CHECK(tmdbIdEditor(widget)->text() == "8091");
    }

    SECTION("selecting another set shows its values and writes into neither")
    {
        sets->setCurrentCell(1, 0);

        CHECK(overviewEditor(widget)->toPlainText() == "The Yautja hunt.");
        CHECK(tmdbIdEditor(widget)->text().isEmpty());
        // The dangerous half: loadSet() empties the fields before it fills them, and by then the
        // table already points at the incoming row -- so an unblocked clear would blank that
        // set's overview and mirror the blank into all of its movies.
        CHECK(predatorSet->overview() == "The Yautja hunt.");
        CHECK(alienSet->overview() == "Ellen Ripley versus the xenomorphs.");
        CHECK(alienSet->tmdbId() == TmdbId(8091));
        Movie* predator = libraryMovie("Predator");
        REQUIRE(predator != nullptr);
        CHECK(predator->set().overview.isEmpty());
        CHECK_FALSE(predator->hasChanged());
    }

    SECTION("deselecting clears both fields and edits nothing")
    {
        sets->setCurrentCell(-1, -1);

        CHECK(overviewEditor(widget)->toPlainText().isEmpty());
        CHECK(tmdbIdEditor(widget)->text().isEmpty());
        CHECK(alienSet->overview() == "Ellen Ripley versus the xenomorphs.");
        Movie* alien = libraryMovie("Alien");
        REQUIRE(alien != nullptr);
        CHECK_FALSE(alien->hasChanged());
    }
}

TEST_CASE("The set overview and id stay editable without a movie set directory", "[ui][movie][set]")
{
    // Read-only without a directory is narrow: the set's own record and its artwork, not the
    // tab.  This edit is written into movie NFO files, which are writable in every layout.
    MovieSetFolderGuard guard;
    Settings::instance()->setMovieSetArtworkType(MovieSetArtworkType::ArtworkNextToMovies);

    addLibraryMovie("Alien", "Alien Collection");

    SetsWidget widget;
    widget.loadSets();
    REQUIRE_FALSE(Manager::instance()->movieSetModel()->recordsAreConfigured());

    CHECK(overviewEditor(widget)->isEnabled());
    CHECK_FALSE(overviewEditor(widget)->isReadOnly());
    CHECK(tmdbIdEditor(widget)->isEnabled());
    CHECK_FALSE(tmdbIdEditor(widget)->isReadOnly());
    // The neighbour this very layout really does switch off, so that "everything is enabled"
    // would not pass this.
    auto* addSet = widget.findChild<QAction*>("actionAddMovieSet");
    REQUIRE(addSet != nullptr);
    CHECK_FALSE(addSet->isEnabled());

    // And the edit still arrives where it is kept in this layout: in the movie.
    REQUIRE(selectFirstSet(widget) != nullptr);
    overviewEditor(widget)->setPlainText("Ellen Ripley versus the xenomorphs.");
    Movie* alien = libraryMovie("Alien");
    REQUIRE(alien != nullptr);
    CHECK(alien->set().overview == "Ellen Ripley versus the xenomorphs.");
    CHECK(alien->hasChanged());
}

TEST_CASE("An overview edited before a rename survives it", "[ui][movie][set]")
{
    // The edit is mirrored onto the members and queued under the set's name; an all-movie-files
    // rename moves both, so the write is neither lost nor left under a name nothing saves.
    MovieSetFolderGuard guard;
    RenameModeGuard renameMode(MovieSetRenameMode::AllMovieFiles);
    test::DataFileGuard dataFiles;
    QTemporaryDir movieFiles;
    REQUIRE(movieFiles.isValid());
    const QDir movieDir(movieFiles.path());

    addLibraryMovieWithFile(movieDir, "Alien", "Alien Collection");

    SetsWidget widget;
    widget.loadSets();
    REQUIRE(selectFirstSet(widget) != nullptr);
    overviewEditor(widget)->setPlainText("Ellen Ripley versus the xenomorphs.");

    renameFirstSet(widget, "Alien Anthology");

    MovieSet* renamed = Manager::instance()->movieSetModel()->set("Alien Anthology");
    REQUIRE(renamed != nullptr);
    CHECK(renamed->overview() == "Ellen Ripley versus the xenomorphs.");

    Movie* alien = libraryMovie("Alien");
    REQUIRE(alien != nullptr);
    CHECK(alien->set().name == "Alien Anthology");
    CHECK(alien->set().overview == "Ellen Ripley versus the xenomorphs.");

    // Queued under the new name, so the tab's own Save still writes it.
    widget.saveSet();
    REQUIRE(QFileInfo::exists(movieDir.absoluteFilePath("Alien.nfo")));
    const QString movieNfo = fileContents(movieDir.absoluteFilePath("Alien.nfo"));
    CHECK_THAT(movieNfo, Contains("<name>Alien Anthology</name>"));
    CHECK_THAT(movieNfo, Contains("<overview>Ellen Ripley versus the xenomorphs.</overview>"));
}

TEST_CASE("A merged movie carries the overview of the set it joined", "[ui][movie][set]")
{
    // The sibling path: a merge moves movies into another collection, so they must arrive with
    // that collection's text and not with the one they brought from the set that was merged away.
    MovieSetFolderGuard guard;
    RenameModeGuard renameMode(MovieSetRenameMode::AllMovieFiles);

    addLibraryMovie("Alien", "Alien Collection");
    addLibraryMovie("Predator", "Predator Collection");

    MovieSetModel* setModel = Manager::instance()->movieSetModel();
    MovieSet* alienSet = setModel->set("Alien Collection");
    REQUIRE(alienSet != nullptr);
    alienSet->setOverview("Ellen Ripley versus the xenomorphs.");
    alienSet->setTmdbId(TmdbId(8091));

    SetsWidget widget;
    widget.loadSets();
    auto* sets = widget.findChild<QTableWidget*>("sets");
    REQUIRE(sets != nullptr);
    REQUIRE(sets->rowCount() == 2);

    // Row 1 is "Predator Collection"; renaming it onto the other set's name is the merge.
    answerNextQuestion(QMessageBox::Yes);
    sets->item(1, 0)->setText("Alien Collection");

    Movie* predator = libraryMovie("Predator");
    REQUIRE(predator != nullptr);
    CHECK(predator->set().name == "Alien Collection");
    CHECK(predator->set().overview == "Ellen Ripley versus the xenomorphs.");
    CHECK(predator->set().tmdbId == TmdbId(8091));
}

TEST_CASE("A movie added to a set carries the overview of the set it joined", "[ui][movie][set]")
{
    // The sibling of the merge below.  onAddMovie() opens a modal first and does everything
    // else in addMoviesToSet(), which is what this drives; the dialog only decides *which*
    // movies, and it is the value they arrive with that is at stake here.
    MovieSetFolderGuard guard;

    addLibraryMovie("Alien", "Alien Collection");
    addLibraryMovie("Predator", "");

    SetsWidget widget;
    widget.loadSets();

    MovieSet* alienSet = Manager::instance()->movieSetModel()->set("Alien Collection");
    REQUIRE(alienSet != nullptr);
    alienSet->setOverview("Ellen Ripley versus the xenomorphs.");
    alienSet->setTmdbId(TmdbId(8091));

    REQUIRE(selectFirstSet(widget) != nullptr);
    Movie* predator = libraryMovie("Predator");
    REQUIRE(predator != nullptr);
    REQUIRE(predator->set().name.isEmpty());

    REQUIRE(QMetaObject::invokeMethod(
        &widget, "addMoviesToSet", Qt::DirectConnection, Q_ARG(QVector<Movie*>, QVector<Movie*>{predator})));

    CHECK(predator->set().name == "Alien Collection");
    CHECK(predator->set().overview == "Ellen Ripley versus the xenomorphs.");
    CHECK(predator->set().tmdbId == TmdbId(8091));
    CHECK(predator->hasChanged());

    // And it is queued, so this tab's own Save reaches it.
    CHECK(alienSet->movies().contains(predator));
}
