#pragma once

#include "data/TmdbId.h"
#include "data/movie/MovieSetImages.h"
#include "utils/Meta.h"

#include <QObject>
#include <QString>
#include <QVector>

class Movie;

/// \brief A movie collection (aka. set) as an object of its own.
/// \details A set is an aggregate over the movies whose NFO names it, plus the
///          authoritative record in `set.nfo` that holds its overview, its TMDB
///          collection id and its artwork.  See docs/concepts/movie-sets.md, D-C.
///
///          Not to be confused with MovieSetInfo, the three-field value that a
///          single Movie carries about the set it belongs to.
class MovieSet : public QObject
{
    Q_OBJECT

public:
    /// \param name The set's name; the primary key of a set (D-B).
    explicit MovieSet(QString name, QObject* parent = nullptr);
    ~MovieSet() override = default;

    ELCH_NODISCARD QString name() const;
    ELCH_NODISCARD TmdbId tmdbId() const;
    ELCH_NODISCARD QString overview() const;
    /// \brief The set's member movies.  Not owned.
    ELCH_NODISCARD const QVector<Movie*>& movies() const;
    ELCH_NODISCARD MovieSetImages& images();
    ELCH_NODISCARD const MovieSetImages& constImages() const;

    void setName(QString name);
    void setTmdbId(TmdbId id);
    void setOverview(QString overview);

    /// \brief Adds \p movie to this set's members.  Does nothing if it is already one.
    void addMovie(Movie* movie);
    /// \brief Removes \p movie from this set's members.  Does nothing if it is not one.
    void removeMovie(Movie* movie);

    /// \brief Whether this set's own record differs from what is stored on disk.
    ELCH_NODISCARD bool hasChanged() const;
    void setChanged(bool changed);

signals:
    void sigChanged(MovieSet* set);

private:
    QString m_name;
    TmdbId m_tmdbId{TmdbId::NoId};
    QString m_overview;
    /// \brief Member movies.  Not owned; owned by MovieModel.
    QVector<Movie*> m_movies;
    MovieSetImages m_images;
    bool m_hasChanged = false;
};

Q_DECLARE_METATYPE(MovieSet*)
