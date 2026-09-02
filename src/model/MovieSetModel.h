#pragma once

#include "data/movie/MovieSet.h"
#include "data/movie/MovieSetInfo.h"
#include "globals/Globals.h"
#include "media_center/KodiVersion.h"
#include "utils/Meta.h"

#include <QHash>
#include <QModelIndex>
#include <QObject>
#include <QString>
#include <QVector>

class MediaCenterInterface;
class Movie;
class MovieModel;

/// \brief The library's movie sets, owning the MovieSet entities.
/// \details Groups the movie model's movies by set name, keeps the grouping current as
///          movies change, and is the only writer of a movie's set membership (see assign()).
///          Movies are followed individually: Movie::sigChanged for edits, the movie model's
///          rowsAboutToBeRemoved for movies leaving the library and QObject::destroyed as a
///          backstop.  Writes made while a movie's signals are blocked are reconciled by a
///          direct call to syncMovie().
///
/// \see docs/concepts/movie-sets.md
class MovieSetModel : public QObject
{
    Q_OBJECT

public:
    explicit MovieSetModel(QObject* parent = nullptr);

    /// \brief Sets the movie model whose movies this model groups into sets, and loads them.
    void setMovieModel(MovieModel* movieModel);

    /// \brief Sets the media center through which sets' records (`set.nfo`) are read and written.
    /// \details Null means no records: every set is then exactly the movies that name it.
    void setRecordSource(MediaCenterInterface* mediaCenter);

    /// \brief Stops following the library; idempotent.  Called from Manager::~Manager().
    /// \details The model outlives Settings, so a movie destroyed during teardown must not
    ///          reach recordsAreConfigured() from here.
    void detachFromLibrary();

    /// \brief All sets, in the order they were first seen.  Owned by this model.
    /// \details A set is not dropped for merely becoming empty; see dropEmptySets().
    ELCH_NODISCARD const QVector<MovieSet*>& sets() const;
    /// \brief The set called \p name, or nullptr if there is none.  An empty name is no set.
    ELCH_NODISCARD MovieSet* set(const QString& name) const;
    /// \brief The set called \p name, created if it does not exist yet; nullptr for an empty name.
    MovieSet* addSet(const QString& name);
    /// \brief Renames \p movieSet's match key, keeping the object and everything else on it.
    /// \details The only way the key can move: MovieSet::setName() is private to this model,
    ///          because m_sets is keyed by the name and two sets sharing one cannot be told
    ///          apart afterwards.  Renaming a set to the name it already has does nothing, and
    ///          a real rename clears the display title, the two names being one again.
    ///          Only the object is renamed; moving the members' `<set><name>` and the set's
    ///          files is the caller's half of the rename.
    /// \return Whether the set is called \p newName now.  False means \p newName is empty, is
    ///         already taken -- that is a merge, which this refuses -- or that \p movieSet is
    ///         not one of this model's sets; nothing is changed then.
    ELCH_NODISCARD bool renameSet(MovieSet* movieSet, const QString& newName);
    /// \brief Puts \p movie into the set that \p set names, out of the one it is in.
    /// \details The only entry point for a membership edit: writes the value onto the movie and
    ///          marks it changed, but only if the whole MovieSetInfo differs (operator==).  An
    ///          empty name takes the movie out of every set.  For a movie this model has not
    ///          attached, only the value is written.
    void assign(Movie* movie, const MovieSetInfo& set);
    /// \brief Reconciles \p movie's membership with the set value it carries now.
    /// \details Needed for the two writes that reach a library movie under a QSignalBlocker,
    ///          the NFO re-read and the scrape merge; MovieController::syncSetMembership() calls
    ///          it at both sites.  Does nothing for a movie this model has not attached.
    void syncMovie(Movie* movie);

    /// \brief Removes the set called \p name, its record and its movies' membership.
    /// \details The record is removed first; if the media center refuses, nothing is changed.
    ///          Former members whose set value was non-empty are marked changed via assign().
    /// \return Whether the set is gone.  False means the set, its members and its file are
    ///         untouched.  Removing a set that does not exist returns true.
    ELCH_NODISCARD bool removeSet(const QString& name);

    /// \brief Whether sets can have a record at all, i.e. whether a folder is configured.
    /// \details Asked live, so that changing the setting takes effect at once.  False with no
    ///          media center.
    ELCH_NODISCARD bool recordsAreConfigured() const;

    /// \brief What renaming a set does, once the settings have been read.
    enum class RenameMode
    {
        /// Move `set.nfo`'s `<title>` and nothing else.
        SetFileOnly,
        /// Move every member's `<set><name>` and the record's `<originaltitle>` too.
        AllMovieFiles,
        /// Set-file-only was asked for and there is no `set.nfo` to rename.
        Unavailable
    };

    /// \brief Resolves the rename mode from its three inputs.
    /// \details Automatic picks SetFileOnly only if Kodi reads `set.nfo` (v22+) and records are
    ///          configured; it never answers Unavailable.  An explicit SetFileOnly without
    ///          records is Unavailable rather than silently downgraded.
    ELCH_NODISCARD static RenameMode resolveRenameMode(
        MovieSetRenameMode setting, mediaelch::KodiVersion kodiVersion, bool recordsAreConfigured);

    /// \brief resolveRenameMode() for this library, read live from the settings.
    ELCH_NODISCARD RenameMode renameMode() const;

    /// \brief Regroups every movie of the movie model into sets.
    /// \details Existing MovieSet objects are kept.  Also re-asks the media center which sets
    ///          have a record, creates sets for records that no movie names, and drops sets
    ///          with neither members nor a record.  Only the existence of a record is
    ///          refreshed; its contents are read when the set is created or first gains one.
    void reload();
    /// \brief Removes every set.
    void clear();

private slots:
    void onMovieChanged(Movie* movie);
    void onMovieDestroyed(QObject* movie);
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
    /// \brief Fills a record-less set's empty overview and id from its members' NFOs.
    /// \details Called for every membership addition.  Fills only what is missing, walks the
    ///          members in order and takes the first non-empty value of each; never writes.
    ///          The whole walk rather than just the movie that arrived -- a member already present
    ///          may not have been readable when it joined -- at an O(N^2) cost accepted knowingly.
    void seedFromMembers(MovieSet* movieSet);
    /// \brief Drops every set that has no members left and no record of its own.
    void dropEmptySets();
    /// \brief Whether \p movieSet has a `set.nfo` and records are configured.
    /// \details The record flag is refreshed by reload() while records are configured and left
    ///          alone otherwise, so switching the folder back on restores the answer at once.
    ELCH_NODISCARD bool isBacked(const MovieSet* movieSet) const;
    /// \brief Logs a warning if dropping \p movieSet would discard an unsaved record.
    void warnIfRecordIsLost(const MovieSet* movieSet) const;

private:
    QVector<MovieSet*> m_sets;
    /// \brief The set name each attached movie was last seen with, to spot a change.
    /// \details Keyed by QObject* so that a destroyed movie can be looked up without
    ///          its pointer ever being cast back to Movie*.
    QHash<QObject*, QString> m_setNameByMovie;
    /// \brief The sets each movie is a member of; a movie in no set has no entry.
    /// \details Maintained from MovieSet::sigMovieAdded/sigMovieRemoved so that it also covers
    ///          memberships made through the public MovieSet::addMovie().
    QHash<QObject*, QVector<MovieSet*>> m_setsByMovie;
    MovieModel* m_movieModel = nullptr;
    /// \brief Where sets' records are read from and written to; null means no records.
    MediaCenterInterface* m_mediaCenter = nullptr;
};
