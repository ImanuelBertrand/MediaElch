#include "test/test_helpers.h"

#include "media/Path.h"
#include "settings/Settings.h"
#include "test/helpers/movie_set_settings.h"
#include "ui/settings/MovieSettingsWidget.h"

#include <QLineEdit>

namespace {

/// \brief Restores the two movie set artwork settings this file writes.
class ArtworkSettingsGuard
{
public:
    ArtworkSettingsGuard() :
        m_type{Settings::instance()->movieSetArtworkType()}, m_dir{Settings::instance()->movieSetArtworkDirectory()}
    {
    }
    ~ArtworkSettingsGuard()
    {
        Settings::instance()->setMovieSetArtworkType(m_type);
        Settings::instance()->setMovieSetArtworkDirectory(m_dir);
    }
    ArtworkSettingsGuard(const ArtworkSettingsGuard&) = delete;
    ArtworkSettingsGuard& operator=(const ArtworkSettingsGuard&) = delete;

private:
    MovieSetArtworkType m_type;
    mediaelch::DirectoryPath m_dir;
};

} // namespace

TEST_CASE("The movie settings do not invent a movie set directory", "[ui][movie][set][settings]")
{
    // DirectoryPath::toNativePathString() resolves through QDir::absolutePath(), and an
    // invalid path wraps a default QDir whose absolutePath() is the *process's current
    // working directory*.  Showing that in the directory field and reading it back on
    // save turned "no directory chosen" into "the directory MediaElch was started from",
    // permanently and behind the user's back -- and once that is stored,
    // movieSetRecordsEnabled() answers true, KodiXml::movieSetFileName() has nothing left
    // to refuse, and records and artwork are written into the working directory after
    // all.  The guard in the media center cannot see this coming; only this can.
    ArtworkSettingsGuard settingsGuard;
    // saveSettings() rewrites the whole data file list from its line edits.
    test::DataFileGuard dataFiles;

    Settings::instance()->setMovieSetArtworkType(MovieSetArtworkType::ArtworkNextToMovies);
    Settings::instance()->setMovieSetArtworkDirectory(mediaelch::DirectoryPath());
    REQUIRE_FALSE(Settings::instance()->movieSetArtworkDirectory().isValid());

    MovieSettingsWidget widget;
    widget.setSettings(*Settings::instance());
    widget.loadSettings();

    auto* dirField = widget.findChild<QLineEdit*>("movieSetArtworkDir");
    REQUIRE(dirField != nullptr);
    CHECK(dirField->text().isEmpty());

    // And a save that touches nothing keeps it that way.  This is the press that used to
    // do the damage: it need not have anything to do with movie sets at all.
    widget.saveSettings();
    CHECK_FALSE(Settings::instance()->movieSetArtworkDirectory().isValid());
}

TEST_CASE("The movie settings keep a directory that was chosen", "[ui][movie][set][settings]")
{
    // The other direction, so that the check above cannot be satisfied by a dialog that
    // forgets the setting -- including across the layout in which the field is disabled,
    // which is why saveSettings() writes it whichever layout is selected.
    ArtworkSettingsGuard settingsGuard;
    test::DataFileGuard dataFiles;

    const QDir chosen = QDir::current();
    Settings::instance()->setMovieSetArtworkType(MovieSetArtworkType::ArtworkNextToMovies);
    Settings::instance()->setMovieSetArtworkDirectory(mediaelch::DirectoryPath(chosen));

    MovieSettingsWidget widget;
    widget.setSettings(*Settings::instance());
    widget.loadSettings();

    auto* dirField = widget.findChild<QLineEdit*>("movieSetArtworkDir");
    REQUIRE(dirField != nullptr);
    CHECK_FALSE(dirField->text().isEmpty());

    widget.saveSettings();
    CHECK(Settings::instance()->movieSetArtworkDirectory().isValid());
    CHECK(Settings::instance()->movieSetArtworkDirectory().toString() == chosen.absolutePath());
}
