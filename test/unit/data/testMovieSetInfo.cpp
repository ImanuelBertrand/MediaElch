#include "test/test_helpers.h"

#include "data/movie/MovieSetInfo.h"

static MovieSetInfo alienCollection()
{
    MovieSetInfo set;
    set.tmdbId = TmdbId(8091);
    set.name = "Alien Collection";
    set.overview = "A science fiction horror film franchise.";
    return set;
}

TEST_CASE("MovieSetInfo data type", "[data][movie][set]")
{
    SECTION("Renaming keeps overview and id")
    {
        const MovieSetInfo renamed = alienCollection().renamedTo("Alien Anthology");

        CHECK(renamed.name == "Alien Anthology");
        CHECK(renamed.overview == "A science fiction horror film franchise.");
        CHECK(renamed.tmdbId == TmdbId(8091));
    }

    SECTION("Renaming keeps the id of a set without an overview")
    {
        MovieSetInfo set;
        set.tmdbId = TmdbId(8091);
        set.name = "Alien Collection";

        const MovieSetInfo renamed = set.renamedTo("Alien Anthology");

        CHECK(renamed.tmdbId == TmdbId(8091));
        CHECK(renamed.overview.isEmpty());
    }
}

TEST_CASE("MovieSetInfo compares by value", "[data][movie][set]")
{
    // MovieSetModel::assign() leans on this to leave a movie alone when it is asked to
    // put it where it already is.  All three fields count, because all three are
    // written to the movie's NFO -- which also means a name-only value never compares
    // equal to a set that carries an id or an overview.

    SECTION("two default-constructed values are equal")
    {
        CHECK(MovieSetInfo{} == MovieSetInfo{});
        CHECK_FALSE(MovieSetInfo{} != MovieSetInfo{});
    }

    SECTION("a value equals itself")
    {
        CHECK(alienCollection() == alienCollection());
    }

    SECTION("each field on its own makes a difference")
    {
        const MovieSetInfo base = alienCollection();

        MovieSetInfo otherName = base;
        otherName.name = "Alien Anthology";
        CHECK(base != otherName);

        MovieSetInfo otherOverview = base;
        otherOverview.overview = "";
        CHECK(base != otherOverview);

        MovieSetInfo otherId = base;
        otherId.tmdbId = TmdbId(399);
        CHECK(base != otherId);
    }

    SECTION("a name-only value never equals a set that carries more")
    {
        // The sharp edge MovieWidget::onSetChange() and SetsWidget::onAddMovie() guard
        // against with a name comparison of their own: assign() would see a change here
        // and overwrite the id and the overview.
        MovieSetInfo nameOnly;
        nameOnly.name = alienCollection().name;

        CHECK(nameOnly != alienCollection());
    }

    SECTION("a rename is not equal to what it renamed")
    {
        CHECK(alienCollection() != alienCollection().renamedTo("Alien Anthology"));
    }
}
