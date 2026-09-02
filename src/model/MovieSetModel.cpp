#include "model/MovieSetModel.h"

#include "data/movie/Movie.h"
#include "globals/Manager.h"
#include "log/Log.h"
#include "media_center/MediaCenterInterface.h"
#include "model/MovieModel.h"
#include "settings/KodiSettings.h"
#include "settings/Settings.h"
#include "utils/Meta.h"

#include <QSet>

MovieSetModel::MovieSetModel(QObject* parent) : QObject(parent)
{
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
        // Which sets have a record has changed for all of them, and sets that have a record
        // but no member movie can only be found now.
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
    // Linear search: a library has few sets, and a name index would go stale on MovieSet::setName().
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
        // Written and dirtied only if the value differs, so that an NFO the user never touched is
        // not offered for rewriting.
        movie->setSetInfo(setInfo);
    }
    // Outside the guard: the model can be behind the movie when the caller blocked its signals,
    // see syncMovie().  Reconciling twice is a no-op.
    syncMovie(movie);
}

void MovieSetModel::syncMovie(Movie* movie)
{
    if (movie == nullptr || !m_setNameByMovie.contains(movie)) {
        // Not in the library, so there is no membership to reconcile; attachMovie() picks up
        // the movie's set if it ever joins.
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

    // The record goes first and a refusal is honoured: a `set.nfo` that outlived its set would
    // bring it back on the next reload, and detaching the members first would leave them dirty
    // with the set still standing.  This is the only place in the model that removes a file.
    if (isBacked(movieSet) && !m_mediaCenter->removeMovieSetRecord(movieSet->name())) {
        qCWarning(generic) << "[MovieSetModel] Not removing movie set" << movieSet->name()
                           << "-- its record could not be removed, and a set whose record outlives it"
                           << "comes back on the next reload.";
        return false;
    }

    // Membership lives in the members' NFOs, so assign() marks each former member changed.
    // Copied because each assign() removes one from the set.
    const QVector<Movie*> members = movieSet->movies();
    for (Movie* movie : members) {
        assign(movie, MovieSetInfo{});
    }

    dropSet(movieSet);
    return true;
}

void MovieSetModel::dropSet(MovieSet* movieSet)
{
    const qsizetype row = m_sets.indexOf(movieSet);
    if (row < 0) {
        return;
    }
    warnIfRecordIsLost(movieSet);
    m_sets.removeAt(row);
    // Take the deleted set out of the membership index so that no dangling MovieSet* is handed out.
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
        // A set with a `set.nfo` outlives its last member; one without is nothing but the grouping
        // of its movies.  Deliberately not approximated by hasChanged(), which would exempt every
        // set with an unsaved rename for the rest of the session.
        if (movieSet->movies().isEmpty() && !isBacked(movieSet)) {
            dropSet(movieSet);
        }
    }
}

void MovieSetModel::detachFromLibrary()
{
    // Record source first: every path below ends in recordsAreConfigured(), which reaches Settings.
    m_mediaCenter = nullptr;

    if (m_movieModel != nullptr) {
        m_movieModel->disconnect(this);
        m_movieModel = nullptr;
    }
    for (auto it = m_setNameByMovie.cbegin(), end = m_setNameByMovie.cend(); it != end; ++it) {
        it.key()->disconnect(this);
    }
    // Destroyed rather than emptied, so that the sets' own destroyed() connections go with them.
    clear();
}

bool MovieSetModel::recordsAreConfigured() const
{
    return m_mediaCenter != nullptr && m_mediaCenter->movieSetRecordsEnabled();
}

MovieSetModel::RenameMode MovieSetModel::resolveRenameMode(
    MovieSetRenameMode setting, mediaelch::KodiVersion kodiVersion, bool recordsAreConfigured)
{
    switch (setting) {
    case MovieSetRenameMode::SetFileOnly:
        // Explicit, so it is refused rather than downgraded to the all-movie-files rename the
        // user chose this setting to avoid.
        return recordsAreConfigured ? RenameMode::SetFileOnly : RenameMode::Unavailable;

    case MovieSetRenameMode::AllMovieFiles: return RenameMode::AllMovieFiles;

    case MovieSetRenameMode::Automatic: break;
    }

    // Kodi 22 is the first release that reads `set.nfo`; earlier versions would not see a
    // set-file-only rename at all.
    const bool kodiReadsSetFiles = kodiVersion.toInt() >= mediaelch::KodiVersion::v22;
    return kodiReadsSetFiles && recordsAreConfigured ? RenameMode::SetFileOnly : RenameMode::AllMovieFiles;
}

MovieSetModel::RenameMode MovieSetModel::renameMode() const
{
    return resolveRenameMode(Settings::instance()->movieSetRenameMode(),
        Manager::instance()->kodiSettings()->kodiVersion(),
        recordsAreConfigured());
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
    qCWarning(generic) << "[MovieSetModel] Discarding unsaved changes to movie set" << movieSet->name();
}

void MovieSetModel::reload()
{
    for (MovieSet* movieSet : asConst(m_sets)) {
        movieSet->clearMovies();
    }
    m_setNameByMovie.clear();
    m_setsByMovie.clear();

    // Which sets have a record is asked once for the whole library.  Only the existence of a
    // record is refreshed; its contents were read when the set was created.  The exception is
    // a set that gains a record: marked as backed while still holding the empty overview it
    // was created with, it would write that emptiness over the file on the next save.
    //
    // Skipped when records are off, so that the flags survive and turning the folder back on
    // restores every set's answer at once.
    QStringList recordNames;
    if (recordsAreConfigured()) {
        recordNames = m_mediaCenter->movieSetsWithRecord();
        const QSet<QString> setsWithRecord(recordNames.cbegin(), recordNames.cend());
        for (MovieSet* movieSet : asConst(m_sets)) {
            bool hasRecord = setsWithRecord.contains(movieSet->name());
            if (hasRecord && !movieSet->hasRecord()) {
                // A failed read means an I/O error or a change underneath us; the set must not
                // claim the record then.  A memberless set is dropped in that case and comes
                // back on the next reload.
                hasRecord = m_mediaCenter->loadMovieSet(*movieSet);
            }
            // A set whose record has gone keeps the overview and id it read from it, which is
            // recoverable; silently emptying the object would not be.
            movieSet->setHasRecord(hasRecord);
        }
    }

    if (m_movieModel != nullptr) {
        for (Movie* movie : m_movieModel->movies()) {
            attachMovie(movie);
        }
    }
    // Sets that have a record but no member movie can only be found from the records.
    // addSet() returns existing sets untouched.
    for (const QString& name : recordNames) {
        addSet(name);
    }

    // Drop the sets that no movie names any more and that have no record to exist by.
    dropEmptySets();
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
    qDeleteAll(m_sets);
    m_sets.clear();
}

void MovieSetModel::onMovieChanged(Movie* movie)
{
    if (movie == nullptr) {
        return;
    }
    // sigChanged fires for every kind of edit; only a set-name change matters here.
    const QString newName = movie->set().name;
    const QString oldName = m_setNameByMovie.value(movie);
    if (newName == oldName) {
        return;
    }
    m_setNameByMovie.insert(movie, newName);

    MovieSet* oldSet = set(oldName);
    if (oldSet != nullptr) {
        // Deliberately no drop when this empties the set: an edit never destroys a set, see
        // dropEmptySets().
        oldSet->removeMovie(movie);
    }
    MovieSet* newSet = addSet(newName);
    if (newSet != nullptr) {
        newSet->addMovie(movie);
    }
}

void MovieSetModel::onMovieDestroyed(QObject* movie)
{
    // The entry must not outlive the movie: its address may be reused.
    m_setNameByMovie.remove(movie);

    // Normally onMoviesAboutToBeRemoved() has detached the movie already; this is the backstop
    // for a movie that died some other way.  Taken, not read, so that the entry cannot outlive
    // the movie either.
    const QVector<MovieSet*> memberships = m_setsByMovie.take(movie);
    for (MovieSet* movieSet : memberships) {
        movieSet->forgetDestroyedMovie(movie);
    }
    dropEmptySets();
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
    // Seeded on the set's signal rather than at this model's call sites, so that a
    // MovieSet::addMovie() made from outside the model is covered too.
    seedFromMembers(movieSet);
}

void MovieSetModel::seedFromMembers(MovieSet* movieSet)
{
    if (movieSet == nullptr) {
        return;
    }
    // Never over a record: a set that has read a `set.nfo` holds the authoritative values.
    // hasRecord() and deliberately not isBacked(): switching the folder off does not make those
    // values any less the record's, and reload() does not re-read the file when it comes back.
    if (movieSet->hasRecord()) {
        return;
    }
    // Only what is missing is filled, so a value an earlier member or the user supplied is never
    // overwritten.  A cleared overview counts as missing and is refilled by the next member to join.
    const bool overviewIsMissing = movieSet->overview().isEmpty();
    const bool idIsMissing = !movieSet->tmdbId().isValid();
    if (!overviewIsMissing && !idIsMissing) {
        return;
    }

    // First member with a non-empty value wins, decided independently for the overview and the
    // id.  Members can disagree, and first-wins is what Kodi does with the same input.
    QString overview;
    TmdbId tmdbId = TmdbId::NoId;
    for (const Movie* movie : movieSet->movies()) {
        const MovieSetInfo info = movie->set();
        // A member whose own set name points elsewhere (possible through the public
        // MovieSet::addMovie()) describes another collection and must not donate.  After a
        // rename, SetsWidget::onSetNameChanged() keeps the members' names matching.
        if (info.name != movieSet->name()) {
            continue;
        }
        if (overview.isEmpty()) {
            overview = info.overview;
        }
        if (!tmdbId.isValid()) {
            tmdbId = info.tmdbId;
        }
        if (!overview.isEmpty() && tmdbId.isValid()) {
            break;
        }
    }

    const bool seedsOverview = overviewIsMissing && !overview.isEmpty();
    const bool seedsId = idIsMissing && tmdbId.isValid();
    if (!seedsOverview && !seedsId) {
        return;
    }

    // Seeding is not an edit: restore the changed flag the setters set, but keep an unsaved
    // rename dirty.
    const bool wasChanged = movieSet->hasChanged();
    if (seedsOverview) {
        movieSet->setOverview(overview);
    }
    if (seedsId) {
        movieSet->setTmdbId(tmdbId);
    }
    movieSet->setChanged(wasChanged);
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
        // Most movies are in no set, so no entry rather than an empty one.
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
    // The movies are still alive and in the movie model here; MovieModel::clear() only calls
    // deleteLater() on them.
    for (int row = first; row <= last; ++row) {
        detachMovie(m_movieModel->movie(row));
    }
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
    // Every set the movie is actually in, not only the one it names: MovieSet::addMovie() is
    // public.  Taken, not read, so that the entry goes even if a set fails to empty it.
    const QVector<MovieSet*> memberships = m_setsByMovie.take(movie);
    for (MovieSet* movieSet : memberships) {
        movieSet->removeMovie(movie);
    }
}

MovieSet* MovieSetModel::createSet(const QString& name)
{
    auto* movieSet = new MovieSet(name, this);
    // Before the set is visible to anything else: dropEmptySets() would destroy a set whose
    // record has not been looked for yet.
    if (m_mediaCenter != nullptr) {
        movieSet->setHasRecord(m_mediaCenter->loadMovieSet(*movieSet));
    }
    // Fed from the set, so that memberships made through the public MovieSet::addMovie() are
    // indexed too.
    connect(movieSet, &MovieSet::sigMovieAdded, this, &MovieSetModel::onSetMovieAdded);
    connect(movieSet, &MovieSet::sigMovieRemoved, this, &MovieSetModel::onSetMovieRemoved);

    m_sets.append(movieSet);
    return movieSet;
}
