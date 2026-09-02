#pragma once

#include "data/movie/MovieSet.h"
#include "data/movie/MovieSetInfo.h"
#include "utils/Meta.h"

#include <QAbstractItemModel>
#include <QHash>
#include <QModelIndex>
#include <QString>
#include <QVector>

class Movie;
class MovieModel;

/// \brief The library's movie sets, owning the MovieSet entities.
/// \details Before this model existed, the set list was recomputed from scratch in
///          three unrelated places, each walking the whole movie model and grouping
///          it by `movie->set().name`: the sets tab, the set combo box of the movie
///          widget and the set filter.  This model does that grouping once and keeps
///          the result up to date, so that all three read one list.
///
///          It is kept current rather than recomputed because MovieModel offers no
///          reset signal to rebuild on (it has only beginInsertRows()/beginRemoveRows(),
///          and clear() calls deleteLater() on every movie).  So the model attaches to
///          each movie as it enters the library and follows it from there:
///
///          - Movie::sigChanged fires for every kind of edit, so a membership change is
///            found by comparing the movie's current set name against the one it was
///            last seen with.  This is permanent, not an interim arrangement: the
///            per-movie value stays writable, because the NFO reader and the scrape
///            merger serve a library movie and a transient one from the same line and
///            cannot tell which they have.  What this model owns is *membership*, and
///            assign() is the only entry point for an edit to it.  The model moves
///            movies between sets in other places too -- attachMovie(), detachMovie(),
///            reload() and onMovieChanged() all do -- but those are it following the
///            library rather than editing it, and none of them dirties a movie.
///          - The comparison is not sufficient on its own, because a caller can
///            suppress the signal it rides on.  Both writes that reach a library movie
///            that way are reconciled by a direct call to syncMovie().
///          - rowsAboutToBeRemoved detaches a movie that leaves the library again.  It
///            is the only notification that arrives while the Movie is still alive and
///            still in the movie model, so it is where the sets let go of it.
///          - QObject::destroyed is the backstop for a movie that dies without leaving
///            the movie model at all.  MovieSet heals its own membership the same way.
///
/// \see docs/concepts/movie-sets.md, D-C.
class MovieSetModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    enum Roles
    {
        NameRole = Qt::UserRole,
        MovieCountRole = Qt::UserRole + 1,
        MovieSetPointerRole = Qt::UserRole + 22
    };

public:
    explicit MovieSetModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex& child) const override;

    /// \brief Sets the movie model whose movies this model groups into sets.
    /// \details Loads the sets from it immediately and follows it from then on.
    void setMovieModel(MovieModel* movieModel);

    /// \brief All sets, in the order they were first seen.  Owned by this model.
    /// \details A set is never dropped for merely having no members -- an edit that
    ///          empties one leaves it standing, and one created by addSet() and never
    ///          filled is a set too.  Sets are dropped when the library is re-derived
    ///          and nothing is left to derive them from (reload(), or the movies
    ///          leaving the movie model) or when they are removed deliberately.
    ELCH_NODISCARD const QVector<MovieSet*>& sets() const;
    /// \brief The set called \p name, or nullptr if there is none.  An empty name is no set.
    ELCH_NODISCARD MovieSet* set(const QString& name) const;
    /// \brief The set called \p name, created if it does not exist yet.
    /// \return nullptr if \p name is empty, because a set is identified by its name (D-B).
    MovieSet* addSet(const QString& name);
    /// \brief Puts \p movie into the set that \p set names, out of the one it is in.
    /// \details This is the membership *edit*, and the only entry point for one: it
    ///          writes the value onto the movie, moves the movie between the MovieSet
    ///          entities and marks the movie changed, because a membership change
    ///          dirties neither the movie nor the set on its own and this edit has to
    ///          reach the member's NFO (D-A).
    ///
    ///          **Only when the value actually changes.**  Asked to put a movie where
    ///          it already is, this does nothing at all -- no write, no dirty flag, no
    ///          signal -- which is the promise MovieSet's own setters make, and which
    ///          keeps MediaElch from offering to rewrite an NFO the user never touched.
    ///          The comparison is MovieSetInfo::operator==, so it is the whole value
    ///          that has to match: a caller handing over a name-only MovieSetInfo for a
    ///          movie whose set carries an id or an overview *is* a change, and will
    ///          overwrite them.  Callers that mean "leave it alone if the name is the
    ///          same" have to say so themselves; MovieWidget::onSetChange() and
    ///          SetsWidget::onAddMovie() both do.
    ///
    ///          The reconcile happens either way, though, because the model can be
    ///          behind the movie for reasons that have nothing to do with this call;
    ///          see syncMovie().
    ///
    ///          An empty name takes the movie out of every set without putting it into
    ///          another one; an empty name is not a set (D-B).
    /// \note For a movie this model has never seen -- a scrape result, a freshly parsed
    ///       NFO -- there is no membership to change, and this writes the value and
    ///       nothing else.
    void assign(Movie* movie, const MovieSetInfo& set);
    /// \brief Reconciles \p movie's membership with the set value it carries now.
    /// \details The model normally follows a movie through Movie::sigChanged, which is
    ///          enough for every write that is not suppressed.  Two writes are, and
    ///          both can land on a movie that is already in the library:
    ///
    ///          - the NFO re-read, which MovieController::loadData(MediaCenterInterface*)
    ///            performs under a QSignalBlocker covering the whole load;
    ///          - the scrape merge, where copyDetailsToMovie() blocks the target for
    ///            the whole merge loop, reached from
    ///            MovieController::loadData(ids, locale, details).
    ///
    ///          Either leaves a library movie naming a set the model has not seen.
    ///          This is the direct call that survives both blockers -- a signal, even a
    ///          dedicated one, would not -- and MovieController::syncSetMembership()
    ///          makes it at both sites.  It is also the repair for a hole this model
    ///          itself opened by becoming the source of the set list.
    ///
    ///          Does nothing for a movie this model has not attached, and nothing for a
    ///          movie whose set name has not moved -- in particular it never re-creates
    ///          a set that removeSet() deliberately took away, because that call clears
    ///          the members' set names as it goes.
    void syncMovie(Movie* movie);

    /// \brief Removes the set called \p name and detaches its movies.
    /// \details This is the deliberate removal, movies and all; nothing else destroys
    ///          a set that still has members.  Detaching a movie is an edit that has to
    ///          reach disk -- membership lives in the member movies' NFOs (D-A) -- and
    ///          neither MovieSet nor this model marks anything dirty for a membership
    ///          change on its own, so this marks the former members changed itself --
    ///          through assign(), and so only those whose set value actually had
    ///          something in it to clear.  A member whose own value was already empty is
    ///          detached without being dirtied, because for that movie nothing about
    ///          the file on disk has changed.
    void removeSet(const QString& name);

    /// \brief Regroups every movie of the movie model into sets.
    /// \details Existing MovieSet objects are kept, so a set's own record survives; a set
    ///          that ends up with no members is dropped, because until `set.nfo` exists
    ///          the movies are all a set has.
    void reload();
    /// \brief Removes every set.
    void clear();

private slots:
    void onMovieChanged(Movie* movie);
    void onMovieDestroyed(QObject* movie);
    void onSetChanged(MovieSet* set);
    void onSetMovieAdded(MovieSet* set, Movie* movie);
    void onSetMovieRemoved(MovieSet* set, QObject* movie);
    void onMoviesInserted(const QModelIndex& parent, int first, int last);
    void onMoviesAboutToBeRemoved(const QModelIndex& parent, int first, int last);

private:
    void attachMovie(Movie* movie);
    void detachMovie(Movie* movie);
    MovieSet* createSet(const QString& name);
    void dropSet(MovieSet* movieSet);
    /// \brief Takes \p movie out of the membership index entry for \p movieSet.
    void unindexMembership(QObject* movie, MovieSet* movieSet);
    /// \brief Drops every set that has no members left.
    void dropEmptySets();
    /// \brief Logs a warning if dropping \p movieSet would discard an unsaved record.
    void warnIfRecordIsLost(const MovieSet* movieSet) const;

private:
    QVector<MovieSet*> m_sets;
    /// \brief The set name each attached movie was last seen with, to spot a change.
    /// \details Keyed by QObject* so that a destroyed movie can be looked up without
    ///          its pointer ever being cast back to Movie*.
    QHash<QObject*, QString> m_setNameByMovie;
    /// \brief The sets each movie is a member of.
    /// \details detachMovie() has to take a leaving movie out of every set that holds
    ///          it, and it used to find them by scanning all of them -- O(sets) per
    ///          movie, so O(movies x sets) across a library reload, the one moment when
    ///          both counts are large at once.  This answers the same question in one
    ///          lookup.
    ///
    ///          It is maintained from MovieSet::sigMovieAdded/sigMovieRemoved rather
    ///          than at this model's own call sites, so that it stays right for a
    ///          membership this model did not make: MovieSet::addMovie() is public.
    ///          Keyed by QObject* for the same reason as m_setNameByMovie, and a movie
    ///          in no set has no entry at all rather than an empty one -- most movies
    ///          are in no set.
    QHash<QObject*, QVector<MovieSet*>> m_setsByMovie;
    MovieModel* m_movieModel = nullptr;
    /// \brief Whether reload() is running; it announces one reset instead of each change.
    bool m_inReset = false;
};
