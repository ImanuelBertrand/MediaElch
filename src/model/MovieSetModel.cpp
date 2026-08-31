#include "model/MovieSetModel.h"

#include "data/movie/Movie.h"
#include "model/MovieModel.h"
#include "utils/Meta.h"

MovieSetModel::MovieSetModel(QObject* parent) : QAbstractItemModel(parent)
{
}

int MovieSetModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        // Root has an invalid model index.
        return 0;
    }
    return qsizetype_to_int(m_sets.size());
}

int MovieSetModel::columnCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        // Root has an invalid model index.
        return 0;
    }
    return 1;
}

QVariant MovieSetModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_sets.size()) {
        return {};
    }
    MovieSet* movieSet = m_sets.at(index.row());

    switch (role) {
    case Qt::DisplayRole:
    case Roles::NameRole: return movieSet->name();
    case Roles::MovieCountRole: return qsizetype_to_int(movieSet->movies().size());
    case Roles::MovieSetPointerRole: return QVariant::fromValue(movieSet);
    default: return {};
    }
}

QModelIndex MovieSetModel::index(int row, int column, const QModelIndex& parent) const
{
    if (parent.isValid() || row < 0 || row >= m_sets.size() || column != 0) {
        return {};
    }
    return createIndex(row, column);
}

QModelIndex MovieSetModel::parent(const QModelIndex& child) const
{
    Q_UNUSED(child);
    return {};
}

void MovieSetModel::setMovieModel(MovieModel* movieModel)
{
    if (m_movieModel == movieModel) {
        return;
    }
    if (m_movieModel != nullptr) {
        m_movieModel->disconnect(this);
    }
    m_movieModel = movieModel;
    if (m_movieModel != nullptr) {
        connect(m_movieModel, &QAbstractItemModel::rowsInserted, this, &MovieSetModel::onMoviesInserted);
        connect(
            m_movieModel, &QAbstractItemModel::rowsAboutToBeRemoved, this, &MovieSetModel::onMoviesAboutToBeRemoved);
    }
    reload();
}

const QVector<MovieSet*>& MovieSetModel::sets() const
{
    return m_sets;
}

MovieSet* MovieSetModel::set(const QString& name) const
{
    if (name.isEmpty()) {
        return nullptr;
    }
    // A linear search rather than a name index: a library has few sets, and an index
    // would silently go stale on MovieSet::setName().
    for (MovieSet* movieSet : m_sets) {
        if (movieSet->name() == name) {
            return movieSet;
        }
    }
    return nullptr;
}

MovieSet* MovieSetModel::addSet(const QString& name)
{
    if (name.isEmpty()) {
        return nullptr;
    }
    MovieSet* existing = set(name);
    if (existing != nullptr) {
        return existing;
    }
    return createSet(name);
}

void MovieSetModel::removeSet(const QString& name)
{
    MovieSet* movieSet = set(name);
    if (movieSet == nullptr) {
        return;
    }

    // Detaching the members has to reach disk, and nothing else marks it: a membership
    // change dirties neither the set (membership is not in `set.nfo`, D-A) nor, by
    // itself, the movie.  Movie::setSet() does mark the movie changed; when it leaves
    // the public API in the next step, whatever replaces it has to keep doing so.
    // The members are copied because each setSet() removes one from the set.
    const QVector<Movie*> members = movieSet->movies();
    for (Movie* movie : members) {
        movie->setSet(MovieSetInfo{});
    }

    // The last member leaving has already dropped the set, so look it up again rather
    // than reusing a pointer that may have been deleted in the meantime.
    MovieSet* remaining = set(name);
    if (remaining != nullptr) {
        dropSet(remaining);
    }
}

void MovieSetModel::dropSet(MovieSet* movieSet)
{
    const int row = qsizetype_to_int(m_sets.indexOf(movieSet));
    if (row < 0) {
        return;
    }
    if (!m_inReset) {
        beginRemoveRows(QModelIndex(), row, row);
    }
    m_sets.removeAt(row);
    if (!m_inReset) {
        endRemoveRows();
    }
    delete movieSet;
}

void MovieSetModel::dropEmptySets()
{
    // Iterated backwards because dropSet() removes from m_sets.
    for (int row = qsizetype_to_int(m_sets.size()) - 1; row >= 0; --row) {
        MovieSet* movieSet = m_sets.at(row);
        if (movieSet->movies().isEmpty()) {
            dropSet(movieSet);
        }
    }
}

void MovieSetModel::reload()
{
    beginResetModel();
    m_inReset = true;

    for (MovieSet* movieSet : asConst(m_sets)) {
        movieSet->clearMovies();
    }
    m_setNameByMovie.clear();
    if (m_movieModel != nullptr) {
        for (Movie* movie : m_movieModel->movies()) {
            attachMovie(movie);
        }
    }
    // Drop the sets no movie names any more.  A set has no record of its own yet --
    // `set.nfo` is a later step -- so an empty one is not a set at all, which is also
    // what the three grouping sites this model replaces did.
    dropEmptySets();

    m_inReset = false;
    endResetModel();
}

void MovieSetModel::clear()
{
    m_setNameByMovie.clear();
    if (m_sets.isEmpty()) {
        return;
    }
    beginRemoveRows(QModelIndex(), 0, qsizetype_to_int(m_sets.size()) - 1);
    qDeleteAll(m_sets);
    m_sets.clear();
    endRemoveRows();
}

void MovieSetModel::onMovieChanged(Movie* movie)
{
    if (movie == nullptr) {
        return;
    }
    // Movie::sigChanged means "repaint me" and fires for every kind of edit, so most
    // of them are not membership changes at all.
    const QString newName = movie->set().name;
    const QString oldName = m_setNameByMovie.value(movie);
    if (newName == oldName) {
        return;
    }
    m_setNameByMovie.insert(movie, newName);

    MovieSet* oldSet = set(oldName);
    if (oldSet != nullptr) {
        oldSet->removeMovie(movie);
        // A set that lost its last movie has nothing left to exist by: until `set.nfo`
        // is written, a set *is* its members, which is what the three grouping sites
        // this model replaces assumed too.  It is also what keeps the model from
        // collecting a set per keystroke: the movie widget's set combo box is editable
        // and rewrites the movie's set name on every one of them.
        if (oldSet->movies().isEmpty()) {
            dropSet(oldSet);
        }
    }
    MovieSet* newSet = addSet(newName);
    if (newSet != nullptr) {
        newSet->addMovie(movie);
    }
}

void MovieSetModel::onMovieDestroyed(QObject* movie)
{
    // The sets drop the movie themselves; only this model's bookkeeping is left.
    // Without this the entry would outlive the movie and could be read again for an
    // unrelated movie allocated at the same address.
    m_setNameByMovie.remove(movie);
}

void MovieSetModel::onSetChanged(MovieSet* movieSet)
{
    if (m_inReset) {
        // A reset announces everything at once.
        return;
    }
    const int row = qsizetype_to_int(m_sets.indexOf(movieSet));
    if (row < 0) {
        return;
    }
    const QModelIndex changed = createIndex(row, 0);
    emit dataChanged(changed, changed);
}

void MovieSetModel::onMoviesInserted(const QModelIndex& parent, int first, int last)
{
    if (m_movieModel == nullptr || parent.isValid()) {
        return;
    }
    for (int row = first; row <= last; ++row) {
        attachMovie(m_movieModel->movie(row));
    }
}

void MovieSetModel::onMoviesAboutToBeRemoved(const QModelIndex& parent, int first, int last)
{
    if (m_movieModel == nullptr || parent.isValid()) {
        return;
    }
    // The movies are still in the movie model and still alive here, which is the whole
    // reason this is the *aboutTo* signal: MovieModel::clear() only calls deleteLater()
    // on them, so waiting for QObject::destroyed would leave every set holding pointers
    // to movies that have already left the library until the event loop next runs.
    for (int row = first; row <= last; ++row) {
        detachMovie(m_movieModel->movie(row));
    }
    // A set whose last member has left the library has nothing left to exist by: until
    // `set.nfo` is written a set *is* its members.  Keeping it would put a name in the
    // set combo box and the set filter that no movie answers to -- neither list could
    // do that before this model existed, because both were computed from the library
    // on every read.
    dropEmptySets();
}

void MovieSetModel::attachMovie(Movie* movie)
{
    if (movie == nullptr) {
        return;
    }
    connect(movie, &Movie::sigChanged, this, &MovieSetModel::onMovieChanged, Qt::UniqueConnection);
    connect(movie, &QObject::destroyed, this, &MovieSetModel::onMovieDestroyed, Qt::UniqueConnection);

    const QString name = movie->set().name;
    m_setNameByMovie.insert(movie, name);
    MovieSet* movieSet = addSet(name);
    if (movieSet != nullptr) {
        movieSet->addMovie(movie);
    }
}

void MovieSetModel::detachMovie(Movie* movie)
{
    if (movie == nullptr) {
        return;
    }
    movie->disconnect(this);
    m_setNameByMovie.remove(movie);
    // Every set, not just the one the movie names: reload() can leave a set holding a
    // member the movie itself does not name, and a pointer left behind here would
    // outlive the movie.
    for (MovieSet* movieSet : asConst(m_sets)) {
        movieSet->removeMovie(movie);
    }
}

MovieSet* MovieSetModel::createSet(const QString& name)
{
    auto* movieSet = new MovieSet(name, this);
    connect(movieSet, &MovieSet::sigChanged, this, &MovieSetModel::onSetChanged);

    const int row = qsizetype_to_int(m_sets.size());
    if (!m_inReset) {
        beginInsertRows(QModelIndex(), row, row);
    }
    m_sets.append(movieSet);
    if (!m_inReset) {
        endInsertRows();
    }
    return movieSet;
}
