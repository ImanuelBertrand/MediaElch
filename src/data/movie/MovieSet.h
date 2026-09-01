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
    /// \warning Nothing here checks that no other set is called \p name, and the
    ///          model cannot check afterwards either.  MovieSetModel::addSet() is
    ///          the only uniqueness guard, so a caller renaming a set has to ask
    ///          MovieSetModel::set() first and treat a hit as a merge.
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
    /// \brief Removes the member that \p movie was, without ever dereferencing it.
    /// \details This set heals itself on QObject::destroyed and calls this from there,
    ///          so members need not do anything.  It is public for a second observer of
    ///          the same signal -- MovieSetModel -- which cannot know whether its own
    ///          handler runs before or after this set's, because that depends on when
    ///          the set connected.  Calling it again is a no-op.
    void forgetDestroyedMovie(QObject* movie);

    /// \brief Whether this set's own record differs from what is stored on disk.
    ELCH_NODISCARD bool hasChanged() const;
    void setChanged(bool changed);

    /// \brief Whether a `set.nfo` for this set exists on disk.
    /// \details This is what makes a set more than the movies that name it: a set with
    ///          a record has an existence of its own and outlives its last member,
    ///          while a set without one is nothing but the grouping of its movies and
    ///          goes when they do -- see MovieSetModel::dropEmptySets().
    ///
    ///          It is a fact about the file system, not an opinion, and it is only ever
    ///          written by the code that looked: MovieSetModel sets it when a set is
    ///          created and when it reloads, from what the media center actually found,
    ///          and the writer sets it when a record has been written.  Nothing infers
    ///          it from hasChanged(), which is a one-way latch and cannot stand in for
    ///          it -- that substitution has been tried and reverted.
    ///
    ///          Read it through MovieSetModel, not directly: a record only counts while
    ///          records are configured at all, and the flag alone does not know that.
    ELCH_NODISCARD bool hasRecord() const;
    void setHasRecord(bool hasRecord);

signals:
    /// \brief Emitted for every movie that becomes a member of this set.
    /// \details Membership is announced per movie, and not only through sigChanged
    ///          below, because MovieSetModel keeps an index of which sets a movie is
    ///          in and an index cannot be maintained from a signal that does not say
    ///          what changed.  Emitting it here rather than having the model update
    ///          the index at its own call sites is what keeps the index right for a
    ///          membership the model did not make itself: addMovie() is public.
    void sigMovieAdded(MovieSet* set, Movie* movie);
    /// \brief Emitted for every movie that stops being a member of this set.
    /// \details Carries a QObject*, not a Movie*, because it is also emitted for a
    ///          member that has been destroyed, whose Movie sub-object is gone by
    ///          then; see forgetDestroyedMovie().  The pointer is for comparison only.
    void sigMovieRemoved(MovieSet* set, QObject* movie);

    /// \brief Emitted whenever anything about this set changed, including its
    ///        membership.
    /// \warning A membership change deliberately marks *nothing* dirty -- not
    ///          this set (membership is not part of set.nfo, D-A) and not the
    ///          movie, whose MovieSetInfo is the value its own file carries and is
    ///          not written from here.  So a caller that means an *edit* -- one that
    ///          has to reach the member's NFO -- must mark the member movies changed
    ///          itself, and MovieSetModel::assign() is the entry point that does.
    ///          Most callers of addMovie()/removeMovie() are not edits at all: the
    ///          model also calls them from attachMovie(), detachMovie(), reload() and
    ///          onMovieChanged(), where it is following the library rather than
    ///          changing it, and where dirtying a movie would be wrong.  Get that
    ///          distinction backwards and either a membership edit is lost with no flag
    ///          set anywhere, or every library reload offers to rewrite every NFO.
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
    /// \brief Whether a `set.nfo` for this set exists on disk; see hasRecord().
    bool m_hasRecord = false;
};

Q_DECLARE_METATYPE(MovieSet*)
