#include "test/test_helpers.h"

#include "data/movie/Movie.h"
#include "data/movie/MovieSet.h"
#include "model/MovieModel.h"
#include "model/MovieSetModel.h"

#include <QAbstractItemModelTester>
#include <QSignalSpy>
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
        // Movie::sigChanged means "repaint me" and fires for every kind of edit, so
        // most of them must not touch the set at all -- not even to remove the movie
        // and put it back, which a view would see as the list jumping.
        alien->setTitle("Alien (1979)");
        alien->setSortTitle("Alien");

        REQUIRE(sets.sets().size() == 1);
        CHECK(sets.set("Alien Collection")->movies() == QVector<Movie*>{alien, aliens});
    }

    SECTION("a movie that is destroyed disappears from its set")
    {
        // MovieModel::clear() calls deleteLater() on every movie and Movie::sigChanged
        // never fires on destruction, so this is the only notification there is.
        delete alien;

        CHECK(sets.set("Alien Collection")->movies() == QVector<Movie*>{aliens});
    }

    SECTION("a set whose last movie is destroyed is dropped")
    {
        // A Movie can die without MovieModel saying so.  The set heals itself on
        // QObject::destroyed too, but this model's handler for that signal runs
        // *before* the set's -- the model connects in attachMovie() before the set is
        // even created -- so it has to ask the sets rather than assume they have
        // already let go.
        delete alien;
        REQUIRE(sets.sets().size() == 1);

        delete aliens;

        CHECK(sets.sets().isEmpty());
    }

    SECTION("a set whose movies leave the library is dropped")
    {
        // MovieFileSearcher::reload() clears the movie model and fills it again.  The
        // movies are only deleteLater()'d there, so rowsAboutToBeRemoved is the one
        // notification that arrives while they are still in the model to be detached.
        QSignalSpy removals(&sets, &QAbstractItemModel::rowsRemoved);

        movies.clear();

        CHECK(sets.sets().isEmpty());
        CHECK(sets.set("Alien Collection") == nullptr);
        CHECK(removals.count() == 1);
    }

    SECTION("a set lets go of a movie that leaves the library")
    {
        // MovieModel::clear() only calls deleteLater(), so a set that kept the pointer
        // would hold a dangling one for as long as the event loop takes to run.  The
        // set survives here because a member the movie model never held keeps it alive.
        MovieSet* alienCollection = sets.set("Alien Collection");
        Movie* outsider = movieInSet(owner, "Alien 3", "Alien Collection");
        alienCollection->addMovie(outsider);
        REQUIRE(alienCollection->movies().size() == 3);

        movies.clear();

        REQUIRE(sets.set("Alien Collection") == alienCollection);
        CHECK(alienCollection->movies() == QVector<Movie*>{outsider});
    }

    SECTION("a set with an unsaved record survives its movies leaving the library")
    {
        // Nothing writes `set.nfo` yet, so an edit to a set's own record lives in this
        // object and nowhere else -- the member movies carry no flag for it, because
        // membership is not what changed.  Dropping the object is the only way it can
        // be lost, and it would be lost without a trace.
        MovieSet* alienCollection = sets.set("Alien Collection");
        alienCollection->setOverview("A science fiction horror film franchise.");

        movies.clear();

        REQUIRE(sets.sets() == QVector<MovieSet*>{alienCollection});
        CHECK(alienCollection->movies().isEmpty());
        CHECK(alienCollection->overview() == "A science fiction horror film franchise.");
    }

    SECTION("reload keeps an empty set that has an unsaved record")
    {
        MovieSet* predatorCollection = sets.addSet("Predator Collection");
        predatorCollection->setTmdbId(TmdbId(399));
        REQUIRE(sets.sets().size() == 2);

        sets.reload();

        CHECK(sets.set("Predator Collection") == predatorCollection);
    }

    SECTION("a movie that left the library is not followed any more")
    {
        movies.clear();
        REQUIRE(sets.sets().isEmpty());

        // The movie is still alive -- clear() only calls deleteLater() -- but it is no
        // longer part of the library, so its edits must not put sets back into the model.
        moveToSet(alien, "Predator Collection");

        CHECK(sets.sets().isEmpty());
    }

    SECTION("a set that loses its last movie to an edit stays")
    {
        // An edit never destroys a set.  The one the user just emptied is very often
        // the one they are about to fill again, and under D-A a set that has a
        // `set.nfo` outlives its last member.
        MovieSet* alienCollection = sets.set("Alien Collection");
        alien->setSet(MovieSetInfo{});
        aliens->setSet(MovieSetInfo{});

        REQUIRE(sets.sets() == QVector<MovieSet*>{alienCollection});
        CHECK(alienCollection->movies().isEmpty());
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

    SECTION("reload does not duplicate what is already there")
    {
        sets.reload();

        REQUIRE(sets.sets().size() == 1);
        CHECK(sets.set("Alien Collection")->movies() == QVector<Movie*>{alien, aliens});
    }

    SECTION("reload evicts a member the movie itself does not name")
    {
        // reload() is the resync: it rebuilds membership from the movies, so a member
        // that only the set believes in is dropped again.
        MovieSet* alienCollection = sets.set("Alien Collection");
        Movie* predator = movieInSet(owner, "Predator", "Predator Collection");
        movies.addMovie(predator);
        alienCollection->addMovie(predator);
        REQUIRE(alienCollection->movies().size() == 3);

        sets.reload();

        CHECK(alienCollection->movies() == QVector<Movie*>{alien, aliens});
        CHECK(sets.set("Predator Collection")->movies() == QVector<Movie*>{predator});
    }

    SECTION("a regroup announces one reset and nothing else")
    {
        // reload() empties every set, rebuilds membership and here has to create a set
        // from scratch as well.  Without the reset guards a view would be told about
        // each of those steps in the middle of a model reset.
        sets.clear();
        QSignalSpy repaints(&sets, &QAbstractItemModel::dataChanged);
        QSignalSpy insertions(&sets, &QAbstractItemModel::rowsInserted);
        QSignalSpy resets(&sets, &QAbstractItemModel::modelReset);

        sets.reload();

        REQUIRE(sets.sets().size() == 1);
        CHECK(resets.count() == 1);
        CHECK(repaints.count() == 0);
        CHECK(insertions.count() == 0);
    }

    SECTION("a set that changed announces a repaint")
    {
        QSignalSpy repaints(&sets, &QAbstractItemModel::dataChanged);

        sets.set("Alien Collection")->setOverview("A science fiction horror franchise.");

        REQUIRE(repaints.count() == 1);
        CHECK(repaints.at(0).at(0).value<QModelIndex>().row() == 0);
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

    SECTION("removeSet removes a set that has no movies")
    {
        // The sets tab's "Remove Movie Set" on a set nobody has put a movie into yet.
        // Nothing else drops it: an emptied set is not dropped for being empty.
        REQUIRE(sets.addSet("Alien Collection") != nullptr);

        sets.removeSet("Alien Collection");

        CHECK(sets.set("Alien Collection") == nullptr);
        CHECK(sets.sets().isEmpty());
    }

    SECTION("removeSet removes a set whose record has unsaved changes")
    {
        // Automatic drops spare such a set; a deliberate removal is the user saying to
        // throw it away, and does (loudly -- it logs).
        MovieSet* alienCollection = sets.addSet("Alien Collection");
        alienCollection->setOverview("A science fiction horror film franchise.");
        REQUIRE(alienCollection->hasChanged());

        sets.removeSet("Alien Collection");

        CHECK(sets.sets().isEmpty());
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

    SECTION("passes the Qt model checker while sets come and go")
    {
        auto tester = std::make_unique<QAbstractItemModelTester>(
            sets.get(), QAbstractItemModelTester::FailureReportingMode::Fatal);
        REQUIRE(sets->rowCount() == 2);

        sets->addSet("Alien vs Predator Collection");
        sets->removeSet("Alien Collection");
        sets->reload();

        CHECK(sets->rowCount() == 1);
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
