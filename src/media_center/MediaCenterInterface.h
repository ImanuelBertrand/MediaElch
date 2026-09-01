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

    /// \brief Whether set artwork has somewhere to go, i.e. whether the layout resolves.
    /// \details **Deliberately not the same question as movieSetRecordsEnabled().**  A
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
    virtual void saveMovieSetPoster(QString setName, QImage poster) = 0;
    virtual void saveMovieSetBackdrop(QString setName, QImage backdrop) = 0;

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
