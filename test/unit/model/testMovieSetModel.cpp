#include "test/test_helpers.h"

#include "data/movie/Movie.h"
#include "data/movie/MovieSet.h"
#include "model/MovieModel.h"
#include "model/MovieSetModel.h"

#include <QAbstractItemModelTester>
#include <memory>

namespace {

/// \brief A movie that belongs to \p setName, owned by \p owner and not marked changed.
Movie* movieInSet(QObject& owner, const QString& title, const QString& setName)
{
    auto* movie = new Movie({}, &owner);
    movie->setTitle(title);
    if (!setName.isEmpty()) {
        MovieSetInfo info;
        info.name = setName;
        movie->setSet(info);
    }
    movie->setChanged(false);
    return movie;
}

/// \brief Puts \p movie into \p setName, the way the rest of MediaElch still does it.
void moveToSet(Movie* movie, const QString& setName)
{
    MovieSetInfo info;
    info.name = setName;
    movie->setSet(info);
}

} // namespace

TEST_CASE("MovieSetModel groups the library", "[model][movie][set]")
{
    QObject owner;
    MovieModel movies;
    MovieSetModel sets;

    SECTION("an empty library has no sets")
    {
        sets.setMovieModel(&movies);
        CHECK(sets.sets().isEmpty());
        CHECK(sets.rowCount() == 0);
    }

    SECTION("the movies present are grouped by set name, once")
    {
        movies.addMovie(movieInSet(owner, "Alien", "Alien Collection"));
        movies.addMovie(movieInSet(owner, "Aliens", "Alien Collection"));
        movies.addMovie(movieInSet(owner, "Predator", "Predator Collection"));
        movies.addMovie(movieInSet(owner, "Casablanca", ""));

        sets.setMovieModel(&movies);

        REQUIRE(sets.sets().size() == 2);
        REQUIRE(sets.set("Alien Collection") != nullptr);
        CHECK(sets.set("Alien Collection")->movies().size() == 2);
        REQUIRE(sets.set("Predator Collection") != nullptr);
        CHECK(sets.set("Predator Collection")->movies().size() == 1);
        // A movie without a set is in no set; an empty name is not a set (D-B).
        CHECK(sets.set("") == nullptr);
    }

    SECTION("a movie that enters the library afterwards joins its set")
    {
        sets.setMovieModel(&movies);
        REQUIRE(sets.sets().isEmpty());

        movies.addMovie(movieInSet(owner, "Alien", "Alien Collection"));

        REQUIRE(sets.sets().size() == 1);
        CHECK(sets.set("Alien Collection")->movies().size() == 1);
    }

    SECTION("movies that enter the library in bulk join their sets")
    {
        sets.setMovieModel(&movies);
        QVector<Movie*> batch;
        batch << movieInSet(owner, "Alien", "Alien Collection");
        batch << movieInSet(owner, "Aliens", "Alien Collection");
        batch << movieInSet(owner, "Predator", "Predator Collection");

        movies.addMovies(batch);

        REQUIRE(sets.sets().size() == 2);
        CHECK(sets.set("Alien Collection")->movies().size() == 2);
    }

    SECTION("set() knows only the names it holds")
    {
        movies.addMovie(movieInSet(owner, "Alien", "Alien Collection"));
        sets.setMovieModel(&movies);

        CHECK(sets.set("Alien Collection") != nullptr);
        CHECK(sets.set("Alien collection") == nullptr); // the name is the key, byte-exact (D-B)
        CHECK(sets.set("Predator Collection") == nullptr);
        CHECK(sets.set("") == nullptr);
    }
}

TEST_CASE("MovieSetModel follows the movies", "[model][movie][set]")
{
    QObject owner;
    MovieModel movies;
    MovieSetModel sets;
    Movie* alien = movieInSet(owner, "Alien", "Alien Collection");
    Movie* aliens = movieInSet(owner, "Aliens", "Alien Collection");
    movies.addMovie(alien);
    movies.addMovie(aliens);
    sets.setMovieModel(&movies);
    REQUIRE(sets.set("Alien Collection")->movies().size() == 2);

    SECTION("a movie moved to another set moves in the model too")
    {
        moveToSet(alien, "Alien vs Predator Collection");

        REQUIRE(sets.sets().size() == 2);
        CHECK(sets.set("Alien Collection")->movies() == QVector<Movie*>{aliens});
        CHECK(sets.set("Alien vs Predator Collection")->movies() == QVector<Movie*>{alien});
    }

    SECTION("a movie whose set is cleared leaves it")
    {
        alien->setSet(MovieSetInfo{});

        CHECK(sets.set("Alien Collection")->movies() == QVector<Movie*>{aliens});
    }

    SECTION("an edit that is not a membership change leaves membership alone")
    {
        // Movie::sigChanged means "repaint me" and fires for every kind of edit.
        alien->setTitle("Alien (1979)");
        alien->setSortTitle("Alien");

        REQUIRE(sets.sets().size() == 1);
        CHECK(sets.set("Alien Collection")->movies().size() == 2);
    }

    SECTION("a movie that is destroyed disappears from its set")
    {
        // MovieModel::clear() calls deleteLater() on every movie and Movie::sigChanged
        // never fires on destruction, so this is the only notification there is.
        delete alien;

        CHECK(sets.set("Alien Collection")->movies() == QVector<Movie*>{aliens});
    }

    SECTION("a set that loses its last movie is dropped")
    {
        // A set is its members until `set.nfo` gives it a record of its own.  Keeping
        // emptied ones would also collect one set per keystroke, because the movie
        // widget's set combo box is editable and rewrites the name on every one.
        alien->setSet(MovieSetInfo{});
        REQUIRE(sets.sets().size() == 1);

        aliens->setSet(MovieSetInfo{});

        CHECK(sets.sets().isEmpty());
        CHECK(sets.set("Alien Collection") == nullptr);
    }

    SECTION("a set that was created empty survives until the next reload")
    {
        // The sets tab adds a set before any movie is put into it.
        REQUIRE(sets.addSet("Predator Collection") != nullptr);
        CHECK(sets.sets().size() == 2);

        sets.reload();

        CHECK(sets.sets().size() == 1);
        CHECK(sets.set("Predator Collection") == nullptr);
    }

    SECTION("reload keeps a set's own record")
    {
        // Set attributes live in `set.nfo`, not in the movies, so regrouping must not
        // throw them away.
        MovieSet* alienCollection = sets.set("Alien Collection");
        alienCollection->setOverview("A science fiction horror film franchise.");
        alienCollection->setTmdbId(TmdbId(8091));

        sets.reload();

        REQUIRE(sets.set("Alien Collection") == alienCollection);
        CHECK(alienCollection->overview() == "A science fiction horror film franchise.");
        CHECK(alienCollection->tmdbId() == TmdbId(8091));
        CHECK(alienCollection->movies().size() == 2);
    }

    SECTION("reload regroups a movie whose set changed without being noticed")
    {
        // A resync, not the normal path: it must not duplicate memberships either.
        sets.reload();

        REQUIRE(sets.sets().size() == 1);
        CHECK(sets.set("Alien Collection")->movies().size() == 2);
    }
}

TEST_CASE("MovieSetModel adds and removes sets", "[model][movie][set]")
{
    QObject owner;
    MovieModel movies;
    MovieSetModel sets;
    sets.setMovieModel(&movies);

    SECTION("addSet creates one set per name")
    {
        MovieSet* first = sets.addSet("Alien Collection");
        REQUIRE(first != nullptr);
        CHECK(first->name() == "Alien Collection");
        CHECK(sets.addSet("Alien Collection") == first);
        CHECK(sets.sets().size() == 1);
    }

    SECTION("addSet refuses the empty name")
    {
        // A set is identified by its name (D-B), so a nameless one cannot exist.
        CHECK(sets.addSet("") == nullptr);
        CHECK(sets.sets().isEmpty());
    }

    SECTION("removeSet detaches its movies and marks them changed")
    {
        // Nothing else would: a membership change dirties neither the set (membership
        // is not in `set.nfo`, D-A) nor, by itself, the movie.  Without this the edit
        // is lost with no flag set anywhere.
        Movie* alien = movieInSet(owner, "Alien", "Alien Collection");
        Movie* aliens = movieInSet(owner, "Aliens", "Alien Collection");
        movies.addMovie(alien);
        movies.addMovie(aliens);
        REQUIRE_FALSE(alien->hasChanged());

        sets.removeSet("Alien Collection");

        CHECK(sets.set("Alien Collection") == nullptr);
        CHECK(sets.sets().isEmpty());
        CHECK(alien->set().name.isEmpty());
        CHECK(aliens->set().name.isEmpty());
        CHECK(alien->hasChanged());
        CHECK(aliens->hasChanged());
    }

    SECTION("removing a set that does not exist changes nothing")
    {
        sets.addSet("Alien Collection");

        sets.removeSet("Predator Collection");
        sets.removeSet("");

        CHECK(sets.sets().size() == 1);
    }

    SECTION("clear removes every set")
    {
        sets.addSet("Alien Collection");
        sets.addSet("Predator Collection");

        sets.clear();

        CHECK(sets.sets().isEmpty());
        CHECK(sets.rowCount() == 0);
    }
}

TEST_CASE("MovieSetModel as an item model", "[model][movie][set]")
{
    QObject owner;
    MovieModel movies;
    movies.addMovie(movieInSet(owner, "Alien", "Alien Collection"));
    movies.addMovie(movieInSet(owner, "Predator", "Predator Collection"));

    auto sets = std::make_unique<MovieSetModel>();
    sets->setMovieModel(&movies);

    SECTION("passes the Qt model checker")
    {
        auto tester = std::make_unique<QAbstractItemModelTester>(
            sets.get(), QAbstractItemModelTester::FailureReportingMode::Fatal);
        CHECK(sets->rowCount() == 2);
    }

    SECTION("a row carries its set's name and the set itself")
    {
        const QModelIndex index = sets->index(0, 0);
        REQUIRE(index.isValid());
        CHECK(sets->data(index, Qt::DisplayRole).toString() == "Alien Collection");
        CHECK(sets->data(index, MovieSetModel::NameRole).toString() == "Alien Collection");
        CHECK(sets->data(index, MovieSetModel::MovieCountRole).toInt() == 1);
        CHECK(
            sets->data(index, MovieSetModel::MovieSetPointerRole).value<MovieSet*>() == sets->set("Alien Collection"));
    }

    SECTION("an invalid index carries nothing")
    {
        CHECK_FALSE(sets->index(2, 0).isValid());
        CHECK_FALSE(sets->index(-1, 0).isValid());
        CHECK_FALSE(sets->data(QModelIndex(), MovieSetModel::NameRole).isValid());
    }
}
