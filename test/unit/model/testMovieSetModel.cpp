#include "test/test_helpers.h"

#include "data/movie/Movie.h"
#include "data/movie/MovieController.h"
#include "data/movie/MovieSet.h"
#include "globals/Manager.h"
#include "model/MovieModel.h"
#include "model/MovieSetModel.h"
#include "scrapers/movie/MovieMerger.h"
#include "test/helpers/message_capture.h"
#include "test/mocks/media_center/MediaCenterInterfaceMock.h"
#include "test/unit/scrapers/custom_movie_scraper/StubMovieScraper.h"

#include <QApplication>
#include <QSignalSpy>

namespace {

/// \brief A movie that belongs to \p setName, owned by \p owner and not marked changed.
Movie* movieInSet(QObject& owner, const QString& title, const QString& setName)
{
    auto* movie = new Movie({}, &owner);
    movie->setTitle(title);
    if (!setName.isEmpty()) {
        MovieSetInfo info;
        info.name = setName;
        movie->setSetInfo(info);
    }
    movie->setChanged(false);
    return movie;
}

/// \brief The whole `<set>` block a member NFO carries: name, overview and id.
MovieSetInfo setInfo(const QString& name, const QString& overview, TmdbId tmdbId = TmdbId::NoId)
{
    MovieSetInfo info;
    info.name = name;
    info.overview = overview;
    info.tmdbId = tmdbId;
    return info;
}

/// \brief A movie whose NFO carries the set's overview and id as well as its name.
Movie* movieInSet(QObject& owner, const QString& title, const MovieSetInfo& info)
{
    auto* movie = new Movie({}, &owner);
    movie->setTitle(title);
    movie->setSetInfo(info);
    movie->setChanged(false);
    return movie;
}

/// \brief Puts \p movie into \p setName, the way the rest of MediaElch still does it.
void moveToSet(Movie* movie, const QString& setName)
{
    MovieSetInfo info;
    info.name = setName;
    movie->setSetInfo(info);
}


} // namespace

TEST_CASE("MovieSetModel resolves the rename mode", "[model][movie][set]")
{
    using RenameMode = MovieSetModel::RenameMode;
    const mediaelch::KodiVersion v22{mediaelch::KodiVersion::v22};
    const mediaelch::KodiVersion v21{mediaelch::KodiVersion::v21};
    const mediaelch::KodiVersion v19{mediaelch::KodiVersion::v19};
    const mediaelch::KodiVersion v17{mediaelch::KodiVersion::v17};

    SECTION("Automatic follows the Kodi version where there are records to rename")
    {
        CHECK(MovieSetModel::resolveRenameMode(MovieSetRenameMode::Automatic, v22, true)
              == RenameMode::SetFileOnly);
        CHECK(MovieSetModel::resolveRenameMode(MovieSetRenameMode::Automatic, v21, true)
              == RenameMode::AllMovieFiles);
        CHECK(MovieSetModel::resolveRenameMode(MovieSetRenameMode::Automatic, v19, true)
              == RenameMode::AllMovieFiles);
        CHECK(MovieSetModel::resolveRenameMode(MovieSetRenameMode::Automatic, v17, true)
              == RenameMode::AllMovieFiles);
    }

    SECTION("Automatic asks about the records too, not only the version")
    {
        // A fresh install is Kodi 22 with no folder, so the version alone is not enough.
        CHECK(MovieSetModel::resolveRenameMode(MovieSetRenameMode::Automatic, v22, false)
              == RenameMode::AllMovieFiles);
    }

    SECTION("Automatic never refuses")
    {
        for (const mediaelch::KodiVersion& version : mediaelch::KodiVersion::all()) {
            for (const bool records : {true, false}) {
                CHECK(MovieSetModel::resolveRenameMode(MovieSetRenameMode::Automatic, version, records)
                      != RenameMode::Unavailable);
            }
        }
    }

    SECTION("An explicit choice overrides the version in both directions")
    {
        CHECK(MovieSetModel::resolveRenameMode(MovieSetRenameMode::SetFileOnly, v19, true)
              == RenameMode::SetFileOnly);
        CHECK(MovieSetModel::resolveRenameMode(MovieSetRenameMode::AllMovieFiles, v22, true)
              == RenameMode::AllMovieFiles);
    }

    SECTION("An explicit set-file-only rename with no record is refused, not downgraded")
    {
        // Downgrading would rewrite every member's NFO, which this setting exists to avoid.
        CHECK(MovieSetModel::resolveRenameMode(MovieSetRenameMode::SetFileOnly, v22, false)
              == RenameMode::Unavailable);
        CHECK(MovieSetModel::resolveRenameMode(MovieSetRenameMode::SetFileOnly, v19, false)
              == RenameMode::Unavailable);
    }

    SECTION("All movie files never needs a record")
    {
        CHECK(MovieSetModel::resolveRenameMode(MovieSetRenameMode::AllMovieFiles, v22, false)
              == RenameMode::AllMovieFiles);
    }
}

TEST_CASE("MovieSetModel is the only thing that changes membership", "[model][movie][set]")
{
    QObject owner;
    MovieModel movies;
    MovieSetModel sets;

    Movie* alien = movieInSet(owner, "Alien", "Alien Collection");
    movies.addMovie(alien);
    sets.setMovieModel(&movies);
    REQUIRE(sets.set("Alien Collection")->movies() == QVector<Movie*>{alien});

    SECTION("assign moves a movie between sets and marks it changed")
    {
        MovieSetInfo predator;
        predator.name = "Predator Collection";

        sets.assign(alien, predator);

        CHECK(alien->set().name == "Predator Collection");
        CHECK(sets.set("Alien Collection")->movies().isEmpty());
        REQUIRE(sets.set("Predator Collection") != nullptr);
        CHECK(sets.set("Predator Collection")->movies() == QVector<Movie*>{alien});
        // A membership change dirties nothing on its own, yet it has to reach the member's NFO.
        CHECK(alien->hasChanged());
    }

    SECTION("assign carries the whole value, not only the name")
    {
        MovieSetInfo predator;
        predator.name = "Predator Collection";
        predator.overview = "A science fiction action franchise.";
        predator.tmdbId = TmdbId(399);

        sets.assign(alien, predator);

        CHECK(alien->set().overview == "A science fiction action franchise.");
        CHECK(alien->set().tmdbId == TmdbId(399));
    }

    SECTION("assign with an empty name takes the movie out of its set")
    {
        sets.assign(alien, MovieSetInfo{});

        CHECK(alien->set().name.isEmpty());
        REQUIRE(sets.set("Alien Collection") != nullptr);
        CHECK(sets.set("Alien Collection")->movies().isEmpty());
    }

    SECTION("assign on a movie outside the library writes the value and no membership")
    {
        Movie* outsider = movieInSet(owner, "Alien 3", "");
        MovieSetInfo predator;
        predator.name = "Predator Collection";

        sets.assign(outsider, predator);

        CHECK(outsider->set().name == "Predator Collection");
        CHECK(sets.sets() == QVector<MovieSet*>{sets.set("Alien Collection")});
    }

    SECTION("assign survives a caller that has blocked the movie's signals")
    {
        // A direct call, because loadData() blocks a movie's signals across a whole NFO re-read.
        MovieSetInfo predator;
        predator.name = "Predator Collection";
        {
            const QSignalBlocker blocker(alien);
            sets.assign(alien, predator);
        }

        REQUIRE(sets.set("Predator Collection") != nullptr);
        CHECK(sets.set("Predator Collection")->movies() == QVector<Movie*>{alien});
    }

    SECTION("syncMovie catches a set written while the movie's signals were blocked")
    {
        // What loadData() does: the set comes back different while sigChanged never fires.
        {
            const QSignalBlocker blocker(alien);
            MovieSetInfo fromNfo;
            fromNfo.name = "Alien Anthology";
            alien->setSetInfo(fromNfo);
        }
        REQUIRE(sets.set("Alien Anthology") == nullptr);

        sets.syncMovie(alien);

        REQUIRE(sets.set("Alien Anthology") != nullptr);
        CHECK(sets.set("Alien Anthology")->movies() == QVector<Movie*>{alien});
        CHECK(sets.set("Alien Collection")->movies().isEmpty());
    }

    SECTION("syncMovie does nothing for a movie whose set has not moved")
    {
        sets.syncMovie(alien);

        CHECK(sets.set("Alien Collection")->movies() == QVector<Movie*>{alien});
        CHECK_FALSE(alien->hasChanged());
    }

    SECTION("syncMovie does not bring back a set that removeSet took away")
    {
        // removeSet() clears its members' set names, so a reconcile finds nothing to read back.
        CHECK(sets.removeSet("Alien Collection"));
        REQUIRE(sets.sets().isEmpty());

        sets.syncMovie(alien);

        CHECK(sets.sets().isEmpty());
        CHECK(alien->set().name.isEmpty());
    }

    SECTION("syncMovie is a no-op for a movie the model never attached")
    {
        Movie* outsider = movieInSet(owner, "Predator", "Predator Collection");

        sets.syncMovie(outsider);

        CHECK(sets.sets() == QVector<MovieSet*>{sets.set("Alien Collection")});
    }

    SECTION("assign to the set the movie is already in changes nothing at all")
    {
        // Dirtying it here would offer to rewrite an NFO the user never touched.
        MovieSetInfo same = alien->set();
        QSignalSpy changes(alien, &Movie::sigChanged);

        sets.assign(alien, same);

        CHECK_FALSE(alien->hasChanged());
        CHECK(changes.isEmpty());
        CHECK(sets.set("Alien Collection")->movies() == QVector<Movie*>{alien});
    }

    SECTION("assign compares the whole value, so a name-only one overwrites the rest")
    {
        // Why MovieWidget::onSetChange() compares names first: the name-only value it hands
        // assign() never matches a set that also carries an id or an overview.
        MovieSetInfo rich;
        rich.name = "Alien Collection";
        rich.tmdbId = TmdbId(8091);
        rich.overview = "A science fiction horror film franchise.";
        sets.assign(alien, rich);
        alien->setChanged(false);

        MovieSetInfo nameOnly;
        nameOnly.name = "Alien Collection";
        sets.assign(alien, nameOnly);

        CHECK(alien->set().name == "Alien Collection");
        CHECK(alien->set().tmdbId == TmdbId::NoId);
        CHECK(alien->set().overview.isEmpty());
        CHECK(alien->hasChanged());
    }

    SECTION("assign still reconciles when the value it was given is unchanged")
    {
        // Only the write is skipped: the model can be behind the movie for other reasons.
        {
            // As in loadData(): the closing setChanged(false) is inside the blocker too.
            const QSignalBlocker blocker(alien);
            MovieSetInfo fromNfo;
            fromNfo.name = "Alien Anthology";
            alien->setSetInfo(fromNfo);
            alien->setChanged(false);
        }
        REQUIRE(sets.set("Alien Anthology") == nullptr);
        REQUIRE_FALSE(alien->hasChanged());

        sets.assign(alien, alien->set());

        REQUIRE(sets.set("Alien Anthology") != nullptr);
        CHECK(sets.set("Alien Anthology")->movies() == QVector<Movie*>{alien});
        CHECK_FALSE(alien->hasChanged());
    }

    SECTION("a scrape of a library movie needs syncMovie, because the merge is blocked")
    {
        Movie scraped;
        MovieSetInfo fromScraper;
        fromScraper.name = "Alien Anthology";
        fromScraper.tmdbId = TmdbId(8091);
        scraped.setSetInfo(fromScraper);

        mediaelch::scraper::copyDetailsToMovie(
            *alien, scraped, QSet<MovieScraperInfo>{MovieScraperInfo::Set}, false, false);

        REQUIRE(alien->set().name == "Alien Anthology");
        CHECK(sets.set("Alien Anthology") == nullptr); // the merge told the model nothing

        sets.syncMovie(alien);

        REQUIRE(sets.set("Alien Anthology") != nullptr);
        CHECK(sets.set("Alien Anthology")->movies() == QVector<Movie*>{alien});
        CHECK(sets.set("Alien Collection")->movies().isEmpty());
    }

    SECTION("assign and syncMovie ignore a null movie")
    {
        sets.assign(nullptr, MovieSetInfo{});
        sets.syncMovie(nullptr);

        CHECK(sets.set("Alien Collection")->movies() == QVector<Movie*>{alien});
    }
}

TEST_CASE("MovieSetModel groups the library", "[model][movie][set]")
{
    QObject owner;
    MovieModel movies;
    MovieSetModel sets;

    SECTION("an empty library has no sets")
    {
        sets.setMovieModel(&movies);
        CHECK(sets.sets().isEmpty());
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
        CHECK(sets.set("") == nullptr);
    }

    SECTION("a movie that enters the library afterwards joins its set")
    {
        sets.setMovieModel(&movies);
        REQUIRE(sets.sets().isEmpty());

        Movie* alien = movieInSet(owner, "Alien", "Alien Collection");
        movies.addMovie(alien);

        REQUIRE(sets.sets().size() == 1);
        CHECK(sets.set("Alien Collection")->movies().size() == 1);
        // Attaching a movie is the model following the library, not editing it.  A setSetInfo()
        // added here would offer every NFO in the library for rewriting on every reload.
        CHECK_FALSE(alien->hasChanged());
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
        CHECK(sets.set("Alien collection") == nullptr); // the name is the key, byte-exact
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
        alien->setSetInfo(MovieSetInfo{});

        CHECK(sets.set("Alien Collection")->movies() == QVector<Movie*>{aliens});
    }

    SECTION("an edit that is not a membership change leaves membership alone")
    {
        // sigChanged fires for every kind of edit, so most must not touch the set at all.
        alien->setTitle("Alien (1979)");
        alien->setSortTitle("Alien");

        REQUIRE(sets.sets().size() == 1);
        CHECK(sets.set("Alien Collection")->movies() == QVector<Movie*>{alien, aliens});
    }

    SECTION("a movie that is destroyed disappears from its set")
    {
        // Movie::sigChanged never fires on destruction, so this is the only notification.
        delete alien;

        CHECK(sets.set("Alien Collection")->movies() == QVector<Movie*>{aliens});
    }

    SECTION("a set whose last movie is destroyed is dropped")
    {
        // A Movie can die without MovieModel saying so.  The set heals itself on destroyed()
        // too, but the model's handler runs first -- it connects in attachMovie(), before the
        // set exists -- so it must ask the sets rather than assume they have let go.
        delete alien;
        REQUIRE(sets.sets().size() == 1);

        delete aliens;

        CHECK(sets.sets().isEmpty());
    }

    SECTION("a set whose movies leave the library is dropped")
    {
        // The movies are only deleteLater()'d, so rowsAboutToBeRemoved is the one
        // notification that arrives while they are still there to be detached.
        movies.clear();

        CHECK(sets.sets().isEmpty());
        CHECK(sets.set("Alien Collection") == nullptr);
        // Read before the deleteLater() runs.  Detaching is the model following the library
        // too: a movie that left it must not come back offering to rewrite its NFO.
        CHECK_FALSE(alien->hasChanged());
        CHECK_FALSE(aliens->hasChanged());
    }

    SECTION("a set lets go of a movie that leaves the library")
    {
        // clear() only calls deleteLater(), so a set that kept the pointer would dangle.
        MovieSet* alienCollection = sets.set("Alien Collection");
        Movie* outsider = movieInSet(owner, "Alien 3", "Alien Collection");
        alienCollection->addMovie(outsider);
        REQUIRE(alienCollection->movies().size() == 3);

        movies.clear();

        REQUIRE(sets.set("Alien Collection") == alienCollection);
        CHECK(alienCollection->movies() == QVector<Movie*>{outsider});
    }

    SECTION("a set lets go of a movie that leaves the library and that it does not name")
    {
        // A set can hold a member whose own set().name points elsewhere, so letting go of only
        // the movies naming it would leave a dangling pointer.
        MovieSet* alienCollection = sets.set("Alien Collection");
        Movie* predator = movieInSet(owner, "Predator", "Predator Collection");
        movies.addMovie(predator);
        alienCollection->addMovie(predator);
        // A member the movie model never held, so the set outlives the clear().
        Movie* outsider = movieInSet(owner, "Alien 3", "Alien Collection");
        alienCollection->addMovie(outsider);
        REQUIRE(alienCollection->movies().size() == 4);

        movies.clear();

        REQUIRE(sets.set("Alien Collection") == alienCollection);
        CHECK(alienCollection->movies() == QVector<Movie*>{outsider});
        CHECK(sets.set("Predator Collection") == nullptr);
    }

    SECTION("a set that is dropped while it still holds a movie is forgotten entirely")
    {
        // detachMovie() finds a movie's sets from an index, so a deleted set has to leave that
        // index or the next detach calls removeMovie() on freed memory.  removeSet() detaches by
        // clearing set names, so a member whose own name points elsewhere is still in the set.
        MovieSet* alienCollection = sets.set("Alien Collection");
        Movie* predator = movieInSet(owner, "Predator", "Predator Collection");
        movies.addMovie(predator);
        alienCollection->addMovie(predator);
        REQUIRE(alienCollection->movies().contains(predator));

        CHECK(sets.removeSet("Alien Collection"));
        REQUIRE(sets.set("Alien Collection") == nullptr);

        movies.clear();

        CHECK(sets.sets().isEmpty());
    }

    SECTION("an unsaved record does not exempt a set from being dropped")
    {
        // A set dirtied and never saved stays dirty, so exempting changed sets would exempt it all
        // session.
        sets.set("Alien Collection")->setOverview("A science fiction horror film franchise.");

        movies.clear();

        CHECK(sets.sets().isEmpty());
    }

    SECTION("reload drops an empty set whose record was edited")
    {
        MovieSet* predatorCollection = sets.addSet("Predator Collection");
        predatorCollection->setTmdbId(TmdbId(399));
        REQUIRE(sets.sets().size() == 2);

        sets.reload();

        CHECK(sets.set("Predator Collection") == nullptr);
    }

    SECTION("a movie that left the library is not followed any more")
    {
        movies.clear();
        REQUIRE(sets.sets().isEmpty());

        // Still alive, since clear() only calls deleteLater(), but out of the library.
        moveToSet(alien, "Predator Collection");

        CHECK(sets.sets().isEmpty());
    }

    SECTION("a set that loses its last movie to an edit stays")
    {
        // The set the user just emptied is very often the one they are about to fill again.
        MovieSet* alienCollection = sets.set("Alien Collection");
        alien->setSetInfo(MovieSetInfo{});
        aliens->setSetInfo(MovieSetInfo{});

        REQUIRE(sets.sets() == QVector<MovieSet*>{alienCollection});
        CHECK(alienCollection->movies().isEmpty());
    }

    SECTION("a set that was created empty survives until the next reload")
    {
        REQUIRE(sets.addSet("Predator Collection") != nullptr);
        CHECK(sets.sets().size() == 2);

        sets.reload();

        CHECK(sets.sets().size() == 1);
        CHECK(sets.set("Predator Collection") == nullptr);
    }

    SECTION("reload keeps a set's own record")
    {
        // Set attributes live in `set.nfo`, so regrouping must not throw them away.
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
        MovieSet* alienCollection = sets.set("Alien Collection");
        Movie* predator = movieInSet(owner, "Predator", "Predator Collection");
        movies.addMovie(predator);
        alienCollection->addMovie(predator);
        REQUIRE(alienCollection->movies().size() == 3);

        sets.reload();

        CHECK(alienCollection->movies() == QVector<Movie*>{alien, aliens});
        CHECK(sets.set("Predator Collection")->movies() == QVector<Movie*>{predator});
    }

    SECTION("a regroup rebuilds the same list")
    {
        sets.clear();

        sets.reload();

        REQUIRE(sets.sets().size() == 1);
        CHECK(sets.set("Alien Collection") != nullptr);
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
        CHECK(sets.addSet("") == nullptr);
        CHECK(sets.sets().isEmpty());
    }

    SECTION("removeSet detaches its movies and marks them changed")
    {
        // Membership is not in `set.nfo` and does not dirty the movie by itself, so without
        // this the edit is lost with no flag set anywhere.
        Movie* alien = movieInSet(owner, "Alien", "Alien Collection");
        Movie* aliens = movieInSet(owner, "Aliens", "Alien Collection");
        movies.addMovie(alien);
        movies.addMovie(aliens);
        REQUIRE_FALSE(alien->hasChanged());

        CHECK(sets.removeSet("Alien Collection"));

        CHECK(sets.set("Alien Collection") == nullptr);
        CHECK(sets.sets().isEmpty());
        CHECK(alien->set().name.isEmpty());
        CHECK(aliens->set().name.isEmpty());
        CHECK(alien->hasChanged());
        CHECK(aliens->hasChanged());
    }

    SECTION("removeSet removes a set that has no movies")
    {
        // Nothing else drops such a set: an emptied set is not dropped for being empty.
        REQUIRE(sets.addSet("Alien Collection") != nullptr);

        CHECK(sets.removeSet("Alien Collection"));

        CHECK(sets.set("Alien Collection") == nullptr);
        CHECK(sets.sets().isEmpty());
    }

    SECTION("removeSet removes a set whose record has unsaved changes")
    {
        MovieSet* alienCollection = sets.addSet("Alien Collection");
        alienCollection->setOverview("A science fiction horror film franchise.");
        REQUIRE(alienCollection->hasChanged());

        CHECK(sets.removeSet("Alien Collection"));

        CHECK(sets.sets().isEmpty());
    }

    SECTION("removeSet says so when it discards an unsaved record")
    {
        sets.addSet("Alien Collection")->setOverview("A science fiction horror film franchise.");

        test::MessageCapture warnings;
        CHECK(sets.removeSet("Alien Collection"));

        REQUIRE(warnings.messages().size() == 1);
        CHECK(warnings.messages().first().contains("Alien Collection"));
    }

    SECTION("removeSet says nothing when there is no record to lose")
    {
        REQUIRE(sets.addSet("Alien Collection") != nullptr);

        test::MessageCapture warnings;
        CHECK(sets.removeSet("Alien Collection"));

        CHECK(warnings.messages().isEmpty());
    }

    SECTION("clear says so for every record it discards")
    {
        sets.addSet("Alien Collection")->setOverview("A science fiction horror film franchise.");
        sets.addSet("Predator Collection")->setTmdbId(TmdbId(399));
        sets.addSet("Rocky Collection");

        test::MessageCapture warnings;
        sets.clear();

        REQUIRE(warnings.messages().size() == 2);
        CHECK(warnings.messages().at(0).contains("Alien Collection"));
        CHECK(warnings.messages().at(1).contains("Predator Collection"));
    }

    SECTION("removing a set that does not exist changes nothing")
    {
        sets.addSet("Alien Collection");

        CHECK(sets.removeSet("Predator Collection")); // nothing to remove is not a failure
        CHECK(sets.removeSet(""));

        CHECK(sets.sets().size() == 1);
    }

    SECTION("clear removes every set")
    {
        sets.addSet("Alien Collection");
        sets.addSet("Predator Collection");

        sets.clear();

        CHECK(sets.sets().isEmpty());
    }
}

TEST_CASE("MovieSetModel renames a set", "[model][movie][set]")
{
    // MovieSet::setName() is private to the model, so this is the only way a set's key moves.
    MovieSetModel sets;
    MovieSet* alienCollection = sets.addSet("Alien Collection");
    REQUIRE(alienCollection != nullptr);

    SECTION("the key moves and the set stays the same object")
    {
        alienCollection->setOverview("A science fiction horror franchise.");

        CHECK(sets.renameSet(alienCollection, "Alien Anthology"));

        CHECK(alienCollection->name() == "Alien Anthology");
        CHECK(sets.set("Alien Anthology") == alienCollection);
        CHECK(sets.set("Alien Collection") == nullptr);
        CHECK(sets.sets().size() == 1);
        // The overview and the id ride along; that is why the object is renamed, not replaced.
        CHECK(alienCollection->overview() == "A science fiction horror franchise.");
    }

    SECTION("moving the key re-unifies the two names")
    {
        // An all-movie-files rename moves the key itself, so no display title is left to hold.
        alienCollection->setTitle("The Alien Saga");

        CHECK(sets.renameSet(alienCollection, "Alien Anthology"));

        CHECK(alienCollection->title().isEmpty());
        CHECK(alienCollection->displayName() == "Alien Anthology");
        CHECK(alienCollection->hasChanged());
    }

    SECTION("a key moved onto its own display title still re-unifies and dirties")
    {
        alienCollection->setTitle("The Alien Saga");
        alienCollection->setChanged(false);

        CHECK(sets.renameSet(alienCollection, "The Alien Saga"));

        CHECK(alienCollection->name() == "The Alien Saga");
        CHECK(alienCollection->title().isEmpty());
        CHECK(alienCollection->hasChanged());
    }

    SECTION("an observer of the rename never sees the abolished display title")
    {
        // setChanged() emits sigChanged synchronously, so an observer reads the set from inside
        // its own slot: clear the title after the emit and every observer sees the new key still
        // carrying the abolished display title.  Asserted inside the slot, because afterwards
        // both orders look identical.
        alienCollection->setTitle("The Alien Saga");
        alienCollection->setChanged(false);

        int observed = 0;
        QString seenDisplayName;
        QString seenTitle;
        QObject::connect(alienCollection, &MovieSet::sigChanged, [&](MovieSet* changed) {
            ++observed;
            seenDisplayName = changed->displayName();
            seenTitle = changed->title();
        });

        CHECK(sets.renameSet(alienCollection, "Alien Anthology"));

        REQUIRE(observed == 1);
        CHECK(seenDisplayName == "Alien Anthology");
        CHECK(seenTitle.isEmpty());
    }

    SECTION("renaming a set to the name it already has is not an edit")
    {
        alienCollection->setChanged(false);
        int changes = 0;
        QObject::connect(alienCollection, &MovieSet::sigChanged, [&changes](MovieSet*) { ++changes; });

        CHECK(sets.renameSet(alienCollection, "Alien Collection"));

        CHECK(changes == 0);
        CHECK_FALSE(alienCollection->hasChanged());
    }

    SECTION("a name another set already holds is refused")
    {
        // Two sets keyed alike would make set() answer with the first of them for every lookup
        // in the application, and nothing detects or recovers from that.
        MovieSet* predatorCollection = sets.addSet("Predator Collection");
        predatorCollection->setChanged(false);

        CHECK_FALSE(sets.renameSet(predatorCollection, "Alien Collection"));

        CHECK(predatorCollection->name() == "Predator Collection");
        CHECK(sets.set("Alien Collection") == alienCollection);
        CHECK(sets.sets().size() == 2);
        // Refused means nothing happened, not even the dirty flag.
        CHECK_FALSE(predatorCollection->hasChanged());
    }

    SECTION("a display title another set holds is not the model's business")
    {
        // Only the match key is the model's index; SetsWidget checks the display titles, which
        // Kodi never matches on.
        MovieSet* predatorCollection = sets.addSet("Predator Collection");
        alienCollection->setTitle("The Saga");

        CHECK(sets.renameSet(predatorCollection, "The Saga"));

        CHECK(predatorCollection->name() == "The Saga");
    }

    SECTION("an empty name is refused")
    {
        CHECK_FALSE(sets.renameSet(alienCollection, QString()));

        CHECK(alienCollection->name() == "Alien Collection");
    }

    SECTION("a set this model does not hold is refused")
    {
        MovieSet stranger{"Predator Collection"};

        CHECK_FALSE(sets.renameSet(&stranger, "Alien Anthology"));
        CHECK_FALSE(sets.renameSet(nullptr, "Alien Anthology"));

        CHECK(stranger.name() == "Predator Collection");
    }
}

// The two tests below use the real Manager: MovieController reaches MovieSetModel through
// the singleton, with no seam to substitute.
namespace {

/// \brief Empties Manager's movie model again, however the test leaves.
struct LibraryGuard
{
    ~LibraryGuard()
    {
        Manager::instance()->movieModel()->clear();
        // clear() only calls deleteLater(); let the sets hear about it.
        qApp->processEvents();
    }
};

} // namespace

TEST_CASE("An NFO reload of a library movie reaches the set model", "[model][movie][set]")
{
    // loadData() re-reads the NFO under a QSignalBlocker covering the whole load, so nothing
    // about this write reaches MovieSetModel by signal; the explicit reconcile is what this
    // pins.  reloadFromNfo = false parses Movie::nfoContent(), so it needs no fixture on disk.
    Manager* manager = Manager::instance();
    MovieSetModel* setModel = manager->movieSetModel();
    LibraryGuard guard;

    auto* movie = new Movie({}, nullptr);
    MovieSetInfo beforeReload;
    beforeReload.name = "Alien Collection";
    movie->setSetInfo(beforeReload);
    movie->setNfoContent(R"(<?xml version="1.0" encoding="UTF-8"?>
<movie><title>Alien</title><set><name>Alien Anthology</name></set></movie>)");
    movie->setChanged(false);
    manager->movieModel()->addMovie(movie);
    REQUIRE(setModel->set("Alien Collection") != nullptr);

    REQUIRE(movie->controller()->loadData(manager->mediaCenterInterface(), true, false));

    REQUIRE(movie->set().name == "Alien Anthology");
    REQUIRE(setModel->set("Alien Anthology") != nullptr);
    CHECK(setModel->set("Alien Anthology")->movies() == QVector<Movie*>{movie});
    CHECK(setModel->set("Alien Collection")->movies().isEmpty());
}

TEST_CASE("A scrape of a library movie reaches the set model", "[model][movie][set]")
{
    // copyDetailsToMovie() blocks the target's signals for the whole merge, so a scrape writes
    // a library movie's set where MovieSetModel cannot see it; the explicit reconcile is what
    // this pins.  The extra QSignalBlocker is deliberate: without it scraperLoadDone() repairs
    // the model a frame later and the reconcile cannot be told from its absence.
    Manager* manager = Manager::instance();
    MovieSetModel* setModel = manager->movieSetModel();
    LibraryGuard guard;

    auto* movie = new Movie({}, nullptr);
    MovieSetInfo beforeScrape;
    beforeScrape.name = "Alien Collection";
    movie->setSetInfo(beforeScrape);
    movie->setChanged(false);
    manager->movieModel()->addMovie(movie);
    REQUIRE(setModel->set("Alien Collection") != nullptr);

    test::StubMovieScraper scraper("stub-set-scraper", nullptr);
    MovieSetInfo fromScraper;
    fromScraper.name = "Alien Anthology";
    fromScraper.tmdbId = TmdbId(8091);
    scraper.stub_movie.setSetInfo(fromScraper);

    {
        const QSignalBlocker blocker(movie);
        movie->controller()->loadData({{&scraper, mediaelch::scraper::MovieIdentifier("stub-id")}},
            mediaelch::Locale("en-US"),
            QSet<MovieScraperInfo>{MovieScraperInfo::Set});
        // MovieScrapeJob::start() defers to the event loop.
        for (int spin = 0; spin < 20; ++spin) {
            qApp->processEvents();
        }

        REQUIRE(movie->set().name == "Alien Anthology");
        REQUIRE(setModel->set("Alien Anthology") != nullptr);
        CHECK(setModel->set("Alien Anthology")->movies() == QVector<Movie*>{movie});
        CHECK(setModel->set("Alien Collection")->movies().isEmpty());
    }
}

TEST_CASE("A set with a record outlives its last movie", "[model][movie][set]")
{
    // Why MovieSet::hasRecord() exists: a set without a record is nothing but the movies that
    // named it, while a set with a `set.nfo` is not derived from anything and stays.
    QObject owner;
    MovieModel movies;
    MediaCenterInterfaceMock mediaCenter;
    MovieSetModel sets;

    Movie* alien = movieInSet(owner, "Alien", "Alien Collection");
    movies.addMovie(alien);
    mediaCenter.putRecord("Alien Collection", {"Ripley versus the Alien.", TmdbId(8091)});
    sets.setMovieModel(&movies);
    sets.setRecordSource(&mediaCenter);

    REQUIRE(sets.set("Alien Collection") != nullptr);

    SECTION("The record is read when the set is created")
    {
        CHECK(sets.set("Alien Collection")->overview() == "Ripley versus the Alien.");
        CHECK(sets.set("Alien Collection")->tmdbId() == TmdbId(8091));
        // Reading what is on disk is not an edit, and the flag would report changes nobody made.
        CHECK_FALSE(sets.set("Alien Collection")->hasChanged());
    }

    SECTION("It survives a reload that leaves it with no members")
    {
        sets.assign(alien, MovieSetInfo{});
        sets.reload();
        CHECK(sets.set("Alien Collection") != nullptr);
        CHECK(sets.set("Alien Collection")->movies().isEmpty());
    }

    SECTION("It survives its movies leaving the library")
    {
        movies.clear();
        qApp->processEvents();
        CHECK(sets.set("Alien Collection") != nullptr);
    }

    SECTION("A set without a record is still dropped")
    {
        // A set nothing derives and nothing records would sit in the combo box and the filter
        // with no movie answering to it.
        sets.addSet("Predator Collection");
        REQUIRE(sets.set("Predator Collection") != nullptr);
        sets.reload();
        CHECK(sets.set("Predator Collection") == nullptr);
    }

    SECTION("With no folder configured, a record counts for nothing")
    {
        // No folder means no records, and it is asked live so a settings change takes effect
        // at once rather than at the next reload.
        sets.assign(alien, MovieSetInfo{});
        mediaCenter.setRecordsEnabled(false);
        sets.reload();
        CHECK(sets.set("Alien Collection") == nullptr);
    }

    SECTION("Without a media center there are no records at all")
    {
        sets.setRecordSource(nullptr);
        sets.assign(alien, MovieSetInfo{});
        sets.reload();
        CHECK(sets.set("Alien Collection") == nullptr);
    }

    SECTION("A record deleted behind MediaElch's back stops keeping the set alive")
    {
        // Every reload re-asks, so a `set.nfo` removed elsewhere stops holding the set up.
        sets.assign(alien, MovieSetInfo{});
        mediaCenter.putRecord("Predator Collection");
        sets.reload();
        REQUIRE(sets.set("Alien Collection") != nullptr);

        mediaCenter.removeMovieSetRecord("Alien Collection");
        sets.reload();
        CHECK(sets.set("Alien Collection") == nullptr);
    }

    SECTION("Deliberate removal takes the record with it")
    {
        // Otherwise the record outlives the set and the next reload brings the set back.
        CHECK(sets.removeSet("Alien Collection"));
        CHECK(sets.set("Alien Collection") == nullptr);
        CHECK_FALSE(mediaCenter.hasRecordOnDisk("Alien Collection"));
        sets.reload();
        CHECK(sets.set("Alien Collection") == nullptr);
    }

    SECTION("A removal the media center refuses changes nothing at all")
    {
        // Ignoring a refusal gives the outcome the record deletion exists to prevent: the row
        // vanishes, the file survives and reload() brings the set back.  The record is tried
        // before the members are detached, so a refusal leaves everything untouched.
        REQUIRE(alien->set().name == "Alien Collection");
        alien->setChanged(false);
        mediaCenter.setRemovalRefused(true);

        CHECK_FALSE(sets.removeSet("Alien Collection"));

        REQUIRE(sets.set("Alien Collection") != nullptr);
        CHECK(sets.set("Alien Collection")->movies() == QVector<Movie*>{alien});
        CHECK(alien->set().name == "Alien Collection");
        CHECK_FALSE(alien->hasChanged());
        CHECK(mediaCenter.hasRecordOnDisk("Alien Collection"));

        sets.reload();
        CHECK(sets.set("Alien Collection") != nullptr);
    }

    SECTION("An automatic drop never removes a record")
    {
        // removeSet() is the only path in the model that deletes a file; dropEmptySets() only
        // destroys objects.  The fixture is the state that lets the fence bite: records enabled,
        // and a record on disk for a set whose flag is still false because no reload has run.
        // Turning records off would prove nothing, as every removal then refuses anyway.
        sets.addSet("Predator Collection");
        REQUIRE(sets.set("Predator Collection") != nullptr);
        REQUIRE_FALSE(sets.set("Predator Collection")->hasRecord());
        mediaCenter.putRecord("Predator Collection");

        movies.clear();
        qApp->processEvents();

        REQUIRE(sets.set("Predator Collection") == nullptr);
        CHECK(mediaCenter.hasRecordOnDisk("Predator Collection"));
    }

    SECTION("Turning the folder off and on again does not cost a set its record")
    {
        // reload() must leave the flags alone while records are off: the media center answers with
        // an empty list, so re-deriving would clear them and the next drop would destroy a set
        // whose `set.nfo` is still on disk.
        mediaCenter.setRecordsEnabled(false);
        sets.reload(); // a visit to the sets tab while the folder is switched off
        mediaCenter.setRecordsEnabled(true);

        movies.clear();
        qApp->processEvents();

        CHECK(sets.set("Alien Collection") != nullptr);
    }
}

TEST_CASE("A set with a record but no movie is found at all", "[model][movie][set]")
{
    // Every other set arrives because a movie named it; a curated set that was never filled
    // is named by none, so the records are listed too.
    QObject owner;
    MovieModel movies;
    MediaCenterInterfaceMock mediaCenter;
    MovieSetModel sets;

    Movie* alien = movieInSet(owner, "Alien", "Alien Collection");
    movies.addMovie(alien);
    mediaCenter.putRecord("Curated Collection", {"Nothing in it yet.", TmdbId::NoId});
    sets.setMovieModel(&movies);
    sets.setRecordSource(&mediaCenter);

    SECTION("The set exists, with its record read")
    {
        REQUIRE(sets.set("Curated Collection") != nullptr);
        CHECK(sets.set("Curated Collection")->movies().isEmpty());
        CHECK(sets.set("Curated Collection")->overview() == "Nothing in it yet.");
        CHECK_FALSE(sets.set("Curated Collection")->hasChanged());
    }

    SECTION("A movie can join it afterwards")
    {
        MovieSetInfo curated;
        curated.name = "Curated Collection";
        sets.assign(alien, curated);
        CHECK(sets.set("Curated Collection")->movies() == QVector<Movie*>{alien});
        // Joining a set does not rebuild it, so the record's overview is still there.
        CHECK(sets.set("Curated Collection")->overview() == "Nothing in it yet.");
    }

    SECTION("Removing it deliberately does not bring it back")
    {
        CHECK(sets.removeSet("Curated Collection"));
        sets.reload();
        CHECK(sets.set("Curated Collection") == nullptr);
    }

    SECTION("Nothing is listed when no folder is configured")
    {
        mediaCenter.setRecordsEnabled(false);
        sets.reload();
        CHECK(sets.set("Curated Collection") == nullptr);
        CHECK(sets.set("Alien Collection") != nullptr);
    }
}

TEST_CASE("A set derived from movies knows what its members know", "[model][movie][set]")
{
    // A derived set is created from a name alone, although every member NFO may carry the
    // overview and the id.  Without the seed, the first `set.nfo` written for it is written
    // from that emptiness while the members hold the data.
    QObject owner;
    MovieModel movies;

    SECTION("The overview and the id come from the member NFOs")
    {
        movies.addMovie(
            movieInSet(owner, "Alien", setInfo("Alien Collection", "Ripley versus the Alien.", TmdbId(8091))));
        MovieSetModel sets;
        sets.setMovieModel(&movies);

        REQUIRE(sets.set("Alien Collection") != nullptr);
        CHECK(sets.set("Alien Collection")->overview() == "Ripley versus the Alien.");
        CHECK(sets.set("Alien Collection")->tmdbId() == TmdbId(8091));
    }

    SECTION("Seeding is not an edit")
    {
        // The setters raise the changed flag, and this is not a change the user made.
        movies.addMovie(
            movieInSet(owner, "Alien", setInfo("Alien Collection", "Ripley versus the Alien.", TmdbId(8091))));
        MovieSetModel sets;
        sets.setMovieModel(&movies);

        REQUIRE(sets.set("Alien Collection") != nullptr);
        CHECK_FALSE(sets.set("Alien Collection")->hasChanged());
    }

    SECTION("An unsaved edit is not forgotten because a movie joined")
    {
        // Restored rather than cleared: the seed runs on every membership addition.
        movies.addMovie(movieInSet(owner, "Alien", setInfo("Alien Collection", "", TmdbId::NoId)));
        MovieSetModel sets;
        sets.setMovieModel(&movies);
        MovieSet* movieSet = sets.set("Alien Collection");
        REQUIRE(movieSet != nullptr);
        REQUIRE(sets.renameSet(movieSet, "Alien Anthology"));
        REQUIRE(movieSet->hasChanged());

        movies.addMovie(movieInSet(owner, "Aliens", setInfo("Alien Anthology", "Ripley returns.", TmdbId(8091))));

        CHECK(movieSet->overview() == "Ripley returns.");
        CHECK(movieSet->hasChanged());
    }

    SECTION("Members that disagree are resolved by the first one that has a value")
    {
        // Members of a library assembled by other tools disagree; the first with a value wins,
        // as in Kodi 19 and 20.
        movies.addMovie(
            movieInSet(owner, "Alien", setInfo("Alien Collection", "Ripley versus the Alien.", TmdbId(8091))));
        movies.addMovie(
            movieInSet(owner, "Aliens", setInfo("Alien Collection", "A very different summary.", TmdbId(1234))));
        MovieSetModel sets;
        sets.setMovieModel(&movies);

        REQUIRE(sets.set("Alien Collection") != nullptr);
        CHECK(sets.set("Alien Collection")->overview() == "Ripley versus the Alien.");
        CHECK(sets.set("Alien Collection")->tmdbId() == TmdbId(8091));
    }

    SECTION("A member with nothing to say does not win over a later one that has")
    {
        // "First member" means the first that carries a value, not the first in the set.
        movies.addMovie(movieInSet(owner, "Alien", setInfo("Alien Collection", "", TmdbId::NoId)));
        movies.addMovie(movieInSet(owner, "Aliens", setInfo("Alien Collection", "Ripley returns.", TmdbId(8091))));
        MovieSetModel sets;
        sets.setMovieModel(&movies);

        REQUIRE(sets.set("Alien Collection") != nullptr);
        CHECK(sets.set("Alien Collection")->overview() == "Ripley returns.");
        CHECK(sets.set("Alien Collection")->tmdbId() == TmdbId(8091));
    }

    SECTION("The overview and the id are decided independently")
    {
        movies.addMovie(
            movieInSet(owner, "Alien", setInfo("Alien Collection", "Ripley versus the Alien.", TmdbId::NoId)));
        movies.addMovie(
            movieInSet(owner, "Aliens", setInfo("Alien Collection", "A very different summary.", TmdbId(8091))));
        MovieSetModel sets;
        sets.setMovieModel(&movies);

        REQUIRE(sets.set("Alien Collection") != nullptr);
        CHECK(sets.set("Alien Collection")->overview() == "Ripley versus the Alien.");
        CHECK(sets.set("Alien Collection")->tmdbId() == TmdbId(8091));
    }

    SECTION("A member that names another set donates nothing")
    {
        // Such a movie's overview and id describe the collection it names, not this one.
        movies.addMovie(movieInSet(owner, "Alien", setInfo("Alien Collection", "", TmdbId::NoId)));
        Movie* predator = movieInSet(owner, "Predator", setInfo("Predator Collection", "The hunt.", TmdbId(399)));
        movies.addMovie(predator);
        MovieSetModel sets;
        sets.setMovieModel(&movies);
        REQUIRE(sets.set("Alien Collection") != nullptr);

        sets.set("Alien Collection")->addMovie(predator);

        CHECK(sets.set("Alien Collection")->overview().isEmpty());
        CHECK_FALSE(sets.set("Alien Collection")->tmdbId().isValid());
    }

    SECTION("A movie that enters the library afterwards brings its set's overview")
    {
        MovieSetModel sets;
        sets.setMovieModel(&movies);
        REQUIRE(sets.sets().isEmpty());

        movies.addMovie(
            movieInSet(owner, "Alien", setInfo("Alien Collection", "Ripley versus the Alien.", TmdbId(8091))));

        REQUIRE(sets.set("Alien Collection") != nullptr);
        CHECK(sets.set("Alien Collection")->overview() == "Ripley versus the Alien.");
        CHECK(sets.set("Alien Collection")->tmdbId() == TmdbId(8091));
        CHECK_FALSE(sets.set("Alien Collection")->hasChanged());
    }

    SECTION("A membership edit that creates a set seeds it")
    {
        // assign()'s contract, not a user gesture: both in-app callers build a name-only value
        // on purpose, since the previous overview and id describe the set being left.
        Movie* alien = movieInSet(owner, "Alien", setInfo("Alien Collection", "", TmdbId::NoId));
        movies.addMovie(alien);
        MovieSetModel sets;
        sets.setMovieModel(&movies);

        sets.assign(alien, setInfo("Alien Anthology", "Ripley returns.", TmdbId(8091)));

        REQUIRE(sets.set("Alien Anthology") != nullptr);
        CHECK(sets.set("Alien Anthology")->overview() == "Ripley returns.");
        CHECK(sets.set("Alien Anthology")->tmdbId() == TmdbId(8091));
        // The movie is changed by the edit and the set is not: its values were read, not edited.
        CHECK(alien->hasChanged());
        CHECK_FALSE(sets.set("Alien Anthology")->hasChanged());
    }

    SECTION("A set created by a reconciled NFO re-read is seeded")
    {
        // The production path the seed exists for: loadData() re-reads the NFO under a
        // QSignalBlocker, so syncMovie() is the only notification, and the file holds the whole
        // `<set>` block rather than a name alone.
        Movie* alien = movieInSet(owner, "Alien", setInfo("Alien Collection", "", TmdbId::NoId));
        movies.addMovie(alien);
        MovieSetModel sets;
        sets.setMovieModel(&movies);
        {
            const QSignalBlocker blocker(alien);
            alien->setSetInfo(setInfo("Alien Anthology", "Ripley returns.", TmdbId(8091)));
        }
        REQUIRE(sets.set("Alien Anthology") == nullptr);

        sets.syncMovie(alien);

        REQUIRE(sets.set("Alien Anthology") != nullptr);
        CHECK(sets.set("Alien Anthology")->overview() == "Ripley returns.");
        CHECK(sets.set("Alien Anthology")->tmdbId() == TmdbId(8091));
        CHECK_FALSE(sets.set("Alien Anthology")->hasChanged());
    }
}

TEST_CASE("A record beats the members", "[model][movie][set]")
{
    // Seeding over a record would replace an overview the user edited with a mirror of it.
    QObject owner;
    MovieModel movies;
    MediaCenterInterfaceMock mediaCenter;
    movies.addMovie(movieInSet(owner, "Alien", setInfo("Alien Collection", "What the members say.", TmdbId(1234))));

    SECTION("A record read at the set's birth is not seeded over")
    {
        mediaCenter.putRecord("Alien Collection", {"What the record says.", TmdbId(8091)});
        MovieSetModel sets;
        sets.setRecordSource(&mediaCenter);
        sets.setMovieModel(&movies);

        REQUIRE(sets.set("Alien Collection") != nullptr);
        REQUIRE(sets.set("Alien Collection")->hasRecord());
        CHECK(sets.set("Alien Collection")->overview() == "What the record says.");
        CHECK(sets.set("Alien Collection")->tmdbId() == TmdbId(8091));
        CHECK_FALSE(sets.set("Alien Collection")->hasChanged());
    }

    SECTION("A record found later replaces what was seeded")
    {
        // Configuring the folder after the sets exist is the reload that re-reads the record.
        MovieSetModel sets;
        sets.setMovieModel(&movies);
        REQUIRE(sets.set("Alien Collection")->overview() == "What the members say.");

        mediaCenter.putRecord("Alien Collection", {"What the record says.", TmdbId(8091)});
        sets.setRecordSource(&mediaCenter);

        REQUIRE(sets.set("Alien Collection") != nullptr);
        CHECK(sets.set("Alien Collection")->overview() == "What the record says.");
        CHECK(sets.set("Alien Collection")->tmdbId() == TmdbId(8091));
        CHECK_FALSE(sets.set("Alien Collection")->hasChanged());
    }

    SECTION("A record still counts while the folder is switched off")
    {
        // The gate is hasRecord(), not isBacked(): switching the folder off does not make the
        // values a set read out of its `set.nfo` stop being the record's, and the re-read branch
        // fires only when a set *gains* one, so anything seeded here would be permanent.
        mediaCenter.putRecord("Alien Collection", {"", TmdbId::NoId});
        MovieSetModel sets;
        sets.setRecordSource(&mediaCenter);
        sets.setMovieModel(&movies);
        REQUIRE(sets.set("Alien Collection") != nullptr);
        REQUIRE(sets.set("Alien Collection")->hasRecord());

        mediaCenter.setRecordsEnabled(false);
        sets.reload();

        REQUIRE(sets.set("Alien Collection") != nullptr);
        CHECK(sets.set("Alien Collection")->overview().isEmpty());
        CHECK_FALSE(sets.set("Alien Collection")->tmdbId().isValid());

        // Back on is where it would show: the record is not re-read a second time.
        mediaCenter.setRecordsEnabled(true);
        sets.reload();

        REQUIRE(sets.set("Alien Collection") != nullptr);
        REQUIRE(sets.set("Alien Collection")->hasRecord());
        CHECK(sets.set("Alien Collection")->overview().isEmpty());
        CHECK_FALSE(sets.set("Alien Collection")->tmdbId().isValid());
    }

    SECTION("A set with no record is seeded while the folder is off")
    {
        // The other side of that gate: with no record to protect the seed must still run.
        mediaCenter.setRecordsEnabled(false);
        MovieSetModel sets;
        sets.setRecordSource(&mediaCenter);
        sets.setMovieModel(&movies);

        REQUIRE(sets.set("Alien Collection") != nullptr);
        REQUIRE_FALSE(sets.set("Alien Collection")->hasRecord());
        CHECK(sets.set("Alien Collection")->overview() == "What the members say.");
        CHECK(sets.set("Alien Collection")->tmdbId() == TmdbId(1234));
    }

    SECTION("An empty record is still a record")
    {
        // An `<overview>` the user cleared is a value, so the members must not refill it.
        mediaCenter.putRecord("Alien Collection", {"", TmdbId::NoId});
        MovieSetModel sets;
        sets.setRecordSource(&mediaCenter);
        sets.setMovieModel(&movies);

        REQUIRE(sets.set("Alien Collection") != nullptr);
        REQUIRE(sets.set("Alien Collection")->hasRecord());
        CHECK(sets.set("Alien Collection")->overview().isEmpty());
        CHECK_FALSE(sets.set("Alien Collection")->tmdbId().isValid());
    }
}

TEST_CASE("A set with members survives a reload in every configuration", "[model][movie][set]")
{
    // Nothing here needs a record or writes a file, in any configuration -- and nothing marks
    // the member changed, which is the invariant that makes a reload safe to run at any time.
    QObject owner;
    MovieModel movies;
    Movie* alien = movieInSet(owner, "Alien", "Alien Collection");
    movies.addMovie(alien);

    SECTION("With no media center at all")
    {
        MovieSetModel sets;
        sets.setMovieModel(&movies);
        REQUIRE(sets.set("Alien Collection") != nullptr);
        REQUIRE_FALSE(alien->hasChanged());

        sets.reload();

        REQUIRE(sets.set("Alien Collection") != nullptr);
        CHECK(sets.set("Alien Collection")->movies() == QVector<Movie*>{alien});
        CHECK_FALSE(alien->hasChanged());
    }

    SECTION("With no movie set information folder configured")
    {
        // The state every user who has never opened the settings is in.
        MediaCenterInterfaceMock mediaCenter;
        mediaCenter.setRecordsEnabled(false);
        MovieSetModel sets;
        sets.setMovieModel(&movies);
        sets.setRecordSource(&mediaCenter);
        REQUIRE(sets.set("Alien Collection") != nullptr);
        REQUIRE_FALSE(alien->hasChanged());

        sets.reload();

        REQUIRE(sets.set("Alien Collection") != nullptr);
        CHECK(sets.set("Alien Collection")->movies() == QVector<Movie*>{alien});
        CHECK_FALSE(alien->hasChanged());
    }

    SECTION("With a folder freshly configured and no records in it yet")
    {
        // The first reload after a folder is chosen: the listing is empty, so every set is
        // told it has no record and survives on its members alone.
        MediaCenterInterfaceMock mediaCenter;
        MovieSetModel sets;
        sets.setMovieModel(&movies);
        sets.setRecordSource(&mediaCenter);
        REQUIRE(sets.set("Alien Collection") != nullptr);
        REQUIRE_FALSE(sets.set("Alien Collection")->hasRecord());
        REQUIRE_FALSE(alien->hasChanged());

        sets.reload();

        REQUIRE(sets.set("Alien Collection") != nullptr);
        CHECK(sets.set("Alien Collection")->movies() == QVector<Movie*>{alien});
        CHECK_FALSE(mediaCenter.hasRecordOnDisk("Alien Collection"));
        CHECK(mediaCenter.savedRecordCount() == 0);
        CHECK_FALSE(alien->hasChanged());
    }
}

TEST_CASE("MovieSetModel lets go of the library before it is torn down", "[model][movie][set]")
{
    // Shutdown in miniature: a movie destroyed during teardown must not reach the media center.
    QObject owner;
    MovieModel movies;
    MovieSetModel sets;
    MediaCenterInterfaceMock mediaCenter;
    mediaCenter.setRecordsEnabled(true);
    Movie* alien = movieInSet(owner, "Alien", "Alien Collection");
    movies.addMovie(alien);
    sets.setMovieModel(&movies);
    sets.setRecordSource(&mediaCenter);
    REQUIRE(sets.sets().size() == 1);

    SECTION("detaching drops the sets and the record source")
    {
        sets.detachFromLibrary();

        CHECK(sets.sets().isEmpty());
        CHECK_FALSE(sets.recordsAreConfigured());
    }

    SECTION("a movie destroyed after detaching does not reach the media center")
    {
        sets.detachFromLibrary();
        const int queriesBefore = mediaCenter.recordsEnabledQueryCount();

        delete alien;

        CHECK(mediaCenter.recordsEnabledQueryCount() == queriesBefore);
        CHECK(sets.sets().isEmpty());
    }

    SECTION("a movie destroyed while still attached does reach it")
    {
        // Without this the section above would pass even if nothing ever detached.
        const int queriesBefore = mediaCenter.recordsEnabledQueryCount();

        delete alien;

        CHECK(mediaCenter.recordsEnabledQueryCount() > queriesBefore);
    }

    SECTION("detaching twice is a no-op")
    {
        sets.detachFromLibrary();
        sets.detachFromLibrary();

        CHECK(sets.sets().isEmpty());
    }
}

TEST_CASE("Manager detaches its set model from the library before it dies", "[model][movie][set]")
{
    // The production wiring, which the four sections above do not pin: each calls
    // detachFromLibrary() itself, so removing the call from ~Manager leaves all four green.
    // Pinned through a Manager of this test's own, since the singleton cannot be destroyed.
    // Without the call the movie file searcher goes first, and each movie's destroyed() reaches
    // a set model still holding a record source -- the read that aborted MediaElch on exit.
    MediaCenterInterfaceMock mediaCenter;
    mediaCenter.setRecordsEnabled(true);

    int queriesDuringTeardown = 0;
    {
        Manager manager;
        MovieSetModel* sets = manager.movieSetModel();
        REQUIRE(sets != nullptr);
        sets->setRecordSource(&mediaCenter);
        sets->setMovieModel(manager.movieModel());

        auto* alien = new Movie({}, manager.movieFileSearcher());
        alien->setTitle("Alien");
        MovieSetInfo info;
        info.name = "Alien Collection";
        alien->setSetInfo(info);
        manager.movieModel()->addMovie(alien);
        REQUIRE(sets->sets().size() == 1);

        const int before = mediaCenter.recordsEnabledQueryCount();
        // Leaving this scope destroys the Manager, then its children in creation order.
        queriesDuringTeardown = -before;
    }
    queriesDuringTeardown += mediaCenter.recordsEnabledQueryCount();

    CHECK(queriesDuringTeardown == 0);
}
