#include "test/test_helpers.h"

#include "data/movie/Movie.h"
#include "data/movie/MovieSet.h"

#include <QSignalSpy>
#include <QVector>

TEST_CASE("MovieSet membership", "[data][movie][set]")
{
    SECTION("a new set has no movies")
    {
        MovieSet set{"Alien Collection"};
        CHECK(set.name() == "Alien Collection");
        CHECK(set.movies().isEmpty());
    }

    SECTION("addMovie appends in insertion order")
    {
        MovieSet set{"Alien Collection"};
        Movie alien;
        Movie aliens;

        set.addMovie(&alien);
        set.addMovie(&aliens);

        REQUIRE(set.movies().size() == 2);
        CHECK(set.movies().at(0) == &alien);
        CHECK(set.movies().at(1) == &aliens);
    }

    SECTION("adding the same movie twice does not duplicate it")
    {
        MovieSet set{"Alien Collection"};
        Movie alien;

        set.addMovie(&alien);
        set.addMovie(&alien);

        CHECK(set.movies().size() == 1);
    }

    SECTION("a null movie is not added")
    {
        MovieSet set{"Alien Collection"};
        set.addMovie(nullptr);
        CHECK(set.movies().isEmpty());
    }

    SECTION("removeMovie removes only the given movie")
    {
        MovieSet set{"Alien Collection"};
        Movie alien;
        Movie aliens;
        set.addMovie(&alien);
        set.addMovie(&aliens);

        set.removeMovie(&alien);

        REQUIRE(set.movies().size() == 1);
        CHECK(set.movies().at(0) == &aliens);
    }

    SECTION("removing a movie that is not a member changes nothing")
    {
        MovieSet set{"Alien Collection"};
        Movie alien;
        Movie predator;
        set.addMovie(&alien);

        int changes = 0;
        QObject::connect(&set, &MovieSet::sigChanged, [&changes](MovieSet*) { ++changes; });

        set.removeMovie(&predator);
        set.removeMovie(nullptr);

        REQUIRE(set.movies().size() == 1);
        CHECK(set.movies().at(0) == &alien);
        CHECK(changes == 0);
    }

    SECTION("a destroyed movie removes itself from the set")
    {
        // A set does not own its members, and nothing else tells it when one dies:
        // Movie::sigChanged is not emitted from ~Movie and MovieModel::clear() only
        // calls deleteLater() on every movie.  See docs/concepts/movie-sets.md, D-C.
        MovieSet set{"Alien Collection"};
        Movie aliens;
        {
            Movie alien;
            set.addMovie(&alien);
            set.addMovie(&aliens);
            REQUIRE(set.movies().size() == 2);
        }

        REQUIRE(set.movies().size() == 1);
        CHECK(set.movies().at(0) == &aliens);
    }

    SECTION("a movie destroyed after leaving the set changes nothing")
    {
        MovieSet set{"Alien Collection"};
        Movie aliens;
        set.addMovie(&aliens);

        int changes = 0;
        QObject::connect(&set, &MovieSet::sigChanged, [&changes](MovieSet*) { ++changes; });
        {
            Movie alien;
            set.addMovie(&alien);
            set.removeMovie(&alien);
        }

        REQUIRE(set.movies().size() == 1);
        CHECK(set.movies().at(0) == &aliens);
        CHECK(changes == 2); // the add and the remove, nothing for the destruction
    }

    SECTION("clearMovies removes every member")
    {
        MovieSet set{"Alien Collection"};
        Movie alien;
        Movie aliens;
        set.addMovie(&alien);
        set.addMovie(&aliens);

        set.clearMovies();

        CHECK(set.movies().isEmpty());
        // Membership is not part of the set's own record (D-A).
        CHECK_FALSE(set.hasChanged());
    }

    SECTION("clearing a set without members changes nothing")
    {
        MovieSet set{"Alien Collection"};
        int changes = 0;
        QObject::connect(&set, &MovieSet::sigChanged, [&changes](MovieSet*) { ++changes; });

        set.clearMovies();

        CHECK(changes == 0);
    }

    SECTION("a movie destroyed after clearMovies changes nothing")
    {
        MovieSet set{"Alien Collection"};
        int changes = 0;
        {
            Movie alien;
            set.addMovie(&alien);
            set.clearMovies();
            // The destroyed() connection outlives the membership on purpose, so the
            // handler has to be a no-op for a movie that is no longer a member.
            QObject::connect(&set, &MovieSet::sigChanged, [&changes](MovieSet*) { ++changes; });
        }

        CHECK(set.movies().isEmpty());
        CHECK(changes == 0);
    }

    SECTION("membership does not write the set onto the movie")
    {
        // This section was planted as a tripwire, to be retired by the step that made
        // MovieSetModel the writer.  That step has now been taken, and the assertion
        // survives it -- because the division of labour it describes turned out to be
        // the permanent one rather than an interim state.
        //
        // A movie's MovieSetInfo is the value its own file carries.  A MovieSet's
        // membership is the model's.  Putting a movie into a set here would write the
        // value from a second place, which is the duplicated state the split exists to
        // remove.  MovieSetModel::assign() does both halves, and it is the only thing
        // that does; see docs/concepts/movie-sets.md, D-C, and the test case
        // "MovieSetModel is the only thing that changes membership".
        MovieSet set{"Alien Collection"};
        Movie alien;

        set.addMovie(&alien);

        CHECK(alien.set().name.isEmpty());
        CHECK(set.movies() == QVector<Movie*>{&alien});
        // Nor does it dirty the movie, which is the other half of the same warning:
        // a membership edit that has to reach disk is marked by MovieSetModel::assign(),
        // and by nothing here.
        CHECK_FALSE(alien.hasChanged());
    }
}

TEST_CASE("MovieSet change notification", "[data][movie][set]")
{
    SECTION("sigChanged fires once per real change")
    {
        MovieSet set{"Alien Collection"};
        int changes = 0;
        QObject::connect(&set, &MovieSet::sigChanged, [&changes](MovieSet*) { ++changes; });

        set.setOverview("A science fiction horror film franchise.");
        CHECK(changes == 1);
        CHECK(set.hasChanged());

        set.setTmdbId(TmdbId(8091));
        set.setName("Alien Anthology");
        CHECK(changes == 3);
    }

    SECTION("setting a value to what it already is changes nothing")
    {
        MovieSet set{"Alien Collection"};
        set.setOverview("A science fiction horror film franchise.");
        set.setChanged(false);

        int changes = 0;
        QObject::connect(&set, &MovieSet::sigChanged, [&changes](MovieSet*) { ++changes; });

        set.setName("Alien Collection");
        set.setOverview("A science fiction horror film franchise.");
        set.setTmdbId(TmdbId::NoId);
        // The fourth scalar setter.  Empty is what a set with no display title of its
        // own already holds, and setting the title *to the name* normalises to empty --
        // so neither is a change.
        set.setTitle(QString());
        set.setTitle("Alien Collection");

        CHECK(changes == 0);
        CHECK_FALSE(set.hasChanged());
    }

    SECTION("adding a movie notifies but does not dirty the set's own record")
    {
        // Membership lives in the member movies' NFOs, not in set.nfo (D-A).
        MovieSet set{"Alien Collection"};
        Movie alien;
        int changes = 0;
        QObject::connect(&set, &MovieSet::sigChanged, [&changes](MovieSet*) { ++changes; });

        set.addMovie(&alien);
        set.removeMovie(&alien);

        CHECK(changes == 2);
        CHECK_FALSE(set.hasChanged());
    }
}

TEST_CASE("MovieSet display title", "[data][movie][set]")
{
    SECTION("a new set is displayed under its key")
    {
        MovieSet set{"Alien Collection"};
        CHECK(set.title().isEmpty());
        CHECK(set.displayName() == "Alien Collection");
    }

    SECTION("a set-file-only rename moves the title and leaves the key alone")
    {
        MovieSet set{"Alien Collection"};
        set.setTitle("The Alien Saga");

        CHECK(set.name() == "Alien Collection");
        CHECK(set.title() == "The Alien Saga");
        CHECK(set.displayName() == "The Alien Saga");
        CHECK(set.hasChanged());
    }

    SECTION("clearing the title puts the display name back on the key")
    {
        MovieSet set{"Alien Collection"};
        set.setTitle("The Alien Saga");
        set.setTitle(QString());

        CHECK(set.displayName() == "Alien Collection");
        CHECK(set.title().isEmpty());
    }

    SECTION("a title equal to the key is stored as no title at all")
    {
        // One representation for "there is no divergence", or the writer would emit a
        // redundant <title> that the reader then declines to read back, and the two
        // would disagree about what an un-renamed set looks like.
        MovieSet set{"Alien Collection"};
        set.setTitle("Alien Collection");
        CHECK(set.title().isEmpty());
    }

    SECTION("moving the key re-unifies the two names")
    {
        // An all-movie-files rename rewrites every member's <set><name> and the record's
        // <originaltitle> alike, so there is no separate display title left to hold.
        MovieSet set{"Alien Collection"};
        set.setTitle("The Alien Saga");
        set.setName("Alien Anthology");

        CHECK(set.name() == "Alien Anthology");
        CHECK(set.title().isEmpty());
        CHECK(set.displayName() == "Alien Anthology");
    }

    SECTION("a key moved onto its own display title still dirties the set")
    {
        // The order inside setName() matters: clear the title first, and the set is
        // still marked as needing to be saved.  Clear it after a no-op check on the
        // title and this rename would look like nothing happened.
        MovieSet set{"Alien Collection"};
        set.setTitle("The Alien Saga");
        set.setChanged(false);

        set.setName("The Alien Saga");

        CHECK(set.name() == "The Alien Saga");
        CHECK(set.title().isEmpty());
        CHECK(set.hasChanged());
    }
}

TEST_CASE("MovieSetImages", "[data][movie][set]")
{
    SECTION("an image is stored as the bytes it was given")
    {
        // Set artwork must not be re-encoded; it is stored like every other image.
        MovieSet set{"Alien Collection"};
        const QByteArray poster("\x89PNG-not-a-jpeg", 15);

        set.images().setImage(ImageType::MovieSetPoster, poster);

        CHECK(set.images().image(ImageType::MovieSetPoster) == poster);
        CHECK(set.images().hasImage(ImageType::MovieSetPoster));
        CHECK(set.images().imageHasChanged(ImageType::MovieSetPoster));
        CHECK(set.hasChanged());
    }

    SECTION("image types do not bleed into each other")
    {
        MovieSet set{"Alien Collection"};
        set.images().setImage(ImageType::MovieSetPoster, QByteArray("poster"));

        CHECK(set.images().image(ImageType::MovieSetBackdrop).isEmpty());
        CHECK_FALSE(set.images().hasImage(ImageType::MovieSetBackdrop));
        CHECK_FALSE(set.images().imageHasChanged(ImageType::MovieSetBackdrop));
    }

    SECTION("removing an image that only exists on disk schedules it for deletion")
    {
        // The disk scan reports artwork with setHasImage() and loads no bytes.
        MovieSet set{"Alien Collection"};
        set.images().setHasImage(ImageType::MovieSetBackdrop, true);
        set.setChanged(false);

        set.images().removeImage(ImageType::MovieSetBackdrop);

        CHECK(set.images().imagesToRemove().contains(ImageType::MovieSetBackdrop));
        CHECK_FALSE(set.images().hasImage(ImageType::MovieSetBackdrop));
        // Without this the saver skips the set and the file stays on disk.
        CHECK(set.hasChanged());
    }

    SECTION("removing a downloaded image drops it without scheduling a deletion")
    {
        MovieSet set{"Alien Collection"};
        set.images().setImage(ImageType::MovieSetPoster, QByteArray("poster"));
        set.setChanged(false);

        set.images().removeImage(ImageType::MovieSetPoster);

        CHECK(set.images().image(ImageType::MovieSetPoster).isEmpty());
        CHECK_FALSE(set.images().hasImage(ImageType::MovieSetPoster));
        CHECK_FALSE(set.images().imageHasChanged(ImageType::MovieSetPoster));
        CHECK_FALSE(set.images().imagesToRemove().contains(ImageType::MovieSetPoster));
        CHECK(set.hasChanged());
    }

    SECTION("re-adding a removed image cancels its pending deletion")
    {
        // Deliberately unlike MovieImages::setImage(), which does not clear the
        // pending deletion: otherwise a writer honouring both would write the new
        // poster and then delete it again, or lose the write, depending on order.
        MovieSet set{"Alien Collection"};
        set.images().setHasImage(ImageType::MovieSetPoster, true);
        set.images().removeImage(ImageType::MovieSetPoster);
        REQUIRE(set.images().imagesToRemove().contains(ImageType::MovieSetPoster));

        set.images().setImage(ImageType::MovieSetPoster, QByteArray("new poster"));

        CHECK_FALSE(set.images().imagesToRemove().contains(ImageType::MovieSetPoster));
        CHECK(set.images().image(ImageType::MovieSetPoster) == QByteArray("new poster"));
    }

    SECTION("clearImages frees the bytes but keeps the knowledge that art exists")
    {
        MovieSet set{"Alien Collection"};
        set.images().setImage(ImageType::MovieSetPoster, QByteArray("poster"));
        set.images().setImage(ImageType::MovieSetBackdrop, QByteArray("backdrop"));

        set.images().clearImages();

        CHECK(set.images().image(ImageType::MovieSetPoster).isEmpty());
        CHECK(set.images().image(ImageType::MovieSetBackdrop).isEmpty());
        // The saver still has to know which artwork the set has.
        CHECK(set.images().hasImage(ImageType::MovieSetPoster));
        CHECK(set.images().hasImage(ImageType::MovieSetBackdrop));
    }

    SECTION("both existing set image types are supported")
    {
        CHECK(MovieSetImages::isSupportedImageType(ImageType::MovieSetPoster));
        CHECK(MovieSetImages::isSupportedImageType(ImageType::MovieSetBackdrop));
        CHECK_FALSE(MovieSetImages::isSupportedImageType(ImageType::MoviePoster));
    }
}

TEST_CASE("MovieSet announces membership per movie", "[data][movie][set]")
{
    // sigChanged says only "something about this set changed", which is enough to
    // repaint a row and not enough to maintain an index of which sets a movie is in.
    // MovieSetModel keeps such an index, so membership is announced per movie as well.

    SECTION("addMovie announces the movie that joined")
    {
        MovieSet set{"Alien Collection"};
        Movie alien;
        QSignalSpy added(&set, &MovieSet::sigMovieAdded);

        set.addMovie(&alien);

        REQUIRE(added.size() == 1);
        CHECK(added.at(0).at(0).value<MovieSet*>() == &set);
        CHECK(added.at(0).at(1).value<Movie*>() == &alien);
    }

    SECTION("adding the same movie twice announces it once")
    {
        MovieSet set{"Alien Collection"};
        Movie alien;
        QSignalSpy added(&set, &MovieSet::sigMovieAdded);

        set.addMovie(&alien);
        set.addMovie(&alien);

        CHECK(added.size() == 1);
    }

    SECTION("removeMovie announces the movie that left")
    {
        MovieSet set{"Alien Collection"};
        Movie alien;
        set.addMovie(&alien);
        QSignalSpy removed(&set, &MovieSet::sigMovieRemoved);

        set.removeMovie(&alien);

        REQUIRE(removed.size() == 1);
        CHECK(removed.at(0).at(0).value<MovieSet*>() == &set);
        CHECK(removed.at(0).at(1).value<QObject*>() == static_cast<QObject*>(&alien));

        // A movie that is not a member announces nothing.
        set.removeMovie(&alien);
        CHECK(removed.size() == 1);
    }

    SECTION("clearMovies announces every member, not just the fact that it emptied")
    {
        // An index keyed by movie cannot be repaired from one collective signal, so
        // emptying a set has to name each movie it drops.
        MovieSet set{"Alien Collection"};
        Movie alien;
        Movie aliens;
        set.addMovie(&alien);
        set.addMovie(&aliens);
        QSignalSpy removed(&set, &MovieSet::sigMovieRemoved);

        set.clearMovies();

        REQUIRE(removed.size() == 2);
        CHECK(removed.at(0).at(1).value<QObject*>() == static_cast<QObject*>(&alien));
        CHECK(removed.at(1).at(1).value<QObject*>() == static_cast<QObject*>(&aliens));
        // The membership a handler sees is the one the call leaves behind.
        CHECK(set.movies().isEmpty());

        set.clearMovies();
        CHECK(removed.size() == 2);
    }

    SECTION("a destroyed member is announced like any other departure")
    {
        MovieSet set{"Alien Collection"};
        QSignalSpy removed(&set, &MovieSet::sigMovieRemoved);
        {
            Movie alien;
            set.addMovie(&alien);
            REQUIRE(removed.isEmpty());
        }

        REQUIRE(removed.size() == 1);
        CHECK(removed.at(0).at(0).value<MovieSet*>() == &set);
    }
}
