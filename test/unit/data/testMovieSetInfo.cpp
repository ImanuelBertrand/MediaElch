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
