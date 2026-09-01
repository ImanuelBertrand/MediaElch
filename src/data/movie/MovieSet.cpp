#include "data/movie/MovieSet.h"

// For the upcast in onMovieDestroyed(); a forward declaration is not enough for it.
#include "data/movie/Movie.h"

#include <algorithm>
#include <utility>

MovieSet::MovieSet(QString name, QObject* parent) : QObject(parent), m_name{std::move(name)}, m_images{*this}
{
}

QString MovieSet::name() const
{
    return m_name;
}

TmdbId MovieSet::tmdbId() const
{
    return m_tmdbId;
}

QString MovieSet::overview() const
{
    return m_overview;
}

const QVector<Movie*>& MovieSet::movies() const
{
    return m_movies;
}

MovieSetImages& MovieSet::images()
{
    return m_images;
}

const MovieSetImages& MovieSet::constImages() const
{
    return m_images;
}

// The three setters below are the fan-out point of D-A: an edit to the set's own
// record means one `set.nfo` write plus a mirrored copy into every member movie's
// NFO (and, for setName(), the rename mode of D-B decides whether the members'
// `<set><name>` join key moves at all).  A setter does none of that itself: it
// updates this object, marks it as needing to be saved and announces it.  The
// `set.nfo` write happens when the set is saved -- KodiXml::saveMovieSet(), which
// is also what clears the flag again -- and the mirror into the members' NFOs when
// those movies are saved.  What keeps a set standing while it has no members is
// hasRecord(), not this flag; see MovieSetModel::dropEmptySets().

void MovieSet::setName(QString name)
{
    if (m_name == name) {
        return;
    }
    m_name = std::move(name);
    setChanged(true);
}

void MovieSet::setTmdbId(TmdbId id)
{
    if (m_tmdbId == id) {
        return;
    }
    m_tmdbId = id;
    setChanged(true);
}

void MovieSet::setOverview(QString overview)
{
    if (m_overview == overview) {
        return;
    }
    m_overview = std::move(overview);
    setChanged(true);
}

void MovieSet::addMovie(Movie* movie)
{
    if (movie == nullptr || m_movies.contains(movie)) {
        return;
    }
    m_movies.append(movie);
    // The set does not own its members and has to survive their death; QObject::destroyed
    // is the only notification it gets.  The connection is deliberately never taken down
    // again -- removeMovie() leaves it in place, because a later addMovie() of the same
    // movie would only have to make it again, and the handler is a no-op for a movie that
    // is not a member.
    connect(movie, &QObject::destroyed, this, &MovieSet::onMovieDestroyed, Qt::UniqueConnection);
    // Membership is stored in the member movies' NFOs, not in `set.nfo` (D-A), so
    // it does not make this set's own record dirty.  Writing it back to the movie is
    // deliberately not done here: a movie's MovieSetInfo is the value its own file
    // carries, and writing it from here as well is the two-writer problem the split
    // exists to remove.  A caller that means an edit does both halves through
    // MovieSetModel::assign(); a caller that is only following the library, as the
    // model itself is in attachMovie() and reload(), deliberately does neither.
    emit sigMovieAdded(this, movie);
    emit sigChanged(this);
}

void MovieSet::removeMovie(Movie* movie)
{
    if (m_movies.removeAll(movie) == 0) {
        return;
    }
    emit sigMovieRemoved(this, movie);
    emit sigChanged(this);
}

void MovieSet::clearMovies()
{
    if (m_movies.isEmpty()) {
        return;
    }
    // Taken first: a handler of sigMovieRemoved may look at movies() and has to see
    // the membership this call leaves behind, not the one it is undoing.
    const QVector<Movie*> former = std::exchange(m_movies, {});
    for (Movie* movie : former) {
        emit sigMovieRemoved(this, movie);
    }
    emit sigChanged(this);
}

void MovieSet::onMovieDestroyed(QObject* movie)
{
    forgetDestroyedMovie(movie);
}

void MovieSet::forgetDestroyedMovie(QObject* movie)
{
    // The Movie sub-object is gone by now, so the pointer is only ever compared,
    // never dereferenced -- and never cast back to Movie* for that reason.
    const auto it = std::remove_if(m_movies.begin(), m_movies.end(), [movie](const Movie* member) {
        return static_cast<const QObject*>(member) == movie;
    });
    if (it == m_movies.end()) {
        return;
    }
    m_movies.erase(it, m_movies.end());
    emit sigMovieRemoved(this, movie);
    emit sigChanged(this);
}

bool MovieSet::hasChanged() const
{
    return m_hasChanged;
}

void MovieSet::setChanged(bool changed)
{
    m_hasChanged = changed;
    emit sigChanged(this);
}

bool MovieSet::hasRecord() const
{
    return m_hasRecord;
}

void MovieSet::setHasRecord(bool hasRecord)
{
    // Deliberately no sigChanged(): whether a `set.nfo` exists is not a property of the
    // set that anything displays, and announcing it would make a reload that merely
    // re-checked the disk look like an edit.
    m_hasRecord = hasRecord;
}
