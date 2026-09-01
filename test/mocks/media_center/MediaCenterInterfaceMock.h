#pragma once

#include "data/TmdbId.h"
#include "data/movie/MovieSet.h"
#include "media_center/MediaCenterInterface.h"

#include <QHash>
#include <QString>
#include <QStringList>

/// \brief A MediaCenterInterface that stores movie set records in memory.
/// \details MovieSetModel asks a media center which sets have a `set.nfo` and reads and
///          writes those records through it.  Everything else on the interface is a
///          no-op here: the model uses five of its forty methods.
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
    /// \brief Puts a `set.nfo` for \p setName on the pretend disk.
    void putRecord(const QString& setName, Record record) { m_records.insert(setName, record); }
    void putRecord(const QString& setName) { putRecord(setName, Record()); }
    ELCH_NODISCARD bool hasRecordOnDisk(const QString& setName) const { return m_records.contains(setName); }
    ELCH_NODISCARD int savedRecordCount() const { return m_savedRecordCount; }
    ELCH_NODISCARD int listingCount() const { return m_listingCount; }

    // movie set records -- the five methods MovieSetModel actually uses
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
        if (!m_recordsEnabled) {
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
        if (!m_recordsEnabled) {
            return false;
        }
        m_records.remove(setName);
        return true;
    }

    // Everything below is unused by MovieSetModel and does nothing.
    bool saveMovie(Movie* /*movie*/) override { return false; }
    bool loadMovie(Movie* /*movie*/, QString /*nfoContent*/ = "") override { return false; }
    QImage movieSetPoster(QString /*setName*/) override { return {}; }
    QImage movieSetBackdrop(QString /*setName*/) override { return {}; }
    void saveMovieSetPoster(QString /*setName*/, QImage /*poster*/) override {}
    void saveMovieSetBackdrop(QString /*setName*/, QImage /*backdrop*/) override {}
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
    QHash<QString, Record> m_records;
    bool m_recordsEnabled = true;
    int m_savedRecordCount = 0;
    int m_listingCount = 0;
};
