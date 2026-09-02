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
    /// \details A question about the layout, not about one set: in the artwork-next-to-movies
    ///          layout a set whose members have no files still has nowhere to put artwork.
    ///          Not the same question as movieSetRecordsEnabled(): records live only in the
    ///          movie set information folder, artwork also next to the movies, which is the
    ///          default.  Implementations should answer by calling movieSetRecordsEnabled().
    ELCH_NODISCARD virtual bool movieSetArtworkEnabled() const = 0;
    virtual QImage movieSetPoster(QString setName) = 0;
    virtual QImage movieSetBackdrop(QString setName) = 0;
    /// \brief Writes \p poster as the set's poster.  Returns whether it was written.
    /// \details Refuses when this set has nowhere to put it.  Callers must not discard the
    ///          unsaved image on a refusal -- ELCH_NODISCARD is silent on a virtual under GCC,
    ///          so that rule is held by a test and not by the compiler.
    ELCH_NODISCARD virtual bool saveMovieSetPoster(QString setName, QImage poster) = 0;
    /// \brief Writes \p backdrop as the set's backdrop.  Returns whether it was written.
    /// \details See saveMovieSetPoster(); the same refusals and the same obligation.
    ELCH_NODISCARD virtual bool saveMovieSetBackdrop(QString setName, QImage backdrop) = 0;

    // movie sets: the set's own record, `set.nfo`
    //
    // A record lives in the movie set information folder, which exists only in the
    // separate-artwork-folder layout.  Without one, all of these are no-ops and sets are read-only.

    /// \brief Whether set records can be stored at all, i.e. whether a folder is configured.
    /// \details Asked live, so that changing the setting takes effect at once.
    ELCH_NODISCARD virtual bool movieSetRecordsEnabled() const = 0;
    /// \brief The names of all sets that have a record, read from the records themselves.
    /// \details Empty when records are not enabled.  This is how a set with no member movies is found.
    ELCH_NODISCARD virtual QStringList movieSetsWithRecord() = 0;
    /// \brief Reads \p set's record into it.  Returns whether one was found.
    /// \details Leaves the set unchanged and returns false when there is no record.
    virtual bool loadMovieSet(MovieSet& set) = 0;
    /// \brief Writes \p set's record.  Returns whether it was written.
    virtual bool saveMovieSet(MovieSet& set) = 0;
    /// \brief Deletes the record of the set called \p setName.  Returns whether it is gone.
    /// \details Removes the record only, never the folder or the artwork in it.
    virtual bool removeMovieSetRecord(const QString& setName) = 0;
    /// \brief How much of a set's on-disk state a rename managed to move.
    /// \details Three answers, because the separate-folder layout renames the directory and then
    ///          the files inside it, so the record can move while some artwork does not.
    enum class MovieSetFileMove
    {
        /// Everything moved, or there was nothing to move.
        Moved,
        /// Nothing moved.  Everything is still where it was, under the old name.
        NotMoved,
        /// Some moved and some did not; neither name has the whole set.
        PartlyMoved
    };

    /// \brief Moves the set's `set.nfo` and artwork from \p oldName's place to \p newName's.
    /// \details Answers Moved when there was nothing to move.  A partial or failed move is not
    ///          undone; the caller has to report it.
    /// \warning Call before the members are reassigned: in the artwork-next-to-movies layout the
    ///          paths are found through a member whose set name is still the old one.
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
