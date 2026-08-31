#pragma once

#include "data/movie/MovieSet.h"
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
///            last seen with.  This is the interim arrangement while Movie::setSet is
///            still part of the public API; the next step makes this model the only
///            writer and the comparison goes away with it.
///          - rowsAboutToBeRemoved detaches a movie that leaves the library again.  It
///            is the only notification that arrives while the Movie is still alive and
///            still in the movie model, so it is where the sets let go of it.
///          - QObject::destroyed is the backstop for a movie that dies without leaving
///            the movie model.  MovieSet heals its own membership the same way.
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
    /// \brief Removes the set called \p name and detaches its movies.
    /// \details This is the deliberate removal, movies and all; nothing else destroys
    ///          a set that still has members.  Detaching a movie is an edit that has to
    ///          reach disk -- membership lives in the member movies' NFOs (D-A) -- and
    ///          neither MovieSet nor this model marks anything dirty for a membership
    ///          change on its own, so this marks the former members changed itself.
    void removeSet(const QString& name);

    /// \brief Regroups every movie of the movie model into sets.
    /// \details Existing MovieSet objects are kept, so a set's own record survives; a set
    ///          that ends up with no members is dropped, because until `set.nfo` exists a
    ///          set has no record apart from its movies.
    void reload();
    /// \brief Removes every set.
    void clear();

private slots:
    void onMovieChanged(Movie* movie);
    void onMovieDestroyed(QObject* movie);
    void onSetChanged(MovieSet* set);
    void onMoviesInserted(const QModelIndex& parent, int first, int last);
    void onMoviesAboutToBeRemoved(const QModelIndex& parent, int first, int last);

private:
    void attachMovie(Movie* movie);
    void detachMovie(Movie* movie);
    MovieSet* createSet(const QString& name);
    void dropSet(MovieSet* movieSet);
    /// \brief Drops every set that has no members left.
    void dropEmptySets();

private:
    QVector<MovieSet*> m_sets;
    /// \brief The set name each attached movie was last seen with, to spot a change.
    /// \details Keyed by QObject* so that a destroyed movie can be looked up without
    ///          its pointer ever being cast back to Movie*.
    QHash<QObject*, QString> m_setNameByMovie;
    MovieModel* m_movieModel = nullptr;
    /// \brief Whether reload() is running; it announces one reset instead of each change.
    bool m_inReset = false;
};
