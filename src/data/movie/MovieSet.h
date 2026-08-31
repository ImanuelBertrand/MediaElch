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

    /// \brief Sets the set's name, its primary key (D-B).
    /// \details Assigning the value the set already has does nothing at all: it
    ///          neither dirties the set nor emits sigChanged.  That guarantee is
    ///          scoped to these three scalar setters; MovieSetImages::setImage()
    ///          and setChanged() below both fire unconditionally.
    void setName(QString name);
    void setTmdbId(TmdbId id);
    void setOverview(QString overview);

    /// \brief Adds \p movie to this set's members.  Does nothing if it is already one.
    /// \details A member that is destroyed removes itself again; see onMovieDestroyed().
    void addMovie(Movie* movie);
    /// \brief Removes \p movie from this set's members.  Does nothing if it is not one.
    void removeMovie(Movie* movie);
    /// \brief Removes every member.  Does nothing if there are none.
    void clearMovies();

    /// \brief Whether this set's own record differs from what is stored on disk.
    ELCH_NODISCARD bool hasChanged() const;
    void setChanged(bool changed);

signals:
    /// \brief Emitted whenever anything about this set changed, including its
    ///        membership.
    /// \warning A membership change deliberately marks *nothing* dirty -- not
    ///          this set (membership is not part of set.nfo, D-A) and not the
    ///          movie (the model becomes the only writer in a later step).  So
    ///          whoever calls addMovie()/removeMovie() -- MovieSetModel, once it
    ///          exists -- has to mark the member movies changed itself.  Forget
    ///          that and a membership edit is lost with no flag set anywhere,
    ///          while this signal has already claimed otherwise.
    void sigChanged(MovieSet* set);

private slots:
    /// \brief Drops a member that has been destroyed.
    /// \details A set holds its members without owning them, and nothing else tells
    ///          it when one dies: Movie::sigChanged is not emitted from ~Movie, and
    ///          MovieModel neither resets nor names what it removed -- clear() only
    ///          calls deleteLater() on every movie.  QObject::destroyed is therefore
    ///          the one notification a set can rely on, so membership heals itself
    ///          rather than being rebuilt from the outside.
    void onMovieDestroyed(QObject* movie);

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
