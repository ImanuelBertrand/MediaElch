#include "test/test_helpers.h"

#include "media_center/KodiVersion.h"

using namespace mediaelch;

TEST_CASE("KodiVersion", "[media_center][kodi]")
{
    SECTION("every spelling of the default agrees with latest()")
    {
        // "The default Kodi version" used to be written out three times over -- the constructor's
        // default argument, the member initialiser and fromInt()'s out-of-range fallback --
        // so pinning them together is what makes the next bump either complete or red.
        CHECK(KodiVersion::latest().toInt() == 22);
        CHECK(KodiVersion().toInt() == KodiVersion::latest().toInt());
        CHECK(KodiVersion(KodiVersion::Latest).toInt() == KodiVersion::latest().toInt());
        // An out-of-range int lands on the same default rather than a second opinion.
        CHECK(KodiVersion(999).toInt() == KodiVersion::latest().toInt());
    }

    SECTION("isValid() and all() cover exactly the versions the enum lists")
    {
        CHECK_FALSE(KodiVersion::isValid(16));
        CHECK(KodiVersion::isValid(17));
        CHECK(KodiVersion::isValid(KodiVersion::Latest));
        CHECK_FALSE(KodiVersion::isValid(KodiVersion::Latest + 1));

        const QVector<KodiVersion> all = KodiVersion::all();
        REQUIRE_FALSE(all.isEmpty());
        CHECK(all.first().toInt() == 17);
        CHECK(all.last().toInt() == KodiVersion::latest().toInt());
        for (const KodiVersion& version : all) {
            CHECK(KodiVersion::isValid(version.toInt()));
        }
    }

    SECTION("a stored version is taken as it is")
    {
        CHECK(KodiVersion(19).toInt() == 19);
        CHECK(KodiVersion(21).toInt() == 21);
        CHECK(KodiVersion(21).toString() == "21");
    }
}
