#pragma once

#include "data/TmdbId.h"
#include "utils/Meta.h"

#include <QObject>
#include <QString>
#include <QVector>

class Movie;

/// \brief A movie collection (aka. set) as an object of its own.
/// \details A set is the movies whose NFO names it, plus optionally a record of its own in
///          `set.nfo`.  Overview and TMDB id are mirrored into every member NFO; artwork lives
///          as image files in the set's folder.  A set that has a record outlives its last
///          member.  Not to be confused with MovieSetInfo, the value a single Movie carries
///          about the set it belongs to.
class MovieSet : public QObject
{
    Q_OBJECT

public:
    /// \param name The set's name, which identifies it.
    explicit MovieSet(QString name, QObject* parent = nullptr);
    ~MovieSet() override = default;

    /// \brief The set's match key: identical to every member's `<set><name>`, to `set.nfo`'s
    ///        `<originaltitle>` and to the folder name.  Not necessarily what the user sees;
    ///        show displayName().
    ELCH_NODISCARD QString name() const;
    /// \brief The set's display title, or empty if it is the same as name().  Prefer displayName().
    ELCH_NODISCARD QString title() const;
    /// \brief What to show the user: title() where there is one, otherwise name().
    ELCH_NODISCARD QString displayName() const;
    ELCH_NODISCARD TmdbId tmdbId() const;
    ELCH_NODISCARD QString overview() const;
    /// \brief The set's member movies.  Not owned.
    ELCH_NODISCARD const QVector<Movie*>& movies() const;

    /// \brief Sets the match key.  Clears title(), since an all-movie-files rename re-unifies the two.
    /// \details Like the other scalar setters, assigning the current value does nothing at all.
    /// \warning Does not check that no other set is called \p name; callers must ask
    ///          MovieSetModel::set() first and treat a hit as a merge.
    void setName(QString name);
    /// \brief Sets the display title, the half a set-file-only rename moves.
    /// \details Empty means "the same as name()".  A display title lives only in the set's
    ///          record, so a set without one cannot have it.
    /// \warning Does not check that no other set shows the same title; SetsWidget::onSetNameChanged() does.
    void setTitle(QString title);
    void setTmdbId(TmdbId id);
    void setOverview(QString overview);

    /// \brief Adds \p movie to this set's members.  Does nothing if it is already one.
    void addMovie(Movie* movie);
    /// \brief Removes \p movie from this set's members.  Does nothing if it is not one.
    void removeMovie(Movie* movie);
    /// \brief Removes every member.  Does nothing if there are none.
    void clearMovies();
    /// \brief Removes the member that \p movie was, without dereferencing it.
    /// \details Called from this set's own QObject::destroyed handler; public so that
    ///          MovieSetModel can call it too, whichever handler runs first.  Idempotent.
    void forgetDestroyedMovie(QObject* movie);

    /// \brief Whether this set's own record differs from what is stored on disk.
    ELCH_NODISCARD bool hasChanged() const;
    void setChanged(bool changed);

    /// \brief Whether a `set.nfo` for this set exists on disk.
    /// \details Written only by the code that looked: MovieSetModel on creation and reload, and
    ///          the record writer.  Only meaningful while records are configured, so read it
    ///          through MovieSetModel.
    ELCH_NODISCARD bool hasRecord() const;
    void setHasRecord(bool hasRecord);

signals:
    /// \brief Emitted for every movie that becomes a member of this set.
    /// \details Per movie, so that MovieSetModel can maintain its membership index even for a
    ///          membership made through the public addMovie().
    void sigMovieAdded(MovieSet* set, Movie* movie);
    /// \brief Emitted for every movie that stops being a member of this set.
    /// \details A QObject*, because it is also emitted for a destroyed member; compare only.
    void sigMovieRemoved(MovieSet* set, QObject* movie);

    /// \brief Emitted whenever anything about this set changed, including its membership.
    /// \warning A membership change marks nothing dirty, neither this set nor the movie.
    ///          An edit that has to reach the member's NFO goes through MovieSetModel::assign().
    void sigChanged(MovieSet* set);

private slots:
    /// \brief Drops a member that has been destroyed; QObject::destroyed is the only notice a set gets.
    void onMovieDestroyed(QObject* movie);

private:
    QString m_name;
    /// \brief The display title, or empty when it is the same as m_name; see title().
    QString m_title;
    TmdbId m_tmdbId{TmdbId::NoId};
    QString m_overview;
    /// \brief Member movies.  Not owned; owned by MovieModel.
    QVector<Movie*> m_movies;
    bool m_hasChanged = false;
    /// \brief Whether a `set.nfo` for this set exists on disk; see hasRecord().
    bool m_hasRecord = false;
};

Q_DECLARE_METATYPE(MovieSet*)
