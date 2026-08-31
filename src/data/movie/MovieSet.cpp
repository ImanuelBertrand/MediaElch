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
// `<set><name>` join key moves at all).  The `set.nfo` writer is a later step, so
// none of that fan-out happens yet: a setter updates this object, marks it as
// needing to be saved and announces it, and MovieSetModel keeps the object rather
// than dropping it while that flag is set.

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
    // it does not make this set's own record dirty.  Writing it back to the movie
    // is deliberately not done here: the movie-side setter is removed in the later
    // "the model is the only writer" step, and doing it now would create the very
    // two-writer problem that step exists to remove.
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
