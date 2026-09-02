#include "test/test_helpers.h"

#include "data/movie/Movie.h"
#include "data/movie/MovieSet.h"
#include "globals/Manager.h"
#include "model/MovieModel.h"
#include "model/MovieSetModel.h"
#include "test/helpers/movie_set_settings.h"
#include "ui/movies/MovieFilesWidget.h"
#include "ui/movies/MovieWidget.h"

#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QFocusEvent>
#include <QTemporaryDir>

namespace {

/// \brief A one-movie library on disk, with the movie tab loaded on it.
/// \details The movie needs real files: MovieWidget's save re-reads the NFO afterwards, and
///          KodiXml::loadMovie() clears the movie before it looks for one, so a movie
///          without files comes back from a save empty and pins nothing.
class MovieTabFixture
{
public:
    explicit MovieTabFixture(const QString& setName)
    {
        REQUIRE(m_dir.isValid());
        writeFile("Alien.mkv", "not really a movie");
        writeFile("Alien.nfo", QString("<movie><title>Alien</title><set><name>%1</name></set></movie>").arg(setName));

        m_movie = new Movie(QStringList{m_dir.filePath("Alien.mkv")});
        Manager::instance()->movieModel()->addMovie(m_movie);
        m_tab.setMovie(m_movie);
        REQUIRE(m_movie->set().name == setName);
        REQUIRE(setBox()->currentText() == setName);
    }

    ~MovieTabFixture()
    {
        Manager::instance()->movieModel()->clear();
        qApp->processEvents();
        // The set model is a singleton and keeps whatever this test created.
        Manager::instance()->movieSetModel()->reload();
    }

    MovieTabFixture(const MovieTabFixture&) = delete;
    MovieTabFixture& operator=(const MovieTabFixture&) = delete;

    MovieWidget& tab() { return m_tab; }
    Movie* movie() const { return m_movie; }
    QComboBox* setBox() const { return m_tab.findChild<QComboBox*>("set"); }

    /// \brief What the movie's NFO on disk says its set is called.
    QString setInNfo() const
    {
        QFile nfo(m_dir.filePath("Alien.nfo"));
        REQUIRE(nfo.open(QIODevice::ReadOnly));
        const QString content = QString::fromUtf8(nfo.readAll());
        const int start = content.indexOf("<name>");
        const int end = content.indexOf("</name>", start);
        REQUIRE(start >= 0);
        REQUIRE(end > start);
        return content.mid(start + 6, end - start - 6);
    }

private:
    void writeFile(const QString& name, const QString& content) const
    {
        QFile file(m_dir.filePath(name));
        REQUIRE(file.open(QIODevice::WriteOnly));
        REQUIRE(file.write(content.toUtf8()) > 0);
    }

    test::DataFileGuard m_dataFiles;
    QTemporaryDir m_dir;
    // saveInformation() reads the selection out of MovieFilesWidget's singleton, which only
    // its constructor ever sets, so the file list has to exist even though nothing looks at it.
    MovieFilesWidget m_files;
    MovieWidget m_tab;
    Movie* m_movie = nullptr;
};

/// \brief Renames a set the way the sets tab does, leaving the movie tab's box untouched.
void renameSetElsewhere(const QString& oldName, const QString& newName)
{
    MovieSetModel* setModel = Manager::instance()->movieSetModel();
    MovieSet* set = setModel->set(oldName);
    REQUIRE(set != nullptr);
    const QVector<Movie*> members = set->movies();
    REQUIRE(setModel->renameSet(set, newName));
    for (Movie* movie : members) {
        setModel->assign(movie, movie->set().renamedTo(newName));
    }
}

} // namespace

TEST_CASE("Saving a movie does not undo a set renamed in the sets tab", "[ui][movie][set]")
{
    // The blocker: nothing refills the set combo when the user comes back to this tab, so a
    // save that commits it unconditionally writes the pre-rename name back onto the movie and
    // into its NFO -- forking the set in two.
    MovieTabFixture f("Alien Collection");

    renameSetElsewhere("Alien Collection", "Alien Anthology");
    REQUIRE(f.movie()->set().name == "Alien Anthology");
    REQUIRE(f.setBox()->currentText() == "Alien Collection");

    f.tab().saveInformation();

    CHECK(f.movie()->set().name == "Alien Anthology");
    CHECK(f.setInNfo() == "Alien Anthology");
}

TEST_CASE("Saving a movie commits a set name typed into the box", "[ui][movie][set]")
{
    // The other half: the navbar's save buttons are QToolButtons, take no focus and so never
    // end the combo's edit, which is why the save has to commit it at all.
    MovieTabFixture f("Alien Collection");

    f.setBox()->setCurrentText("Alien Anthology");

    f.tab().saveAll();

    CHECK(f.movie()->set().name == "Alien Anthology");
    CHECK(f.setInNfo() == "Alien Anthology");
}

TEST_CASE("A committed set name is not committed again over a later rename", "[ui][movie][set]")
{
    // The commit has to record what it wrote, or the box keeps diverging from the name the
    // widget thinks it last handled and every later save writes it again.
    MovieTabFixture f("Alien Collection");

    f.setBox()->setCurrentText("Predator Collection");
    QFocusEvent focusOut(QEvent::FocusOut, Qt::MouseFocusReason);
    QApplication::sendEvent(f.setBox(), &focusOut);
    REQUIRE(f.movie()->set().name == "Predator Collection");

    renameSetElsewhere("Predator Collection", "Predator Anthology");
    REQUIRE(f.setBox()->currentText() == "Predator Collection");

    f.tab().saveInformation();

    CHECK(f.movie()->set().name == "Predator Anthology");
}

TEST_CASE("Saving after the tab was cleared does not take the movie out of its set", "[ui][movie][set]")
{
    // clear() empties the box without anyone editing it, so a save that reads the emptiness
    // as an edit would commit "" -- which is how a movie leaves its set.
    MovieTabFixture f("Alien Collection");

    f.tab().clear();
    REQUIRE(f.setBox()->currentText().isEmpty());

    f.tab().saveInformation();

    CHECK(f.movie()->set().name == "Alien Collection");
}
