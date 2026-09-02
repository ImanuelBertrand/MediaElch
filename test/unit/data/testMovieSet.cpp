#include "test/test_helpers.h"

#include "data/movie/Movie.h"
#include "data/movie/MovieSet.h"

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
        // The movie-side value stays untouched; the model becomes the only writer
        // in a later step.  See docs/concepts/movie-sets.md, D-C.
        MovieSet set{"Alien Collection"};
        Movie alien;

        set.addMovie(&alien);

        CHECK(alien.set().name.isEmpty());
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
