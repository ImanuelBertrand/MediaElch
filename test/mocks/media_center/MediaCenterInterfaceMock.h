#pragma once

#include "data/TmdbId.h"
#include "data/movie/MovieSet.h"
#include "media_center/MediaCenterInterface.h"

#include <QHash>
#include <QString>
#include <QStringList>

/// \brief A MediaCenterInterface that stores movie set records in memory.
/// \details MovieSetModel asks a media center which sets have a `set.nfo` and reads
///          those records through it; it uses four of the interface's forty methods.
///          The fifth implemented here, saveMovieSet(), is SetsWidget's -- the model
///          never writes a record, only reads and removes.  Everything else is a no-op.
///
///          The records are a QHash, not files, so a test can say "this set has a record
///          on disk" without a movie set information folder, a Settings singleton or a
///          temporary directory.
class MediaCenterInterfaceMock : public MediaCenterInterface
{
public:
    explicit MediaCenterInterfaceMock(QObject* parent = nullptr) : MediaCenterInterface(parent) {}

    struct Record
    {
        QString overview;
        TmdbId tmdbId{TmdbId::NoId};
    };

    /// \brief Whether a movie set information folder is configured.
    void setRecordsEnabled(bool enabled) { m_recordsEnabled = enabled; }
    /// \brief Whether set artwork has somewhere to go.  Independent of the records here.
    /// \details KodiXml derives one from the other; this mock lets a test set them apart,
    ///          which is how the two layouts are told apart without a real Settings.
    void setArtworkEnabled(bool enabled) { m_artworkEnabled = enabled; }
    /// \brief Puts a `set.nfo` for \p setName on the pretend disk.
    void putRecord(const QString& setName, Record record) { m_records.insert(setName, record); }
    void putRecord(const QString& setName) { putRecord(setName, Record()); }
    ELCH_NODISCARD bool hasRecordOnDisk(const QString& setName) const { return m_records.contains(setName); }
    /// \brief Makes every record removal refuse, as a read-only mount or a locked file does.
    void setRemovalRefused(bool refused) { m_removalRefused = refused; }
    /// \brief Makes every record write refuse, as a folder owned by another set does.
    void setWriteRefused(bool refused) { m_writeRefused = refused; }
    /// \brief Makes every artwork write refuse, as a failing QImage::save() does.
    void setArtworkRefused(bool refused) { m_artworkRefused = refused; }
    ELCH_NODISCARD int savedArtworkCount() const { return m_savedArtworkCount; }
    ELCH_NODISCARD int savedRecordCount() const { return m_savedRecordCount; }
    ELCH_NODISCARD int listingCount() const { return m_listingCount; }

    // The movie set record calls: the four MovieSetModel uses, plus saveMovieSet(),
    // which is SetsWidget's.
    ELCH_NODISCARD bool movieSetRecordsEnabled() const override { return m_recordsEnabled; }
    ELCH_NODISCARD QStringList movieSetsWithRecord() override
    {
        ++m_listingCount;
        return m_recordsEnabled ? QStringList(m_records.keys()) : QStringList();
    }
    bool loadMovieSet(MovieSet& set) override
    {
        if (!m_recordsEnabled || !m_records.contains(set.name())) {
            return false;
        }
        const Record& record = m_records[set.name()];
        set.setOverview(record.overview);
        set.setTmdbId(record.tmdbId);
        set.setChanged(false);
        return true;
    }
    bool saveMovieSet(MovieSet& set) override
    {
        if (!m_recordsEnabled || m_writeRefused) {
            return false;
        }
        ++m_savedRecordCount;
        m_records.insert(set.name(), Record{set.overview(), set.tmdbId()});
        set.setHasRecord(true);
        set.setChanged(false);
        return true;
    }
    bool removeMovieSetRecord(const QString& setName) override
    {
        if (!m_recordsEnabled || m_removalRefused) {
            return false;
        }
        m_records.remove(setName);
        return true;
    }

    // Everything below is unused by MovieSetModel and does nothing.
    bool saveMovie(Movie* /*movie*/) override { return false; }
    bool loadMovie(Movie* /*movie*/, QString /*nfoContent*/ = "") override { return false; }
    ELCH_NODISCARD bool movieSetArtworkEnabled() const override { return m_artworkEnabled; }
    QImage movieSetPoster(QString /*setName*/) override { return {}; }
    QImage movieSetBackdrop(QString /*setName*/) override { return {}; }
    ELCH_NODISCARD bool saveMovieSetPoster(QString /*setName*/, QImage /*poster*/) override { return saveArtwork(); }
    ELCH_NODISCARD bool saveMovieSetBackdrop(QString /*setName*/, QImage /*backdrop*/) override
    {
        return saveArtwork();
    }
    bool saveConcert(Concert* /*concert*/) override { return false; }
    bool loadConcert(Concert* /*concert*/, QString /*nfoContent*/ = "") override { return false; }
    bool loadTvShow(TvShow* /*show*/, QString /*nfoContent*/ = "") override { return false; }
    bool loadTvShowEpisode(TvShowEpisode* /*episode*/, QString /*nfoContent*/ = "") override { return false; }
    bool saveTvShow(TvShow* /*show*/) override { return false; }
    bool saveTvShowEpisode(TvShowEpisode* /*episode*/) override { return false; }
    QStringList extraFanartNames(Movie* /*movie*/) override { return {}; }
    QStringList extraFanartNames(TvShow* /*show*/) override { return {}; }
    QStringList extraFanartNames(Concert* /*concert*/) override { return {}; }
    QStringList extraFanartNames(Artist* /*artist*/) override { return {}; }
    bool saveArtist(Artist* /*artist*/) override { return false; }
    bool saveAlbum(Album* /*album*/) override { return false; }
    bool loadArtist(Artist* /*artist*/, QString /*nfoContent*/ = "") override { return false; }
    bool loadAlbum(Album* /*album*/, QString /*nfoContent*/ = "") override { return false; }
    QString actorImageName(Movie* /*movie*/, Actor /*actor*/) override { return {}; }
    QString actorImageName(TvShow* /*show*/, Actor /*actor*/) override { return {}; }
    QString actorImageName(TvShowEpisode* /*episode*/, Actor /*actor*/) override { return {}; }
    QString nfoFilePath(Movie* /*movie*/) override { return {}; }
    QString nfoFilePath(Concert* /*concert*/) override { return {}; }
    QString nfoFilePath(TvShowEpisode* /*episode*/) override { return {}; }
    QString nfoFilePath(TvShow* /*show*/) override { return {}; }
    QString nfoFilePath(Artist* /*artist*/) override { return {}; }
    QString nfoFilePath(Album* /*album*/) override { return {}; }
    // clang-format off
    QString imageFileName(const Movie* /*movie*/,           ImageType /*type*/, QVector<DataFile> /*dataFiles*/ = QVector<DataFile>(), bool /*constructName*/ = false) override { return {}; }
    QString imageFileName(const Concert* /*concert*/,       ImageType /*type*/, QVector<DataFile> /*dataFiles*/ = QVector<DataFile>(), bool /*constructName*/ = false) override { return {}; }
    QString imageFileName(const TvShowEpisode* /*episode*/, ImageType /*type*/, QVector<DataFile> /*dataFiles*/ = QVector<DataFile>(), bool /*constructName*/ = false) override { return {}; }
    QString imageFileName(const Artist* /*artist*/,         ImageType /*type*/, QVector<DataFile> /*dataFiles*/ = QVector<DataFile>(), bool /*constructName*/ = false) override { return {}; }
    QString imageFileName(const Album* /*album*/,           ImageType /*type*/, QVector<DataFile> /*dataFiles*/ = QVector<DataFile>(), bool /*constructName*/ = false) override { return {}; }
    QString imageFileName(const TvShow* /*show*/,           ImageType /*type*/, SeasonNumber /*season*/ = SeasonNumber::NoSeason, QVector<DataFile> /*dataFiles*/ = QVector<DataFile>(), bool /*constructName*/ = false) override { return {}; }
    // clang-format on
    void loadBooklets(Album* /*album*/) override {}

private:
    bool saveArtwork()
    {
        if (!m_artworkEnabled || m_artworkRefused) {
            return false;
        }
        ++m_savedArtworkCount;
        return true;
    }

    QHash<QString, Record> m_records;
    bool m_recordsEnabled = true;
    bool m_artworkEnabled = true;
    bool m_artworkRefused = false;
    int m_savedArtworkCount = 0;
    bool m_removalRefused = false;
    bool m_writeRefused = false;
    int m_savedRecordCount = 0;
    int m_listingCount = 0;
};
