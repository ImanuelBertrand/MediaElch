#pragma once

#include "data/Actor.h"
#include "data/tv_show/SeasonNumber.h"
#include "globals/Globals.h"
#include "settings/DataFile.h"

#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

class Album;
class Artist;
class Concert;
class Movie;
class MovieSet;
class TvShow;
class TvShowEpisode;

/// \brief The MediaCenterInterface class
/// This class is the base for every MediaCenter.
class MediaCenterInterface : public QObject
{
    Q_OBJECT
public:
    explicit MediaCenterInterface(QObject* parent) : QObject(parent) {}

    // movies
    virtual bool saveMovie(Movie* movie) = 0;
    virtual bool loadMovie(Movie* movie, QString nfoContent = "") = 0;
    // movie images (e.g. posters)

    /// \brief Whether the configured artwork layout resolves at all.
    /// \details A question about the **layout**, not about any one set.  It is what the
    ///          UI needs, and it is weaker than "this set's artwork can be written": in
    ///          the artwork-next-to-movies layout the path is resolved through a member
    ///          movie's folder, so a set whose members have no files still has nowhere to
    ///          put artwork and its save still refuses, while this answers true.
    ///
    ///          Harmless today, because a refused save keeps the image and says so, and
    ///          because a set with no members does not linger in that layout.  **Worth
    ///          re-checking in step 6/7**, where an automatic or bulk artwork write would
    ///          meet that gap without a user watching the result.
    ///
    ///          **Deliberately not the same question as movieSetRecordsEnabled().**  A
    ///          set's *record* only ever lives in the movie set information folder, but
    ///          its *artwork* lives in both layouts: "artwork next to movies" writes it
    ///          beside the movie folders, and that is MediaElch's shipping default.
    ///          Gating artwork on the record predicate would therefore take set artwork
    ///          away from every user who has never opened the settings.
    ///
    ///          The only configuration with nowhere to put artwork is the separate
    ///          folder selected without a folder having been chosen, which used to
    ///          resolve to the process's working directory.
    ///
    ///          An implementation must answer this by *calling* movieSetRecordsEnabled()
    ///          rather than repeating its condition, so that the two cannot drift apart.
    ///          That the two are different questions, and which configurations separate
    ///          them, is pinned by the truth-table section of
    ///          test/integration/media_center/testKodi_v22_movie_set.cpp.
    ELCH_NODISCARD virtual bool movieSetArtworkEnabled() const = 0;
    virtual QImage movieSetPoster(QString setName) = 0;
    virtual QImage movieSetBackdrop(QString setName) = 0;
    /// \brief Writes \p poster as the set's poster.  Returns whether it was written.
    /// \details Refuses when *this set* has nowhere to put it, which is a narrower thing
    ///          than movieSetArtworkEnabled() answers -- see there -- and reports a write
    ///          that failed.  **The caller has to listen.**  A
    ///          set's artwork exists only in the sets tab's own map until it is saved,
    ///          so a caller that clears that map on a refusal destroys the image and
    ///          then reports success.  That is what this return value was added for.
    ///
    ///          ELCH_NODISCARD is silent on a virtual under GCC (reproduced, 14.2), so
    ///          nothing but a test holds this refusal; see
    ///          test/unit/ui/testSetsWidget.cpp.
    ELCH_NODISCARD virtual bool saveMovieSetPoster(QString setName, QImage poster) = 0;
    /// \brief Writes \p backdrop as the set's backdrop.  Returns whether it was written.
    /// \details See saveMovieSetPoster(); the same refusals and the same obligation.
    ELCH_NODISCARD virtual bool saveMovieSetBackdrop(QString setName, QImage backdrop) = 0;

    // movie sets: the set's own record, `set.nfo` (docs/concepts/movie-sets.md, D-A)
    //
    // A set's overview, TMDB id and artwork are authoritative in `set.nfo`, which lives
    // in the movie set information folder.  That folder only exists in one of the two
    // artwork layouts, so every one of these is a no-op when it is not configured --
    // and that is the design, not a gap: with no folder there is nowhere to put a
    // record, so no set has one and sets are read-only.

    /// \brief Whether set records can be stored at all, i.e. whether a folder is configured.
    /// \details Asked live rather than remembered, so that changing the setting takes
    ///          effect at once instead of at the next reload.
    ELCH_NODISCARD virtual bool movieSetRecordsEnabled() const = 0;
    /// \brief The names of all sets that have a record, read from the records themselves.
    /// \details Empty when records are not enabled.  This is how a set with no member
    ///          movies is found at all: nothing else knows it exists, because a set is
    ///          otherwise only ever derived from the movies that name it.
    ELCH_NODISCARD virtual QStringList movieSetsWithRecord() = 0;
    /// \brief Reads \p set's record into it.  Returns whether one was found.
    /// \details Leaves the set unchanged and returns false when there is no record.
    virtual bool loadMovieSet(MovieSet& set) = 0;
    /// \brief Writes \p set's record.  Returns whether it was written.
    virtual bool saveMovieSet(MovieSet& set) = 0;
    /// \brief Deletes the record of the set called \p setName.  Returns whether it is gone.
    /// \details Removes the record only, never the folder or the artwork in it.  A set
    ///          whose record outlived it would be found again by movieSetsWithRecord()
    ///          and come back, so a deliberate removal has to take the record with it.
    virtual bool removeMovieSetRecord(const QString& setName) = 0;
    /// \brief How much of a set's on-disk state a rename managed to move.
    /// \details Three answers because the caller has to *say* which happened, and two of
    ///          them are true in different places.  The separate-folder layout renames a
    ///          directory and then renames the set-name-derived files inside it, so the
    ///          directory can move while a file in it does not: the record is at the new
    ///          name and some artwork beside it still spells the old one.  Telling that
    ///          user their files are "still stored under the old name" would send them to
    ///          a folder that no longer exists.
    enum class MovieSetFileMove
    {
        /// Everything moved, or there was nothing to move.
        Moved,
        /// Nothing moved.  Everything is still where it was, under the old name.
        NotMoved,
        /// Some moved and some did not; neither name has the whole set.
        PartlyMoved
    };

    /// \brief Moves everything a set keeps on disk from \p oldName's place to \p newName's.
    /// \details The set's `set.nfo` **and** its artwork, together, because an
    ///          all-movie-files rename moves the match key and Kodi derives the movie set
    ///          information folder from that key
    ///          (`VideoInfoScanner.cpp:839`, before the record is even loaded).  Leave the
    ///          record behind and `movieSetsWithRecord()` reports the old name at the next
    ///          reload, resurrecting the set as a memberless ghost; leave the artwork
    ///          behind and Kodi finds an empty folder for the renamed set.
    ///
    ///          Answers `Moved` when the move succeeded **or there was nothing to move**
    ///          -- no folder configured, no files under the old name, or a name that
    ///          legalises to the same path.  The other two mean files exist and are not
    ///          all where the new name says, which the caller must report: the rename
    ///          itself is not undone, because the movie NFOs are the set's identity and a
    ///          rename whose artwork move failed is still a rename that happened.
    ///
    /// \warning Must be called **before** the members are reassigned.  In the
    ///          artwork-next-to-movies layout the paths are found through a movie whose
    ///          `set().name` is still the old one, so afterwards there is nothing left to
    ///          resolve them from and the artwork is orphaned silently.
    virtual MovieSetFileMove renameMovieSetFiles(const QString& oldName, const QString& newName) = 0;

    // concerts
    virtual bool saveConcert(Concert* concert) = 0;
    virtual bool loadConcert(Concert* concert, QString nfoContent = "") = 0;

    // TV shows
    virtual bool loadTvShow(TvShow* show, QString nfoContent = "") = 0;
    virtual bool loadTvShowEpisode(TvShowEpisode* episode, QString nfoContent = "") = 0;
    virtual bool saveTvShow(TvShow* show) = 0;
    virtual bool saveTvShowEpisode(TvShowEpisode* episode) = 0;

    // fanart
    virtual QStringList extraFanartNames(Movie* movie) = 0;
    virtual QStringList extraFanartNames(TvShow* show) = 0;
    virtual QStringList extraFanartNames(Concert* concert) = 0;
    virtual QStringList extraFanartNames(Artist* artist) = 0;

    // music
    virtual bool saveArtist(Artist* artist) = 0;
    virtual bool saveAlbum(Album* album) = 0;
    virtual bool loadArtist(Artist* artist, QString initialNfoContent = "") = 0;
    virtual bool loadAlbum(Album* album, QString initialNfoContent = "") = 0;

    // actors
    virtual QString actorImageName(Movie* movie, Actor actor) = 0;
    virtual QString actorImageName(TvShow* show, Actor actor) = 0;
    virtual QString actorImageName(TvShowEpisode* episode, Actor actor) = 0;

    // nfo file paths
    virtual QString nfoFilePath(Movie* movie) = 0;
    virtual QString nfoFilePath(Concert* concert) = 0;
    virtual QString nfoFilePath(TvShowEpisode* episode) = 0;
    virtual QString nfoFilePath(TvShow* show) = 0;
    virtual QString nfoFilePath(Artist* artist) = 0;
    virtual QString nfoFilePath(Album* album) = 0;

    // clang-format off
    virtual QString imageFileName(const Movie *movie,           ImageType type, QVector<DataFile> dataFiles = QVector<DataFile>(), bool constructName = false) = 0;
    virtual QString imageFileName(const Concert *concert,       ImageType type, QVector<DataFile> dataFiles = QVector<DataFile>(), bool constructName = false) = 0;
    virtual QString imageFileName(const TvShowEpisode *episode, ImageType type, QVector<DataFile> dataFiles = QVector<DataFile>(), bool constructName = false) = 0;
    virtual QString imageFileName(const Artist *artist,         ImageType type, QVector<DataFile> dataFiles = QVector<DataFile>(), bool constructName = false) = 0;
    virtual QString imageFileName(const Album *album,           ImageType type, QVector<DataFile> dataFiles = QVector<DataFile>(), bool constructName = false) = 0;
    virtual QString imageFileName(const TvShow *show,           ImageType type, SeasonNumber season = SeasonNumber::NoSeason, QVector<DataFile> dataFiles = QVector<DataFile>(), bool constructName = false) = 0;
    // clang-format on

    virtual void loadBooklets(Album* album) = 0;
};
