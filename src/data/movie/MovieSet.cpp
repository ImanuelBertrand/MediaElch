#include "data/movie/MovieSet.h"

// The upcast in forgetDestroyedMovie() needs the full type.
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

QString MovieSet::title() const
{
    return m_title;
}

QString MovieSet::displayName() const
{
    return m_title.isEmpty() ? m_name : m_title;
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

// A setter updates this object, marks it as needing to be saved and announces it.  The
// `set.nfo` write happens in KodiXml::saveMovieSet(), the mirror into the members' NFOs
// when those movies are saved.

void MovieSet::setName(QString name)
{
    if (m_name == name) {
        return;
    }
    m_name = std::move(name);
    // Before setChanged(true): it emits sigChanged synchronously and observers would otherwise
    // see the new key still carrying the old display title.
    m_title.clear();
    setChanged(true);
}

void MovieSet::setTitle(QString title)
{
    // "Display title equals key" has one representation, so that reader and writer agree.
    if (title == m_name) {
        title.clear();
    }
    if (m_title == title) {
        return;
    }
    m_title = std::move(title);
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
    // The connection is never taken down again; the handler is a no-op for a non-member.
    connect(movie, &QObject::destroyed, this, &MovieSet::onMovieDestroyed, Qt::UniqueConnection);
    // Membership dirties neither this set nor the movie; MovieSetModel::assign() does that
    // for an edit.
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
    // Taken first: a handler of sigMovieRemoved may look at movies().
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
    // The Movie sub-object is gone by now; the pointer is only compared, never dereferenced.
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
    // Deliberately no sigChanged(): a reload that merely re-checked the disk is not an edit.
    m_hasRecord = hasRecord;
}
