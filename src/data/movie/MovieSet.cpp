#include "data/movie/MovieSet.h"

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
// `<set><name>` join key moves at all).  None of that is implemented yet: there is
// no MovieSetModel and no `set.nfo` writer, and this entity is not wired to
// anything.  Until then a setter only updates this object and announces it.

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
    // Membership is stored in the member movies' NFOs, not in `set.nfo` (D-A), so
    // it does not make this set's own record dirty.  Writing it back to the movie
    // is deliberately not done here: the movie-side setter is removed in the later
    // "the model is the only writer" step, and doing it now would create the very
    // two-writer problem that step exists to remove.
    emit sigChanged(this);
}

void MovieSet::removeMovie(Movie* movie)
{
    if (m_movies.removeAll(movie) == 0) {
        return;
    }
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
