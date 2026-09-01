#include "model/MovieSetModel.h"

#include "data/movie/Movie.h"
#include "log/Log.h"
#include "media_center/MediaCenterInterface.h"
#include "model/MovieModel.h"
#include "utils/Meta.h"

#include <QSet>

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

void MovieSetModel::setRecordSource(MediaCenterInterface* mediaCenter)
{
    if (m_mediaCenter == mediaCenter) {
        return;
    }
    m_mediaCenter = mediaCenter;
    if (m_movieModel != nullptr) {
        // The sets have to be re-derived, because the answer to "which of you have a
        // record?" has just changed for all of them -- and because the sets that have
        // one but no member movie can only be found now.
        reload();
    }
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

void MovieSetModel::assign(Movie* movie, const MovieSetInfo& setInfo)
{
    if (movie == nullptr) {
        return;
    }
    if (movie->set() != setInfo) {
        // Marks the movie changed, which is the half of a membership edit that nothing
        // else does: neither MovieSet nor this model dirties anything for a membership
        // change on its own, and this one has to reach the member's NFO (D-A).
        //
        // Guarded, because putting a movie where it already is must change nothing at
        // all -- MovieSet's own setters promise exactly that, and a dirty flag nobody
        // asked for means MediaElch offers to rewrite an NFO the user never touched.
        movie->setSetInfo(setInfo);
    }
    // Outside the guard on purpose.  Writing the value is what has to be skipped when
    // nothing changed; reconciling is not, because the model can be behind the movie
    // for reasons that have nothing to do with this call -- see syncMovie().  Normally
    // it is redundant, since setSetInfo() emits Movie::sigChanged and onMovieChanged()
    // has already run; it is not redundant when the caller has blocked the movie's
    // signals.  Reconciling twice is a no-op.
    syncMovie(movie);
}

void MovieSetModel::syncMovie(Movie* movie)
{
    if (movie == nullptr || !m_setNameByMovie.contains(movie)) {
        // A movie this model never attached is not in the library, so it has no
        // membership here to reconcile.  Its set is picked up by attachMovie() if it
        // ever joins.
        return;
    }
    onMovieChanged(movie);
}

bool MovieSetModel::removeSet(const QString& name)
{
    MovieSet* movieSet = set(name);
    if (movieSet == nullptr) {
        // Nothing to remove, so the caller's postcondition already holds.
        return true;
    }

    // The record goes first, and the refusal is honoured.
    //
    // It has to be what makes the set exist apart from its movies, so a `set.nfo` that
    // outlived the set would be found by the next reload and bring the set back:
    // "Delete Movie Set" would delete nothing that lasted.  The media center can refuse
    // -- an unreadable record, a record that turns out to belong to another set, a
    // read-only mount, a file locked by something else -- and a refusal that was ignored
    // produced exactly that outcome through the door the guard opened.
    //
    // Attempted *before* the members are detached, and this ordering is the whole point.
    // Detaching is an edit that dirties every member, so doing it first and bailing out
    // afterwards would leave the members detached and dirty with the set still standing:
    // half-done, and worse than either clean outcome.  Failing here leaves everything
    // exactly as it was.
    //
    // This is the only place in this model that removes a file, and it is the only place
    // that may.  removeSet() is reached exclusively from a deliberate removal in the sets
    // tab; every automatic path goes through dropEmptySets(), which destroys objects and
    // never touches the disk.  Only the record is removed -- the folder and any artwork
    // in it stay, and a folder without a `set.nfo` is not a set, so it cannot resurrect
    // this one.
    if (isBacked(movieSet) && !m_mediaCenter->removeMovieSetRecord(movieSet->name())) {
        qCWarning(generic) << "[MovieSetModel] Not removing movie set" << movieSet->name()
                           << "-- its record could not be removed, and a set whose record outlives it"
                           << "comes back on the next reload.";
        return false;
    }

    // Detaching the members has to reach disk, and nothing else marks it: a membership
    // change dirties neither the set (membership is not in `set.nfo`, D-A) nor, by
    // itself, the movie.  assign() is what marks it.
    // The members are copied because each assign() removes one from the set.
    const QVector<Movie*> members = movieSet->movies();
    for (Movie* movie : members) {
        assign(movie, MovieSetInfo{});
    }

    dropSet(movieSet);
    return true;
}

void MovieSetModel::dropSet(MovieSet* movieSet)
{
    const int row = qsizetype_to_int(m_sets.indexOf(movieSet));
    if (row < 0) {
        return;
    }
    warnIfRecordIsLost(movieSet);
    if (!m_inReset) {
        beginRemoveRows(QModelIndex(), row, row);
    }
    m_sets.removeAt(row);
    if (!m_inReset) {
        endRemoveRows();
    }
    // Nothing else takes a deleted set out of the membership index, and an entry that
    // kept it would hand out a dangling MovieSet*.  Normally there is nothing to do:
    // a set is dropped either for having no members or after its members have been
    // detached.
    const QVector<Movie*> members = movieSet->movies();
    for (Movie* movie : members) {
        unindexMembership(movie, movieSet);
    }
    delete movieSet;
}

void MovieSetModel::dropEmptySets()
{
    // Iterated backwards because dropSet() removes from m_sets.
    for (int row = qsizetype_to_int(m_sets.size()) - 1; row >= 0; --row) {
        MovieSet* movieSet = m_sets.at(row);
        // No members left *and* no record of its own (D-A).  A set with a `set.nfo` is
        // more than the grouping of its movies: it has an overview, a collection id and
        // artwork that belong to the set and not to any movie, so it outlives its last
        // member.  A set without one is nothing but that grouping, and goes when the
        // grouping does -- otherwise its name would sit in the set combo box and the set
        // filter with no movie answering to it.
        //
        // The record is a fact about the file system, established when the set was
        // created and refreshed by reload().  It is deliberately *not* approximated by
        // hasChanged(), which has been tried and reverted: that flag was a one-way latch
        // until the `set.nfo` writer gave it a clearing edge, so exempting a changed set
        // exempted every set that had ever been renamed for the rest of the session --
        // immune even to reload(), the very thing meant to cure that.
        if (movieSet->movies().isEmpty() && !isBacked(movieSet)) {
            dropSet(movieSet);
        }
    }
}

bool MovieSetModel::recordsAreConfigured() const
{
    return m_mediaCenter != nullptr && m_mediaCenter->movieSetRecordsEnabled();
}

bool MovieSetModel::isBacked(const MovieSet* movieSet) const
{
    return recordsAreConfigured() && movieSet->hasRecord();
}

void MovieSetModel::warnIfRecordIsLost(const MovieSet* movieSet) const
{
    if (!movieSet->hasChanged()) {
        return;
    }
    // Every path that destroys a set object arrives here, through dropSet(): the
    // deliberate removeSet(), the automatic dropEmptySets() and clear().  The flag means
    // "this set differs from what is stored on disk", and since the `set.nfo` writer gave
    // it a clearing edge it means that literally, so this fires exactly when an edit the
    // user has not saved into the set's file is about to go.
    qCWarning(generic) << "[MovieSetModel] Discarding unsaved changes to movie set" << movieSet->name();
}

void MovieSetModel::reload()
{
    beginResetModel();
    m_inReset = true;

    for (MovieSet* movieSet : asConst(m_sets)) {
        movieSet->clearMovies();
    }
    m_setNameByMovie.clear();
    m_setsByMovie.clear();

    // Which sets have a record is asked once for the whole library, and every set here is
    // told whether it is among the answers.  It is not a cheap question: the media center
    // has to open and parse every `set.nfo` in the folder to find out which set each one
    // names, so this costs one parse per record -- plus a second one for each set that
    // has to be created from a record below, which goes through loadMovieSet() again.
    // It is bounded by the number of sets, not by the size of the library.
    //
    // Only the *existence* of a record is refreshed.  Its contents are read once, when
    // the set is created, because re-reading would overwrite an overview or an id the
    // user has edited and not saved yet.
    //
    // Skipped entirely when records are not configured, and that is not just an
    // optimisation: with no folder there is nothing to re-derive from, so clearing every
    // flag would throw away the last thing that was actually known.  isBacked() already
    // returns false for every set while records are off, so a preserved flag changes no
    // decision -- and it is what makes turning the folder back on restore every set's
    // answer immediately rather than at the next reload.
    QStringList recordNames;
    if (recordsAreConfigured()) {
        recordNames = m_mediaCenter->movieSetsWithRecord();
        const QSet<QString> setsWithRecord(recordNames.cbegin(), recordNames.cend());
        for (MovieSet* movieSet : asConst(m_sets)) {
            bool hasRecord = setsWithRecord.contains(movieSet->name());
            if (hasRecord && !movieSet->hasRecord()) {
                // A record this set did not have before -- the folder was configured or
                // changed, the file was put there by something else, or the set was
                // renamed onto one.  Its contents have to be read, and not only its
                // existence: a set marked as having a record while still holding the
                // empty overview it was created with would write that emptiness over the
                // file on the next save.
                //
                // And if that read fails -- the listing found the file a moment ago, so
                // this means an I/O error or a change underneath us -- the set does not
                // get to claim the record.  Claiming it on a failed read is the same
                // emptiness hazard by another route.
                hasRecord = m_mediaCenter->loadMovieSet(*movieSet);
            }
            // The other direction is deliberately left alone.  A set whose record has
            // gone keeps the overview and id it read from it, so saving the set writes
            // the file again with those contents.  That is the same thing that happens
            // to any other unsaved value in this application and it is recoverable;
            // silently emptying the object would not be.
            movieSet->setHasRecord(hasRecord);
        }
    }

    if (m_movieModel != nullptr) {
        for (Movie* movie : m_movieModel->movies()) {
            attachMovie(movie);
        }
    }
    // A set can have a record and no member movie at all -- one the user curated and
    // has not filled yet, or one whose last member left.  Nothing else finds it: sets
    // are otherwise only ever derived from the movies that name them, and Kodi does not
    // enumerate the folder either (it discovers sets from movies, exactly as this model
    // did).  addSet() creates the ones that are missing, reading each one's record as it
    // goes, and returns the ones that already exist untouched.
    for (const QString& name : recordNames) {
        addSet(name);
    }

    // Drop the sets that no movie names any more and that have no record to exist by.
    dropEmptySets();

    m_inReset = false;
    endResetModel();
}

void MovieSetModel::clear()
{
    m_setNameByMovie.clear();
    m_setsByMovie.clear();
    if (m_sets.isEmpty()) {
        return;
    }
    for (const MovieSet* movieSet : asConst(m_sets)) {
        warnIfRecordIsLost(movieSet);
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
        // Deliberately no drop when this empties the set.  An edit never destroys a
        // set: the set the user just emptied is very often the one they are about to
        // fill again, and a set that has a `set.nfo` outlives its last member in any
        // case (D-A).  Sets go when the library is re-derived and nothing is left to
        // derive them from -- see dropEmptySets().
        oldSet->removeMovie(movie);
    }
    MovieSet* newSet = addSet(newName);
    if (newSet != nullptr) {
        newSet->addMovie(movie);
    }
}

void MovieSetModel::onMovieDestroyed(QObject* movie)
{
    // Without this the entry would outlive the movie and could be read again for an
    // unrelated movie allocated at the same address.
    m_setNameByMovie.remove(movie);

    // A movie that leaves the library normally has been detached already, by
    // onMoviesAboutToBeRemoved(), which also disconnects this handler.  Reaching here
    // means the movie died some other way, so this is the backstop.  The sets heal
    // themselves on the same signal, but whether they have done so yet depends on when
    // each of them connected, so they are asked rather than assumed -- forgetting a
    // movie twice is a no-op.
    //
    // Taken, not read.  The sets normally empty this entry themselves, one
    // sigMovieRemoved at a time, so taking it changes nothing when they do -- but an
    // entry keyed by a movie that no longer exists is the hazard the line above
    // guards against, and this is the same hazard with a heavier consequence: a
    // MovieSet* read back for an unrelated movie at the same address.
    const QVector<MovieSet*> memberships = m_setsByMovie.take(movie);
    for (MovieSet* movieSet : memberships) {
        movieSet->forgetDestroyedMovie(movie);
    }
    dropEmptySets();
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

void MovieSetModel::onSetMovieAdded(MovieSet* movieSet, Movie* movie)
{
    if (movieSet == nullptr || movie == nullptr) {
        return;
    }
    QVector<MovieSet*>& memberships = m_setsByMovie[movie];
    if (!memberships.contains(movieSet)) {
        memberships.append(movieSet);
    }
}

void MovieSetModel::onSetMovieRemoved(MovieSet* movieSet, QObject* movie)
{
    unindexMembership(movie, movieSet);
}

void MovieSetModel::unindexMembership(QObject* movie, MovieSet* movieSet)
{
    const auto it = m_setsByMovie.find(movie);
    if (it == m_setsByMovie.end()) {
        return;
    }
    it->removeAll(movieSet);
    if (it->isEmpty()) {
        // A movie in no set has no entry: most of a library is in no set at all, and
        // an empty vector per movie would cost more than the index saves.
        m_setsByMovie.erase(it);
    }
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
    // A set whose last member has left the library has nothing left to exist by, unless
    // it has a `set.nfo` of its own.  Keeping one that does not would put a name in the
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
    // Every set the movie is actually in, which is not the same as the one it names:
    // MovieSet::addMovie() is public, so a set can hold a member whose own set().name
    // points elsewhere.  reload() cures that -- it rebuilds membership from the movies
    // -- but a movie can leave the library before one runs, and a pointer left behind
    // here would outlive the movie.
    //
    // The index answers that in one lookup.  Scanning every set instead was O(sets) per
    // movie, and a library reload detaches every movie, so it was O(movies x sets).
    // Taken, not read.  Each removeMovie() empties this entry itself, one
    // sigMovieRemoved at a time, so taking it changes nothing when they do; it is what
    // keeps a detached movie out of the index even if one of them does not.
    const QVector<MovieSet*> memberships = m_setsByMovie.take(movie);
    for (MovieSet* movieSet : memberships) {
        movieSet->removeMovie(movie);
    }
}

MovieSet* MovieSetModel::createSet(const QString& name)
{
    auto* movieSet = new MovieSet(name, this);
    // Asked at birth, and before the set is visible to anything else.  Nothing may ever
    // see a set whose record has not been looked for yet: dropEmptySets() would take a
    // set that has one for a set that has none and destroy it, records and all.
    //
    // It is one file read per set that is new to the model, which over a library scan is
    // one per distinct set name -- and none at all unless a movie set information folder
    // is configured, since then loadMovieSet() resolves no path and does no I/O.
    if (m_mediaCenter != nullptr) {
        movieSet->setHasRecord(m_mediaCenter->loadMovieSet(*movieSet));
    }
    connect(movieSet, &MovieSet::sigChanged, this, &MovieSetModel::onSetChanged);
    // The membership index is fed from the set, not from this model's own calls, so
    // that a membership made through the public MovieSet::addMovie() is indexed too.
    connect(movieSet, &MovieSet::sigMovieAdded, this, &MovieSetModel::onSetMovieAdded);
    connect(movieSet, &MovieSet::sigMovieRemoved, this, &MovieSetModel::onSetMovieRemoved);

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
