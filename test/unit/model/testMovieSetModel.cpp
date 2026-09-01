#include "test/test_helpers.h"

#include "data/movie/Movie.h"
#include "data/movie/MovieController.h"
#include "data/movie/MovieSet.h"
#include "globals/Manager.h"
#include "model/MovieModel.h"
#include "model/MovieSetModel.h"
#include "scrapers/movie/MovieMerger.h"
#include "test/mocks/media_center/MediaCenterInterfaceMock.h"
#include "test/unit/scrapers/custom_movie_scraper/StubMovieScraper.h"

#include <QAbstractItemModelTester>
#include <QApplication>
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
        movie->setSetInfo(info);
    }
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

/// \brief Captures qWarning() output for as long as it is in scope.
/// \details MovieSetModel's only signal that it is throwing a set's own record away is
///          a log line, so a test that does not read the log cannot tell the warning
///          from its absence.
class WarningCapture
{
public:
    WarningCapture() : m_previous{qInstallMessageHandler(&WarningCapture::handle)} { s_messages = &m_messages; }
    ~WarningCapture()
    {
        qInstallMessageHandler(m_previous);
        s_messages = nullptr;
    }
    WarningCapture(const WarningCapture&) = delete;
    WarningCapture& operator=(const WarningCapture&) = delete;

    const QStringList& messages() const { return m_messages; }

private:
    static void handle(QtMsgType type, const QMessageLogContext& context, const QString& message)
    {
        Q_UNUSED(context)
        if (type == QtWarningMsg && s_messages != nullptr) {
            s_messages->append(message);
        }
    }

    QStringList m_messages;
    QtMessageHandler m_previous = nullptr;
    static QStringList* s_messages;
};

QStringList* WarningCapture::s_messages = nullptr;

} // namespace

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
        // A membership change dirties neither the movie nor the set on its own, and it
        // has to reach the member's NFO (D-A).  assign() is what marks it.
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
        // An edit never destroys a set, even the one it just emptied.
        REQUIRE(sets.set("Alien Collection") != nullptr);
        CHECK(sets.set("Alien Collection")->movies().isEmpty());
    }

    SECTION("assign on a movie outside the library writes the value and no membership")
    {
        // A scrape result or a freshly parsed NFO has no membership here to change.
        Movie* outsider = movieInSet(owner, "Alien 3", "");
        MovieSetInfo predator;
        predator.name = "Predator Collection";

        sets.assign(outsider, predator);

        CHECK(outsider->set().name == "Predator Collection");
        CHECK(sets.sets() == QVector<MovieSet*>{sets.set("Alien Collection")});
    }

    SECTION("assign survives a caller that has blocked the movie's signals")
    {
        // The reconcile is a direct call, not a signal, which is the whole reason the
        // model can be the authority on membership at all: MovieController::loadData()
        // blocks a movie's signals across a whole NFO re-read.
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
        // Exactly what MovieController::loadData() does around its NFO re-read: the
        // movie's set comes back different from disk and Movie::sigChanged never fires.
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
        QSignalSpy changes(&sets, &QAbstractItemModel::dataChanged);

        sets.syncMovie(alien);

        CHECK(sets.set("Alien Collection")->movies() == QVector<Movie*>{alien});
        CHECK(changes.isEmpty());
    }

    SECTION("syncMovie does not bring back a set that removeSet took away")
    {
        // removeSet() clears its members' set names as it detaches them, so there is
        // nothing left for a later reconcile to read the set back out of.
        sets.removeSet("Alien Collection");
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
        // Putting a movie where it already is must not dirty it: MediaElch would then
        // offer to rewrite an NFO the user never touched.  MovieSet's own setters make
        // the same promise, and assign() is the membership entry point beside them.
        MovieSetInfo same = alien->set();
        QSignalSpy changes(alien, &Movie::sigChanged);

        sets.assign(alien, same);

        CHECK_FALSE(alien->hasChanged());
        CHECK(changes.isEmpty());
        CHECK(sets.set("Alien Collection")->movies() == QVector<Movie*>{alien});
    }

    SECTION("assign compares the whole value, so a name-only one overwrites the rest")
    {
        // The sharp edge behind MovieWidget::onSetChange()'s own name comparison: the
        // widget hands assign() a name and nothing else, so for a movie whose set
        // carries an id or an overview the guard below never matches and the write goes
        // through.  That is right when the user picked a different set and wrong when
        // they picked the same one, which is why the widget guards on the name first.
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
        // Only the write is skipped, not the reconcile: the model can be behind the
        // movie for reasons that have nothing to do with this call.
        {
            // Exactly the shape of MovieController::loadData(): the whole re-read,
            // including its closing setChanged(false), happens inside the blocker.
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
        // The value did not change, so nothing was dirtied on its account.
        CHECK_FALSE(alien->hasChanged());
    }

    SECTION("a scrape of a library movie needs syncMovie, because the merge is blocked")
    {
        // copyDetailsToMovie() blocks the target's signals for the whole merge, so a
        // scrape writes a library movie's set where the model cannot see it.  Today
        // MovieController::scraperLoadDone() happens to repair that a frame later --
        // Movie::setChanged() emits sigChanged even when the flag does not change --
        // but that is a coincidence, so the reconcile is made explicitly and this is
        // what pins it.
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
        alien->setSetInfo(MovieSetInfo{});

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

    SECTION("a set lets go of a movie that leaves the library and that it does not name")
    {
        // MovieSet::addMovie() is public, so a set can hold a member whose own
        // set().name points somewhere else -- the state the reload section below
        // cures.  Until a reload runs, that movie can leave the library, and a set
        // that let go of only the movies naming it would keep a pointer that outlives
        // the movie.  Asking the movie for its set name is therefore not enough here.
        MovieSet* alienCollection = sets.set("Alien Collection");
        Movie* predator = movieInSet(owner, "Predator", "Predator Collection");
        movies.addMovie(predator);
        alienCollection->addMovie(predator);
        // A member the movie model never held, so that the set outlives the clear()
        // and its membership can still be read afterwards.
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
        // detachMovie() finds the sets a movie is in from an index rather than by
        // scanning all of them, so a set that is deleted has to leave that index with
        // it: nothing else would take it out, and the next detach of that movie would
        // call removeMovie() on freed memory.
        //
        // removeSet() detaches by clearing each member's set name, which takes the
        // movie out of the set that *names* it -- so a member added through the public
        // MovieSet::addMovie(), whose own name points elsewhere, is still in the set
        // when it is deleted.
        MovieSet* alienCollection = sets.set("Alien Collection");
        Movie* predator = movieInSet(owner, "Predator", "Predator Collection");
        movies.addMovie(predator);
        alienCollection->addMovie(predator);
        REQUIRE(alienCollection->movies().contains(predator));

        sets.removeSet("Alien Collection");
        REQUIRE(sets.set("Alien Collection") == nullptr);

        // The deleted set must not be reached for again on predator's way out.
        movies.clear();

        CHECK(sets.sets().isEmpty());
    }

    SECTION("an unsaved record does not exempt a set from being dropped")
    {
        // MovieSet::hasChanged() is a one-way latch -- nothing calls setChanged(false)
        // -- so exempting a changed set would exempt it for the rest of the session and
        // put a name in the set combo and the set filter that no movie answers to.
        // D-A's "a set with a `set.nfo` outlives its last member" arrives with the
        // `set.nfo` writer and a clearing edge, not with this flag.
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
        alien->setSetInfo(MovieSetInfo{});
        aliens->setSetInfo(MovieSetInfo{});

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
        // A deliberate removal is the user saying to throw the record away, and does.
        MovieSet* alienCollection = sets.addSet("Alien Collection");
        alienCollection->setOverview("A science fiction horror film franchise.");
        REQUIRE(alienCollection->hasChanged());

        sets.removeSet("Alien Collection");

        CHECK(sets.sets().isEmpty());
    }

    SECTION("removeSet says so when it discards an unsaved record")
    {
        // The log line is the only signal there is that a record was thrown away, so
        // deleting it would otherwise cost nothing that any test can see.
        sets.addSet("Alien Collection")->setOverview("A science fiction horror film franchise.");

        WarningCapture warnings;
        sets.removeSet("Alien Collection");

        REQUIRE(warnings.messages().size() == 1);
        CHECK(warnings.messages().first().contains("Alien Collection"));
    }

    SECTION("removeSet says nothing when there is no record to lose")
    {
        REQUIRE(sets.addSet("Alien Collection") != nullptr);

        WarningCapture warnings;
        sets.removeSet("Alien Collection");

        CHECK(warnings.messages().isEmpty());
    }

    SECTION("clear says so for every record it discards")
    {
        sets.addSet("Alien Collection")->setOverview("A science fiction horror film franchise.");
        sets.addSet("Predator Collection")->setTmdbId(TmdbId(399));
        sets.addSet("Rocky Collection");

        WarningCapture warnings;
        sets.clear();

        REQUIRE(warnings.messages().size() == 2);
        CHECK(warnings.messages().at(0).contains("Alien Collection"));
        CHECK(warnings.messages().at(1).contains("Predator Collection"));
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

// The only test here that uses the real Manager.  It has to: what it pins is the
// wiring in MovieController, which reaches MovieSetModel through the singleton, and
// there is no seam between them to substitute.  Manager works inside this binary --
// measured -- so the cost is one test opting in, not the whole suite depending on it.
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
    // MovieController::loadData() re-reads the NFO under a QSignalBlocker covering the
    // whole load, including its closing setChanged(false), so nothing about this write
    // reaches MovieSetModel by signal.  This is the pin for the explicit reconcile.
    //
    // Driven from cached NFO content rather than from a file: reloadFromNfo = false
    // makes KodiXml parse Movie::nfoContent(), which is the path a movie restored from
    // the database cache takes anyway, and it needs no fixture on disk.
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
    // copyDetailsToMovie() blocks the target's signals for the whole merge, so a scrape
    // writes a library movie's set where MovieSetModel cannot see it.  This is the pin
    // for MovieController's explicit reconcile.
    //
    // The scrape is run inside a QSignalBlocker on purpose.  Without it the section
    // passes either way: scraperLoadDone() calls Movie::setChanged(true) one frame
    // later and that emits sigChanged even when the flag does not change, so the model
    // is repaired by coincidence and the reconcile cannot be told from its absence.
    // Blocking the movie removes the coincidence and leaves only the direct call, which
    // is the thing under test -- a signal blocker cannot swallow it.
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
    // D-A's other half, and the reason MovieSet::hasRecord() exists.  Until `set.nfo`
    // was written, a set was nothing but the movies that named it, so re-deriving the
    // library and finding none left meant the set was gone.  A set with a record of its
    // own is not derived from anything: it has an overview, a collection id and artwork
    // that belong to the set, and it stays.
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
        // Reading what is on disk is not an edit.  Leaving the flag set would have the
        // model warn about discarding unsaved changes to a set nobody touched.
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
        // The relaxation is of one predicate, not of the rule.  A set nothing derives
        // and nothing records would otherwise sit in the set combo box and the set
        // filter with no movie answering to it.
        sets.addSet("Predator Collection");
        REQUIRE(sets.set("Predator Collection") != nullptr);
        sets.reload();
        CHECK(sets.set("Predator Collection") == nullptr);
    }

    SECTION("With no folder configured, a record counts for nothing")
    {
        // Read-only mode: with no movie set information folder there is nowhere to keep
        // a record, so no set has one and every set is its movies again.  Asked live, so
        // it takes effect at once rather than at the next reload.
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
        // Whether a set has a record is re-asked on every reload -- one directory
        // listing, not a probe per set -- so a `set.nfo` removed by something else does
        // not keep a set standing for the rest of the session.
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
        // Otherwise the record outlives the set, the next reload finds it again and the
        // set comes back: "Delete Movie Set" would delete nothing that lasted.
        sets.removeSet("Alien Collection");
        CHECK(sets.set("Alien Collection") == nullptr);
        CHECK_FALSE(mediaCenter.hasRecordOnDisk("Alien Collection"));
        sets.reload();
        CHECK(sets.set("Alien Collection") == nullptr);
    }

    SECTION("An automatic drop never removes a record")
    {
        // The only path in the model that deletes a file is removeSet(), the deliberate
        // one.  dropEmptySets() destroys objects; a library re-derivation must never
        // cost the user a file.
        mediaCenter.setRecordsEnabled(false);
        sets.assign(alien, MovieSetInfo{});
        sets.reload();
        REQUIRE(sets.set("Alien Collection") == nullptr);
        CHECK(mediaCenter.hasRecordOnDisk("Alien Collection"));
    }
}
