#include "KodiXml.h"

#include "data/Image.h"
#include "data/movie/MovieSet.h"
#include "globals/Globals.h"
#include "globals/Helper.h"
#include "globals/Manager.h"
#include "log/Log.h"
#include "media_center/kodi/AlbumXmlReader.h"
#include "media_center/kodi/AlbumXmlWriter.h"
#include "media_center/kodi/ArtistXmlReader.h"
#include "media_center/kodi/ArtistXmlWriter.h"
#include "media_center/kodi/ConcertXmlReader.h"
#include "media_center/kodi/ConcertXmlWriter.h"
#include "media_center/kodi/EpisodeXmlReader.h"
#include "media_center/kodi/EpisodeXmlWriter.h"
#include "media_center/kodi/MakeLegalFileName.h"
#include "media_center/kodi/MovieSetXmlReader.h"
#include "media_center/kodi/MovieSetXmlWriter.h"
#include "media_center/kodi/MovieXmlReader.h"
#include "media_center/kodi/MovieXmlWriter.h"
#include "media_center/kodi/TvShowXmlReader.h"
#include "media_center/kodi/TvShowXmlWriter.h"
#include "settings/Settings.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <array>
#include <memory>

KodiXml::KodiXml(mediaelch::KodiSettings& settings, MediaPersistence persistence, QObject* parent) :
    MediaCenterInterface(parent), m_settings{settings}, m_persistence{persistence}
{
}

KodiXml::~KodiXml() = default;


QByteArray KodiXml::getMovieXml(Movie* movie)
{
    auto version = m_settings.kodiVersion();
    auto writer = std::make_unique<mediaelch::kodi::MovieXmlWriterGeneric>(version, *movie);
    writer->setWriteThumbUrlsToNfo(Settings::instance()->advanced()->writeThumbUrlsToNfo());
    writer->setUseFirstStudioOnly(Settings::instance()->advanced()->useFirstStudioOnly());
    writer->setIgnoreDuplicateOriginalTitle(Settings::instance()->ignoreDuplicateOriginalTitle());
    return writer->getMovieXml();
}

/// \brief Saves a movie (including images)
/// \param movie Movie to save
/// \return Saving success
/// \see KodiXml::writeMovieXml
bool KodiXml::saveMovie(Movie* movie)
{
    qCDebug(generic) << "Save movie as Kodi NFO file; movie: " << movie->title();
    QByteArray xmlContent = getMovieXml(movie);

    if (movie->files().isEmpty()) {
        qCWarning(generic) << "Movie has no files";
        return false;
    }

    movie->setNfoContent(xmlContent);

    bool saved = false;
    QFileInfo fi(movie->files().first().toString());
    auto dataFiles = Settings::instance()->dataFiles(DataFileType::MovieNfo);
    for (DataFile& dataFile : dataFiles) {
        QString saveFileName = dataFile.saveFileName(fi.fileName(), SeasonNumber::NoSeason, movie->files().count() > 1);
        QString saveFilePath = fi.absolutePath() + "/" + saveFileName;
        QDir saveFileDir = QFileInfo(saveFilePath).dir();
        if (!saveFileDir.exists()) {
            saveFileDir.mkpath(".");
        }
        QFile file(saveFilePath);
        qCDebug(generic) << "Saving to" << file.fileName();
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            qCWarning(generic) << "File could not be opened for writing";
        } else {
            const auto bytesWritten = file.write(xmlContent);
            file.close();
            // if an error occurred, -1 is returned
            saved = bytesWritten != -1;
        }
    }
    if (!saved) {
        return false;
    }

    for (const auto imageType : Movie::imageTypes()) {
        DataFileType dataFileType = DataFile::dataFileTypeForImageType(imageType);
        if (movie->images().imageHasChanged(imageType) && !movie->images().image(imageType).isNull()) {
            for (DataFile dataFile : Settings::instance()->dataFiles(dataFileType)) {
                QString saveFileName =
                    dataFile.saveFileName(fi.fileName(), SeasonNumber::NoSeason, movie->files().count() > 1);
                if (imageType == ImageType::MoviePoster
                    && (movie->discType() == DiscType::BluRay || movie->discType() == DiscType::Dvd)) {
                    saveFileName = "poster.jpg";
                }
                if (imageType == ImageType::MovieBackdrop
                    && (movie->discType() == DiscType::BluRay || movie->discType() == DiscType::Dvd)) {
                    saveFileName = "fanart.jpg";
                }
                saveFile(getPath(movie).filePath(saveFileName), movie->images().image(imageType));
            }
        }

        if (movie->images().imagesToRemove().contains(imageType)) {
            for (DataFile dataFile : Settings::instance()->dataFiles(dataFileType)) {
                QString saveFileName =
                    dataFile.saveFileName(fi.fileName(), SeasonNumber::NoSeason, movie->files().count() > 1);
                if (imageType == ImageType::MoviePoster
                    && (movie->discType() == DiscType::BluRay || movie->discType() == DiscType::Dvd)) {
                    saveFileName = "poster.jpg";
                }
                if (imageType == ImageType::MovieBackdrop
                    && (movie->discType() == DiscType::BluRay || movie->discType() == DiscType::Dvd)) {
                    saveFileName = "fanart.jpg";
                }
                QFile(getPath(movie).filePath(saveFileName)).remove();
            }
        }
    }

    if (movie->inSeparateFolder() && !movie->files().isEmpty()) {
        for (const QString& file : movie->images().extraFanartsToRemove()) {
            QFile::remove(file);
        }
        QDir dir(movie->files().first().dir().toString() + "/extrafanart");
        if (!dir.exists() && !movie->images().extraFanartToAdd().isEmpty()) {
            QDir(movie->files().first().dir().toString()).mkdir("extrafanart");
        }
        for (const QByteArray& img : movie->images().extraFanartToAdd()) {
            int num = 1;
            while (QFileInfo::exists(dir.absolutePath() + "/" + QString("fanart%1.jpg").arg(num))) {
                ++num;
            }
            saveFile(dir.absolutePath() + "/" + QString("fanart%1.jpg").arg(num), img);
        }
    }

    for (const Actor* actor : movie->actors()) {
        if (!actor->image.isNull()) {
            QDir dir;
            dir.mkdir(fi.absolutePath() + "/" + ".actors");
            QString actorName = actor->name;
            actorName = actorName.replace(" ", "_");
            saveFile(fi.absolutePath() + "/" + ".actors" + "/" + actorName + ".jpg", actor->image);
        }
    }

    for (Subtitle* subtitle : movie->subtitles()) {
        if (subtitle->changed()) {
            QString subFileName = fi.completeBaseName();
            if (!subtitle->language().isEmpty()) {
                subFileName.append("." + subtitle->language());
            }
            if (subtitle->forced()) {
                subFileName.append(".forced");
            }

            QStringList newFiles;
            for (const QString& subFile : subtitle->files()) {
                QFileInfo subFi(fi.absolutePath() + "/" + subFile);
                QString newFileName = subFileName + "." + subFi.suffix();
                QFile f(fi.absolutePath() + "/" + subFile);
                if (f.rename(fi.absolutePath() + "/" + newFileName)) {
                    newFiles << newFileName;
                } else {
                    qCWarning(generic) << "Could not rename" << subFi.absoluteFilePath() << "to"
                                       << fi.absolutePath() + "/" + newFileName;
                    newFiles << subFi.fileName();
                }
            }
            subtitle->setFiles(newFiles);
        }
    }

    // TODO: Multithreaded?
    m_persistence.movies.update(movie);

    return true;
}

/**
 * \brief Tries to find an nfo file for the movie
 * \param movie Movie
 * \return Path to nfo file, if none found returns an empty string
 */
QString KodiXml::nfoFilePath(Movie* movie)
{
    QString nfoFile;
    if (movie->files().isEmpty()) {
        qCWarning(generic) << "Movie has no files";
        return nfoFile;
    }
    QFileInfo fi(movie->files().first().toString());
    if (!fi.isFile()) {
        qCWarning(generic) << "First file of the movie is not readable" << movie->files().at(0);
        return nfoFile;
    }

    for (DataFile dataFile : Settings::instance()->dataFiles(DataFileType::MovieNfo)) {
        QString file = dataFile.saveFileName(fi.fileName(), SeasonNumber::NoSeason, movie->files().count() > 1);
        QFileInfo nfoFi(fi.absolutePath() + "/" + file);
        if (nfoFi.exists()) {
            nfoFile = fi.absolutePath() + "/" + file;
            break;
        }
    }

    return nfoFile;
}

QString KodiXml::nfoFilePath(TvShowEpisode* episode)
{
    QString nfoFile;
    if (episode->files().isEmpty()) {
        qCWarning(generic) << "[KodiXml] Episode has no files";
        return nfoFile;
    }
    QFileInfo fi(episode->files().first().toString());
    if (!fi.isFile()) {
        qCWarning(generic) << "[KodiXml] First file of the episode is not readable" << episode->files().first();
        return nfoFile;
    }

    for (DataFile dataFile : Settings::instance()->dataFiles(DataFileType::TvShowEpisodeNfo)) {
        QString file = dataFile.saveFileName(fi.fileName(), SeasonNumber::NoSeason, episode->files().size() > 1);
        QFileInfo nfoFi(fi.absolutePath() + "/" + file);
        if (nfoFi.exists()) {
            nfoFile = fi.absolutePath() + "/" + file;
            break;
        }
    }

    return nfoFile;
}

QString KodiXml::nfoFilePath(TvShow* show)
{
    QString nfoFile;
    if (!show->dir().isValid()) {
        qCWarning(generic) << "[KodiXml] Show dir is empty";
        return nfoFile;
    }

    for (DataFile dataFile : Settings::instance()->dataFiles(DataFileType::TvShowNfo)) {
        QFile file(show->dir().filePath(dataFile.saveFileName("")));
        if (file.exists()) {
            nfoFile = file.fileName();
            break;
        }
    }

    return nfoFile;
}

/**
 * \brief Tries to find an nfo file for the concert
 * \param concert Concert
 * \return Path to nfo file, if none found returns an empty string
 */
QString KodiXml::nfoFilePath(Concert* concert)
{
    QString nfoFile;
    if (concert->files().isEmpty()) {
        qCWarning(generic) << "[KodiXml] Concert has no files";
        return nfoFile;
    }
    QFileInfo fi(concert->files().first().toString());
    if (!fi.isFile()) {
        qCWarning(generic) << "[KodiXml] First file of the concert is not readable" << concert->files().at(0);
        return nfoFile;
    }

    for (DataFile dataFile : Settings::instance()->dataFiles(DataFileType::ConcertNfo)) {
        QString file = dataFile.saveFileName(fi.fileName(), SeasonNumber::NoSeason, concert->files().size() > 1);
        QFileInfo nfoFi(fi.absolutePath() + "/" + file);
        if (nfoFi.exists()) {
            nfoFile = fi.absolutePath() + "/" + file;
            break;
        }
    }

    return nfoFile;
}

/**
 * \brief Loads movie infos (except images). Does not block signals of the movie.
 * \param movie Movie to load
 * \return Loading success
 */
bool KodiXml::loadMovie(Movie* movie, QString initialNfoContent)
{
    movie->clear();
    movie->setChanged(false);

    QString nfoContent;
    if (initialNfoContent.isEmpty()) {
        QString nfoFile = nfoFilePath(movie);
        if (nfoFile.isEmpty()) {
            return false;
        }

        QFile file(nfoFile);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qCWarning(generic) << "[KodiXml] File" << nfoFile << "could not be opened for reading";
            return false;
        }
        nfoContent = QString::fromUtf8(file.readAll());
        movie->setNfoContent(nfoContent);
        file.close();
    } else {
        nfoContent = initialNfoContent;
    }

    QDomDocument domDoc;
    domDoc.setContent(nfoContent);

    mediaelch::kodi::MovieXmlReader reader(*movie);
    const bool success = reader.parseNfoDom(domDoc);
    if (!success) {
        return false;
    }

    loadStreamDetails(movie->streamDetails(), domDoc);

    // Existence of images: If no NFO was given, the movie posters were set via
    // Database::moviesInDirectory, so no need to do file searches.
    // TODO: Refactor this condition: This implicit knowledge is hard to keep track of
    if (initialNfoContent.isEmpty()) {
        for (const auto imageType : Movie::imageTypes()) {
            movie->images().setHasImage(imageType, !imageFileName(movie, imageType).isEmpty());
        }
        movie->images().setHasExtraFanarts(!extraFanartNames(movie).isEmpty());
    }

    return true;
}

/// \brief Loads the stream details from the dom document
/// \param streamDetails StreamDetails object
/// \param domDoc Nfo document
/// \return Infos loaded
bool KodiXml::loadStreamDetails(StreamDetails* streamDetails, QDomDocument domDoc)
{
    streamDetails->clear();
    QDomNodeList elements = domDoc.elementsByTagName("streamdetails");
    if (elements.isEmpty()) {
        return false;
    }
    QDomElement elem = elements.at(0).toElement();
    return loadStreamDetails(streamDetails, elem);
}

bool KodiXml::loadStreamDetails(StreamDetails* streamDetails, QDomElement elem)
{
    bool hasDetails = false;
    if (!elem.elementsByTagName("video").isEmpty()) {
        // Only on `<video>` tag allowed by Kodi.
        hasDetails = true;
        QDomElement videoElem = elem.elementsByTagName("video").at(0).toElement();

        auto details = StreamDetails::allVideoDetailsAsList();
        for (const auto detail : details) {
            const QString detailStr = StreamDetails::detailToString(detail);
            QDomNodeList elements = videoElem.elementsByTagName(detailStr);
            if (!elements.isEmpty()) {
                streamDetails->setVideoDetail(detail, elements.at(0).toElement().text());
            }
        }
    }
    {
        QDomNodeList audioElements = elem.elementsByTagName("audio");
        auto audioDetails = StreamDetails::allAudioDetailsAsList();
        for (int i = 0, n = audioElements.count(); i < n; ++i) {
            hasDetails = true;
            QDomElement audioElem = audioElements.at(i).toElement();

            for (const auto detail : audioDetails) {
                const QString detailStr = StreamDetails::detailToString(detail);
                QDomNodeList detailElements = audioElem.elementsByTagName(detailStr);

                if (!detailElements.isEmpty()) {
                    streamDetails->setAudioDetail(i, detail, detailElements.at(0).toElement().text());
                }
            }
        }
    }
    {
        QDomNodeList subtitleElements = elem.elementsByTagName("subtitle");
        auto subtitleDetails = StreamDetails::allSubtitleDetailsAsList();
        for (int i = 0, n = subtitleElements.count(); i < n; ++i) {
            hasDetails = true;
            QDomElement subtitleElem = subtitleElements.at(i).toElement();
            if (!subtitleElem.elementsByTagName("file").isEmpty()) {
                continue;
            }
            for (const auto detail : subtitleDetails) {
                const auto detailStr = StreamDetails::detailToString(detail);
                if (!subtitleElem.elementsByTagName(detailStr).isEmpty()) {
                    streamDetails->setSubtitleDetail(
                        i, detail, subtitleElem.elementsByTagName(detailStr).at(0).toElement().text());
                }
            }
        }
    }
    streamDetails->setLoaded(hasDetails);
    return hasDetails;
}

void KodiXml::parseStreamDetails(QXmlStreamReader& reader, StreamDetails* streamDetails)
{
    int audioStreamNumber = 0;
    int subtitleStreamNumber = 0;
    bool hasDetails = false;

    while (reader.readNextStartElement()) {
        if (reader.name() == QLatin1String("video")) {
            hasDetails = true;
            parseVideoStreamDetails(reader, streamDetails);

        } else if (reader.name() == QLatin1String("audio")) {
            hasDetails = true;
            parseAudioStreamDetails(reader, audioStreamNumber, streamDetails);
            ++audioStreamNumber;

        } else if (reader.name() == QLatin1String("subtitle")) {
            hasDetails = true;
            parseSubtitleStreamDetails(reader, subtitleStreamNumber, streamDetails);
            ++subtitleStreamNumber;

        } else {
            reader.skipCurrentElement();
        }
    }

    streamDetails->setLoaded(hasDetails);
}

void KodiXml::parseVideoStreamDetails(QXmlStreamReader& reader, StreamDetails* streamDetails)
{
    while (reader.readNextStartElement()) {
        StreamDetails::VideoDetails detail = StreamDetails::stringToVideoDetail(reader.name().toString());
        if (detail != StreamDetails::VideoDetails::Unknown) {
            const QString value = reader.readElementText();
            if (!value.isEmpty()) {
                streamDetails->setVideoDetail(detail, value);
            }
        } else {
            reader.skipCurrentElement();
        }
    }
}

void KodiXml::parseAudioStreamDetails(QXmlStreamReader& reader, int streamNumber, StreamDetails* streamDetails)
{
    while (reader.readNextStartElement()) {
        StreamDetails::AudioDetails detail = StreamDetails::stringToAudioDetail(reader.name().toString());
        if (detail != StreamDetails::AudioDetails::Unknown) {
            const QString value = reader.readElementText();
            if (!value.isEmpty()) {
                streamDetails->setAudioDetail(streamNumber, detail, value);
            }
        } else {
            reader.skipCurrentElement();
        }
    }
}

void KodiXml::parseSubtitleStreamDetails(QXmlStreamReader& reader, int streamNumber, StreamDetails* streamDetails)
{
    while (reader.readNextStartElement()) {
        StreamDetails::SubtitleDetails detail = StreamDetails::stringToSubtitleDetail(reader.name().toString());
        if (detail != StreamDetails::SubtitleDetails::Unknown) {
            const QString value = reader.readElementText();
            if (!value.isEmpty()) {
                streamDetails->setSubtitleDetail(streamNumber, detail, value);
            }
        } else {
            reader.skipCurrentElement();
        }
    }
}


/// \brief Writes streamdetails to xml stream
/// \param xml XML Stream
/// \param streamDetails Stream Details object
void KodiXml::writeStreamDetails(QXmlStreamWriter& xml,
    const StreamDetails* const streamDetails,
    const QVector<Subtitle*>& subtitles)
{
    if (streamDetails == nullptr
        || (streamDetails->videoDetails().isEmpty() && streamDetails->audioDetails().isEmpty()
            && streamDetails->subtitleDetails().isEmpty())) {
        const bool hasStreamDetails = streamDetails != nullptr && streamDetails->hasLoaded();
        // We still write <fileinfo> and <streamdetails> because otherwise MediaElch
        // will always mark the media item as changed.
        if (hasStreamDetails) {
            xml.writeStartElement("fileinfo");
            xml.writeStartElement("streamdetails");
            xml.writeEndElement();
            xml.writeEndElement();
        }
        return;
    }

    xml.writeStartElement("fileinfo");
    xml.writeStartElement("streamdetails");

    xml.writeStartElement("video");
    QMapIterator<StreamDetails::VideoDetails, QString> itVideo(streamDetails->videoDetails());
    while (itVideo.hasNext()) {
        itVideo.next();
        if (itVideo.key() == StreamDetails::VideoDetails::Width && itVideo.value().toInt() == 0) {
            continue;
        }
        if (itVideo.key() == StreamDetails::VideoDetails::Height && itVideo.value().toInt() == 0) {
            continue;
        }
        if (itVideo.key() == StreamDetails::VideoDetails::DurationInSeconds && itVideo.value().toInt() == 0) {
            continue;
        }
        if (itVideo.value().isEmpty()) {
            continue;
        }

        QString value = itVideo.value();

        if (itVideo.key() == StreamDetails::VideoDetails::Aspect) {
            value = value.replace(",", ".");
        }

        xml.writeTextElement(StreamDetails::detailToString(itVideo.key()), value);
    }
    xml.writeEndElement();

    for (int i = 0, n = qsizetype_to_int(streamDetails->audioDetails().count()); i < n; ++i) {
        xml.writeStartElement("audio");
        QMapIterator<StreamDetails::AudioDetails, QString> itAudio(streamDetails->audioDetails().at(i));
        while (itAudio.hasNext()) {
            itAudio.next();
            if (itAudio.value() == "") {
                continue;
            }
            xml.writeTextElement(StreamDetails::detailToString(itAudio.key()), itAudio.value());
        }
        xml.writeEndElement();
    }

    for (int i = 0, n = qsizetype_to_int(streamDetails->subtitleDetails().count()); i < n; ++i) {
        xml.writeStartElement("subtitle");
        QMapIterator<StreamDetails::SubtitleDetails, QString> itSubtitle(streamDetails->subtitleDetails().at(i));
        while (itSubtitle.hasNext()) {
            itSubtitle.next();
            if (itSubtitle.value() == "") {
                continue;
            }
            xml.writeTextElement(StreamDetails::detailToString(itSubtitle.key()), itSubtitle.value());
        }
        xml.writeEndElement();
    }


    for (Subtitle* subtitle : subtitles) {
        xml.writeStartElement("subtitle");
        xml.writeTextElement("language", subtitle->language());
        xml.writeTextElement("file", subtitle->files().first());
        xml.writeEndElement();
    }

    xml.writeEndElement();
    xml.writeEndElement();
}

/**
 * \brief Get the path to the actor image
 * \param actor Actor
 * \return Path to actor image
 */
QString KodiXml::actorImageName(Movie* movie, Actor actor)
{
    if (movie->files().isEmpty()) {
        return QString();
    }
    QFileInfo fi(movie->files().first().toString());
    QString actorName = actor.name;
    actorName = actorName.replace(" ", "_");
    QString path = fi.absolutePath() + "/" + ".actors" + "/" + actorName + ".jpg";
    fi.setFile(path);
    if (fi.isFile()) {
        return path;
    }
    return QString();
}

QByteArray KodiXml::getConcertXml(Concert* concert)
{
    auto version = m_settings.kodiVersion();
    auto writer = std::make_unique<mediaelch::kodi::ConcertXmlWriterGeneric>(version, *concert);
    writer->setWriteThumbUrlsToNfo(Settings::instance()->advanced()->writeThumbUrlsToNfo());
    return writer->getConcertXml();
}

/**
 * \brief Saves a concert (including images)
 * \param concert Concert to save
 * \return Saving success
 * \see KodiXml::writeConcertXml
 */
bool KodiXml::saveConcert(Concert* concert)
{
    QByteArray xmlContent = getConcertXml(concert);

    if (concert->files().isEmpty()) {
        qCWarning(generic) << "[KodiXml] Concert has no files";
        return false;
    }

    concert->setNfoContent(xmlContent);
    m_persistence.concerts.update(concert);

    bool saved = false;
    QFileInfo fi(concert->files().first().toString());
    for (DataFile dataFile : Settings::instance()->dataFiles(DataFileType::ConcertNfo)) {
        QString saveFileName =
            dataFile.saveFileName(fi.fileName(), SeasonNumber::NoSeason, concert->files().size() > 1);
        QString saveFilePath = mediaelch::DirectoryPath(fi.absolutePath()).filePath(saveFileName);
        QDir saveFileDir = QFileInfo(saveFilePath).dir();
        if (!saveFileDir.exists()) {
            saveFileDir.mkpath(".");
        }
        QFile file(saveFilePath);
        qCDebug(generic) << "[KodiXml] Saving to" << file.fileName();
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            qCWarning(generic) << "[KodiXml] File could not be opened";
        } else {
            file.write(xmlContent);
            file.close();
            saved = true;
        }
    }
    if (!saved) {
        return false;
    }

    for (const auto imageType : Concert::imageTypes()) {
        DataFileType dataFileType = DataFile::dataFileTypeForImageType(imageType);
        if (concert->imageHasChanged(imageType) && !concert->image(imageType).isNull()) {
            for (DataFile dataFile : Settings::instance()->dataFiles(dataFileType)) {
                QString saveFileName =
                    dataFile.saveFileName(fi.fileName(), SeasonNumber::NoSeason, concert->files().size() > 1);
                if (imageType == ImageType::ConcertPoster
                    && (concert->discType() == DiscType::BluRay || concert->discType() == DiscType::Dvd)) {
                    saveFileName = "poster.jpg";
                }
                if (imageType == ImageType::ConcertBackdrop
                    && (concert->discType() == DiscType::BluRay || concert->discType() == DiscType::Dvd)) {
                    saveFileName = "fanart.jpg";
                }
                saveFile(getPath(concert).filePath(saveFileName), concert->image(imageType));
            }
        }
        if (concert->imagesToRemove().contains(imageType)) {
            for (DataFile dataFile : Settings::instance()->dataFiles(imageType)) {
                QString saveFileName =
                    dataFile.saveFileName(fi.fileName(), SeasonNumber::NoSeason, concert->files().count() > 1);
                if (imageType == ImageType::ConcertPoster
                    && (concert->discType() == DiscType::BluRay || concert->discType() == DiscType::Dvd)) {
                    saveFileName = "poster.jpg";
                }
                if (imageType == ImageType::ConcertBackdrop
                    && (concert->discType() == DiscType::BluRay || concert->discType() == DiscType::Dvd)) {
                    saveFileName = "fanart.jpg";
                }
                QFile(getPath(concert).filePath(saveFileName)).remove();
            }
        }
    }

    if (concert->inSeparateFolder() && !concert->files().isEmpty()) {
        for (const QString& file : concert->extraFanartsToRemove()) {
            QFile::remove(file);
        }
        QDir dir(QFileInfo(concert->files().first().toString()).absolutePath() + "/extrafanart");
        if (!dir.exists() && !concert->extraFanartImagesToAdd().isEmpty()) {
            QDir(QFileInfo(concert->files().first().toString()).absolutePath()).mkdir("extrafanart");
        }
        for (const QByteArray& img : concert->extraFanartImagesToAdd()) {
            int num = 1;
            while (QFileInfo::exists(dir.absolutePath() + "/" + QString("fanart%1.jpg").arg(num))) {
                ++num;
            }
            saveFile(dir.absolutePath() + "/" + QString("fanart%1.jpg").arg(num), img);
        }
    }

    return true;
}

/**
 * \brief Loads concert infos (except images)
 * \param concert Concert to load
 * \return Loading success
 */
bool KodiXml::loadConcert(Concert* concert, QString initialNfoContent)
{
    concert->clear();
    concert->setChanged(false);

    QString nfoContent;
    if (initialNfoContent.isEmpty()) {
        QString nfoFile = nfoFilePath(concert);
        if (nfoFile.isEmpty()) {
            return false;
        }

        QFile file(nfoFile);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qCWarning(generic) << "[KodiXml] File" << nfoFile << "could not be opened for reading";
            return false;
        }
        nfoContent = QString::fromUtf8(file.readAll());
        concert->setNfoContent(nfoContent);
        file.close();
    } else {
        nfoContent = initialNfoContent;
    }

    if (!nfoContent.isEmpty()) {
        QXmlStreamReader reader(nfoContent);
        mediaelch::kodi::ConcertXmlReader concertReader(*concert);
        concertReader.parse(reader);
        if (reader.hasError()) {
            qCCritical(generic) << "[KodiXml] Error parsing NFO file" << reader.errorString();
        }
    }

    // Existence of images
    if (initialNfoContent.isEmpty()) {
        for (const ImageType imageType : Concert::imageTypes()) {
            concert->setHasImage(imageType, !imageFileName(concert, imageType).isEmpty());
        }
        concert->setHasExtraFanarts(!extraFanartNames(concert).isEmpty());
    }

    return true;
}

/**
 * \brief Get path to actor image
 * \return Path to actor image
 */
QString KodiXml::actorImageName(TvShow* show, Actor actor)
{
    if (!show->dir().isValid()) {
        return QString();
    }
    QString actorName = actor.name;
    actorName = actorName.replace(" ", "_");
    QString fileName = show->dir().subDir(".actors").filePath(actorName + ".jpg");
    QFileInfo fi(fileName);
    if (fi.isFile()) {
        return fileName;
    }
    return QString();
}

QString KodiXml::actorImageName(TvShowEpisode* episode, Actor actor)
{
    if (episode->files().isEmpty()) {
        return QString();
    }
    QFileInfo fi(episode->files().first().toString());
    QString actorName = actor.name;
    actorName = actorName.replace(" ", "_");
    QString path = fi.absolutePath() + "/" + ".actors" + "/" + actorName + ".jpg";
    fi.setFile(path);
    if (fi.isFile()) {
        return path;
    }
    return QString();
}

/**
 * \brief Loads TV show information
 * \param show Show to load
 * \return Loading success
 */
bool KodiXml::loadTvShow(TvShow* show, QString initialNfoContent)
{
    show->clear();
    show->setChanged(false);

    QString nfoContent;
    if (initialNfoContent.isEmpty()) {
        if (!show->dir().isValid()) {
            return false;
        }

        QString nfoFile;
        for (DataFile dataFile : Settings::instance()->dataFiles(DataFileType::TvShowNfo)) {
            QString file = dataFile.saveFileName("");
            QFileInfo nfoFi(show->dir().filePath(file));
            if (nfoFi.exists()) {
                nfoFile = show->dir().filePath(file);
                break;
            }
        }
        if (nfoFile.isEmpty()) {
            // Movie has no NFO
            return false;
        }
        QFile file(nfoFile);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qCWarning(generic) << "[KodiXml] NFO file could not be opened for reading" << nfoFile;
            return false;
        }
        nfoContent = QString::fromUtf8(file.readAll());
        show->setNfoContent(nfoContent);
        file.close();
    } else {
        nfoContent = initialNfoContent;
    }

    QDomDocument domDoc;
    domDoc.setContent(nfoContent);

    mediaelch::kodi::TvShowXmlReader reader(*show);
    return reader.parseNfoDom(domDoc);
}

/**
 * \brief Loads TV show episode information
 * \param episode Episode to load infos for
 * \return Loading success
 */
bool KodiXml::loadTvShowEpisode(TvShowEpisode* episode, QString initialNfoContent)
{
    if (episode == nullptr) {
        qCWarning(generic) << "[KodiXml] Passed an empty (null) episode to loadTvShowEpisode";
        return false;
    }
    episode->clear();
    episode->setChanged(false);

    QString nfoContent;
    if (initialNfoContent.isEmpty()) {
        QString nfoFile = nfoFilePath(episode);
        if (nfoFile.isEmpty()) {
            return false;
        }

        QFile file(nfoFile);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qCWarning(generic) << "[KodiXml] File" << nfoFile << "could not be opened for reading";
            return false;
        }
        nfoContent = QString::fromUtf8(file.readAll());
        episode->setNfoContent(nfoContent);
        file.close();
    } else {
        nfoContent = initialNfoContent;
    }

    QDomDocument domDoc;
    domDoc.setContent(mediaelch::kodi::EpisodeXmlReader::makeValidEpisodeXml(nfoContent));

    QDomNodeList episodeDetailsList = domDoc.elementsByTagName("episodedetails");
    if (episodeDetailsList.isEmpty()) {
        return false;
    }

    QDomElement episodeDetails;
    if (episodeDetailsList.count() > 1) {
        bool found = false;
        for (int i = 0, n = episodeDetailsList.count(); i < n; ++i) {
            episodeDetails = episodeDetailsList.at(i).toElement();
            if (!episodeDetails.elementsByTagName("season").isEmpty()
                && episodeDetails.elementsByTagName("season").at(0).toElement().text().toInt()
                       == episode->seasonNumber().toInt()
                && !episodeDetails.elementsByTagName("episode").isEmpty()
                && episodeDetails.elementsByTagName("episode").at(0).toElement().text().toInt()
                       == episode->episodeNumber().toInt()) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }

    } else {
        episodeDetails = episodeDetailsList.at(0).toElement();
    }

    mediaelch::kodi::EpisodeXmlReader reader(*episode);
    const bool success = reader.parseNfoDom(episodeDetails);
    if (!success) {
        return false;
    }

    if (episodeDetails.elementsByTagName("streamdetails").count() > 0) {
        loadStreamDetails(
            episode->streamDetails(), episodeDetails.elementsByTagName("streamdetails").at(0).toElement());
    }

    return true;
}

/**
 * \brief Saves a TV show
 * \param show Show to save
 * \return Saving success
 * \see KodiXml::writeTvShowXml
 */
bool KodiXml::saveTvShow(TvShow* show)
{
    QByteArray xmlContent = getTvShowXml(show);

    if (!show->dir().isValid()) {
        return false;
    }

    show->setNfoContent(xmlContent);
    m_persistence.tvShows.update(show);

    for (DataFile dataFile : Settings::instance()->dataFiles(DataFileType::TvShowNfo)) {
        QString saveFilePath = show->dir().filePath(dataFile.saveFileName(""));
        QDir saveFileDir = QFileInfo(saveFilePath).dir();
        if (!saveFileDir.exists()) {
            saveFileDir.mkpath(".");
        }
        QFile file(saveFilePath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            qCWarning(generic) << "[KodiXml] NFO file could not be opened for writing" << file.fileName();
            return false;
        }
        file.write(xmlContent);
        file.close();
    }

    for (const auto imageType : TvShow::imageTypes()) {
        DataFileType dataFileType = DataFile::dataFileTypeForImageType(imageType);
        if (show->imageHasChanged(imageType) && !show->image(imageType).isNull()) {
            auto dataFiles = Settings::instance()->dataFiles(dataFileType);
            for (DataFile& dataFile : dataFiles) {
                QString saveFileName = dataFile.saveFileName("");
                saveFile(show->dir().filePath(saveFileName), show->image(imageType));
            }
        }
        if (show->imagesToRemove().contains(imageType)) {
            auto dataFiles = Settings::instance()->dataFiles(dataFileType);
            for (DataFile& dataFile : dataFiles) {
                QString saveFileName = dataFile.saveFileName("");
                QFile(show->dir().filePath(saveFileName)).remove();
            }
        }
    }

    for (const auto imageType : TvShow::seasonImageTypes()) {
        DataFileType dataFileType = DataFile::dataFileTypeForImageType(imageType);
        for (const SeasonNumber& season : show->seasons()) {
            if (show->seasonImageHasChanged(season, imageType) && !show->seasonImage(season, imageType).isNull()) {
                for (DataFile dataFile : Settings::instance()->dataFiles(dataFileType)) {
                    QString saveFileName = dataFile.saveFileName("", season);
                    saveFile(show->dir().filePath(saveFileName), show->seasonImage(season, imageType));
                }
            }
            if (show->imagesToRemove().contains(imageType)
                && show->imagesToRemove().value(imageType).contains(season)) {
                for (DataFile dataFile : Settings::instance()->dataFiles(dataFileType)) {
                    QString saveFileName = dataFile.saveFileName("", season);
                    QFile(show->dir().filePath(saveFileName)).remove();
                }
            }
        }
    }

    if (show->dir().isValid()) {
        for (const QString& file : show->extraFanartsToRemove()) {
            QFile::remove(file);
        }
        QDir dir(show->dir().toString() + "/extrafanart");
        if (!dir.exists() && !show->extraFanartImagesToAdd().isEmpty()) {
            QDir(show->dir().toString()).mkdir("extrafanart");
        }
        for (const QByteArray& img : show->extraFanartImagesToAdd()) {
            int num = 1;
            while (QFileInfo::exists(dir.absolutePath() + "/" + QString("fanart%1.jpg").arg(num))) {
                ++num;
            }
            saveFile(dir.absolutePath() + "/" + QString("fanart%1.jpg").arg(num), img);
        }
    }

    for (const Actor* actor : show->actors()) {
        if (!actor->image.isNull()) {
            QDir dir;
            dir.mkdir(show->dir().toString() + "/" + ".actors");
            QString actorName = actor->name;
            actorName = actorName.replace(" ", "_");
            saveFile(show->dir().toString() + "/" + ".actors" + "/" + actorName + ".jpg", actor->image);
        }
    }

    return true;
}

/**
 * \brief Saves a TV show episode
 * \param episode Episode to save
 * \return Saving success
 */
bool KodiXml::saveTvShowEpisode(TvShowEpisode* episode)
{
    // Multi-Episode handling
    QVector<TvShowEpisode*> episodes;
    for (TvShowEpisode* subEpisode : episode->tvShow()->episodes()) {
        if (subEpisode->isDummy()) {
            continue;
        }
        if (episode->files() == subEpisode->files()) {
            episodes.append(subEpisode);
        }
    }

    if (episode->files().isEmpty()) {
        qCWarning(generic) << "[KodiXml] Episode has no files";
        return false;
    }

    const QByteArray xmlContent = getEpisodeXml(episodes);
    for (TvShowEpisode* subEpisode : episodes) {
        subEpisode->setNfoContent(xmlContent);
        subEpisode->setSyncNeeded(true);
        subEpisode->setChanged(false);
        m_persistence.tvShows.update(subEpisode);
    }

    QFileInfo fi(episode->files().first().toString());
    for (DataFile dataFile : Settings::instance()->dataFiles(DataFileType::TvShowEpisodeNfo)) {
        QString saveFileName =
            dataFile.saveFileName(fi.fileName(), SeasonNumber::NoSeason, episode->files().count() > 1);
        QString saveFilePath = fi.absolutePath() + "/" + saveFileName;
        QDir saveFileDir = QFileInfo(saveFilePath).dir();
        if (!saveFileDir.exists()) {
            saveFileDir.mkpath(".");
        }
        QFile file(saveFilePath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            qCCritical(generic) << "[KodiXml] NFO file could not be opened for writing" << saveFileName;
            return false;
        }
        file.write(xmlContent);
        file.close();
    }

    fi.setFile(episode->files().first().toString());
    if (episode->thumbnailImageChanged() && !episode->thumbnailImage().isNull()) {
        if (helper::isBluRay(episode->files().at(0)) || helper::isDvd(episode->files().first())) {
            QDir dir = fi.dir();
            dir.cdUp();
            saveFile(dir.absolutePath() + "/thumb.jpg", episode->thumbnailImage());
        } else if (helper::isDvd(episode->files().first(), true)) {
            saveFile(fi.dir().absolutePath() + "/thumb.jpg", episode->thumbnailImage());
        } else {
            for (DataFile dataFile : Settings::instance()->dataFiles(DataFileType::TvShowEpisodeThumb)) {
                QString saveFileName =
                    dataFile.saveFileName(fi.fileName(), SeasonNumber::NoSeason, episode->files().count() > 1);
                saveFile(fi.absolutePath() + "/" + saveFileName, episode->thumbnailImage());
            }
        }
    }

    fi.setFile(episode->files().first().toString());
    if (episode->imagesToRemove().contains(ImageType::TvShowEpisodeThumb)) {
        if (helper::isBluRay(episode->files().first()) || helper::isDvd(episode->files().at(0))) {
            QDir dir = fi.dir();
            dir.cdUp();
            QFile(dir.absolutePath() + "/thumb.jpg").remove();
        } else if (helper::isDvd(episode->files().first(), true)) {
            QFile(fi.dir().absolutePath() + "/thumb.jpg").remove();
        } else {
            for (DataFile dataFile : Settings::instance()->dataFiles(DataFileType::TvShowEpisodeThumb)) {
                QString saveFileName =
                    dataFile.saveFileName(fi.fileName(), SeasonNumber::NoSeason, episode->files().count() > 1);
                QFile(fi.absolutePath() + "/" + saveFileName).remove();
            }
        }
    }

    fi.setFile(episode->files().first().toString());
    for (const Actor* actor : episode->actors()) {
        if (!actor->image.isNull()) {
            QDir dir;
            dir.mkdir(fi.absolutePath() + "/" + ".actors");
            QString actorName = actor->name;
            actorName = actorName.replace(" ", "_");
            saveFile(fi.absolutePath() + "/" + ".actors" + "/" + actorName + ".jpg", actor->image);
        }
    }

    return true;
}

QByteArray KodiXml::getTvShowXml(TvShow* show)
{
    auto version = m_settings.kodiVersion();
    auto writer = std::make_unique<mediaelch::kodi::TvShowXmlWriterGeneric>(version, *show);
    writer->setWriteThumbUrlsToNfo(Settings::instance()->advanced()->writeThumbUrlsToNfo());
    return writer->getTvShowXml();
}

/// \brief Get an NFO document for the given episode(s). If episodes.length() > 1,
///        then we do multi-episode handling which means we write multiple <episodedetails>
///        to the same document to merge information.
QByteArray KodiXml::getEpisodeXml(const QVector<TvShowEpisode*>& episodes)
{
    auto version = m_settings.kodiVersion();
    auto writer = std::make_unique<mediaelch::kodi::EpisodeXmlWriterGeneric>(version, episodes);
    writer->setWriteThumbUrlsToNfo(Settings::instance()->advanced()->writeThumbUrlsToNfo());
    writer->setUsePlotForOutline(Settings::instance()->usePlotForOutline());
    return writer->getEpisodeXml();
}

QStringList KodiXml::extraFanartNames(Movie* movie)
{
    if (movie->files().isEmpty() || !movie->inSeparateFolder()) {
        return QStringList();
    }
    QFileInfo fi(movie->files().first().toString());
    QDir dir(fi.absolutePath() + "/extrafanart");
    QStringList filters = {"*.jpg", "*.jpeg", "*.JPEG", "*.Jpeg", "*.JPeg"};
    QStringList files;
    for (const QString& file : dir.entryList(filters, QDir::Files | QDir::NoDotAndDotDot, QDir::Name)) {
        files << QDir::toNativeSeparators(dir.path() + "/" + file);
    }
    return files;
}

QStringList KodiXml::extraFanartNames(Concert* concert)
{
    if (concert->files().isEmpty() || !concert->inSeparateFolder()) {
        return QStringList();
    }
    QFileInfo fi(concert->files().first().toString());
    QDir dir(fi.absolutePath() + "/extrafanart");
    QStringList filters = {"*.jpg", "*.jpeg", "*.JPEG", "*.Jpeg", "*.JPeg"};
    QStringList files;
    for (const QString& file : dir.entryList(filters, QDir::Files | QDir::NoDotAndDotDot, QDir::Name)) {
        files << QDir::toNativeSeparators(dir.path() + "/" + file);
    }
    return files;
}

QStringList KodiXml::extraFanartNames(TvShow* show)
{
    if (!show->dir().isValid()) {
        return QStringList();
    }
    QDir dir(show->dir().subDir("extrafanart").toString());
    QStringList filters = {"*.jpg", "*.jpeg", "*.JPEG", "*.Jpeg", "*.JPeg"};
    QStringList files;
    for (const QString& file : dir.entryList(filters, QDir::Files | QDir::NoDotAndDotDot, QDir::Name)) {
        files << QDir::toNativeSeparators(dir.path() + "/" + file);
    }
    return files;
}

QStringList KodiXml::extraFanartNames(Artist* artist)
{
    QDir dir(artist->path().subDir("extrafanart").toString());
    QStringList filters = {"*.jpg", "*.jpeg", "*.JPEG", "*.Jpeg", "*.JPeg"};
    QStringList files;
    for (const QString& file : dir.entryList(filters, QDir::Files | QDir::NoDotAndDotDot, QDir::Name)) {
        files << QDir::toNativeSeparators(dir.path() + "/" + file);
    }
    return files;
}

bool KodiXml::movieSetArtworkEnabled() const
{
    // A disjunction, and it *calls* movieSetRecordsEnabled() rather than repeating its
    // condition, so that the record predicate stays the only place that decides what a
    // configured movie set information folder is.
    //
    // Artwork resolves in both layouts: "artwork next to movies" finds a member movie
    // and writes beside its folder (movieSetFileName() below), which is what MediaElch
    // ships with and what most users have.  A record resolves in one.  So the one
    // configuration where artwork has nowhere to go is the separate folder with no
    // folder chosen -- which is exactly what movieSetRecordsEnabled() already refuses.
    return Settings::instance()->movieSetArtworkType() == MovieSetArtworkType::ArtworkNextToMovies
           || movieSetRecordsEnabled();
}

QImage KodiXml::movieSetPoster(QString setName)
{
    return movieSetImage(setName, DataFileType::MovieSetPoster);
}

QImage KodiXml::movieSetBackdrop(QString setName)
{
    return movieSetImage(setName, DataFileType::MovieSetBackdrop);
}

QImage KodiXml::movieSetImage(const QString& setName, DataFileType type)
{
    const QVector<DataFile> dataFiles = Settings::instance()->dataFiles(type);

    for (DataFile dataFile : dataFiles) {
        QFileInfo fi(movieSetFileName(setName, &dataFile, LegalisePath::Yes));
        if (fi.exists()) {
            return QImage(fi.absoluteFilePath());
        }
    }

    // Only if nothing was found above: older MediaElch versions used the set name verbatim as the
    // folder name.  Kodi never read those folders, but MediaElch did, so keep reading them.
    if (Settings::instance()->movieSetArtworkType() == MovieSetArtworkType::SeparateArtworkFolder) {
        for (DataFile dataFile : dataFiles) {
            QFileInfo fi(movieSetFileName(setName, &dataFile, LegalisePath::No));
            if (fi.exists()) {
                return QImage(fi.absoluteFilePath());
            }
        }
    }

    return QImage();
}

/**
 * \brief Save movie set poster
 */
bool KodiXml::saveMovieSetPoster(QString setName, QImage poster)
{
    qCInfo(generic) << "[KodiXml] Saving movie set poster for movie set:" << setName;
    return saveMovieSetImage(setName, DataFileType::MovieSetPoster, poster);
}

/**
 * \brief Save movie set backdrop
 */
bool KodiXml::saveMovieSetBackdrop(QString setName, QImage backdrop)
{
    qCInfo(generic) << "[KodiXml] Saving movie set backdrop for movie set:" << setName;
    return saveMovieSetImage(setName, DataFileType::MovieSetBackdrop, backdrop);
}

/// \brief Writes \p image under every file name configured for \p type.
/// \return Whether the image reached the disk under every name that resolved to a path,
///         and at least one did.  A configured name that resolves to nothing is skipped
///         rather than counted as a failure, so this can be true while the image is not
///         on disk under every *configured* name.
/// \details "Nothing was attempted" is a failure, not a success.  It means either that
///          movieSetFileName() refused -- the separate artwork folder selected with no
///          folder chosen -- or that the set has no path in this layout at all, and in
///          both cases the image the caller is holding did not reach the disk.  The
///          caller has to be able to tell that apart from a save, because the image
///          exists nowhere else; see MediaCenterInterface::saveMovieSetPoster().
bool KodiXml::saveMovieSetImage(const QString& setName, DataFileType type, const QImage& image)
{
    int attempted = 0;
    int failed = 0;
    for (DataFile dataFile : Settings::instance()->dataFiles(type)) {
        const QString fileName = movieSetFileName(setName, &dataFile);
        if (fileName.isEmpty()) {
            // Nowhere to put it.  Not counted as a failed attempt, but not counted as an
            // attempt either, so a set with no resolvable path at all still answers no.
            continue;
        }
        ++attempted;
        QDir dir = QFileInfo(fileName).dir();
        if (!dir.exists() && !dir.mkpath(".")) {
            qCWarning(generic) << "[KodiXml] Cannot create movie set artwork directory" << dir.absolutePath();
            ++failed;
            continue;
        }
        if (!image.save(fileName, "jpg", 100)) {
            qCWarning(generic) << "[KodiXml] Cannot write movie set artwork" << fileName;
            ++failed;
        }
    }
    return attempted > 0 && failed == 0;
}

bool KodiXml::saveFile(QString filename, QByteArray data)
{
    QDir saveFileDir = QFileInfo(filename).dir();
    if (!saveFileDir.exists()) {
        saveFileDir.mkpath(".");
    }
    QFile file(filename);

    if (file.open(QIODevice::WriteOnly)) {
        file.write(data);
        file.close();
        return true;
    }
    return false;
}

mediaelch::DirectoryPath KodiXml::getPath(const Movie* movie)
{
    if (movie->files().isEmpty()) {
        return mediaelch::DirectoryPath();
    }
    QFileInfo fi(movie->files().first().toString());
    if (movie->discType() == DiscType::BluRay) {
        QDir dir = fi.dir();
        if (QString::compare(dir.dirName(), "BDMV", Qt::CaseInsensitive) == 0) {
            dir.cdUp();
        }
        return mediaelch::DirectoryPath(dir);
    }
    if (movie->discType() == DiscType::Dvd) {
        QDir dir = fi.dir();
        if (QString::compare(dir.dirName(), "VIDEO_TS", Qt::CaseInsensitive) == 0) {
            dir.cdUp();
        }
        return mediaelch::DirectoryPath(dir);
    }
    return mediaelch::DirectoryPath(fi.dir());
}

mediaelch::DirectoryPath KodiXml::getPath(const Concert* concert)
{
    if (concert->files().isEmpty()) {
        return mediaelch::DirectoryPath();
    }
    QFileInfo fi(concert->files().first().toString());
    if (concert->discType() == DiscType::BluRay) {
        QDir dir = fi.dir();
        if (QString::compare(dir.dirName(), "BDMV", Qt::CaseInsensitive) == 0) {
            dir.cdUp();
        }
        return mediaelch::DirectoryPath(dir);
    }
    if (concert->discType() == DiscType::Dvd) {
        QDir dir = fi.dir();
        if (QString::compare(dir.dirName(), "VIDEO_TS", Qt::CaseInsensitive) == 0) {
            dir.cdUp();
        }
        return mediaelch::DirectoryPath(dir);
    }
    return mediaelch::DirectoryPath(fi.dir());
}

namespace {

/// \brief Whether a movie set information folder is configured: the layout *and* a folder.
/// \details The one place the two are asked *together*, and the only place isValid() is
///          asked at all.  The layout on its own is a fair question and is asked in four
///          other places in this file, all of them choosing between the two artwork
///          layouts rather than deciding whether a folder exists.
///
///          The layout half, because a `set.nfo` and a per-set artwork folder only exist
///          in that layout at all.
///
///          The folder half, because DirectoryPath's default constructor leaves
///          isValid() false around a *default* QDir, whose absolutePath() is the
///          process's current working directory.  "Separate folder selected, folder never
///          chosen" therefore names a real, writable path in whatever directory MediaElch
///          was started from.
///
///          movieSetFileName() refuses that path too, and the two guards are still not
///          redundant -- but for one specific reason, so it is worth being exact.  Three
///          of the four `set.nfo` paths (the read, the write and the removal) build their
///          path through movieSetNfoFileName() and therefore through movieSetFileName(),
///          so that guard already covers them.  The fourth, movieSetsWithRecord(), does
///          not: it lists movieSetArtworkDirectory().dir() itself and never goes through
///          movieSetFileName().  It does build a path to each record it finds -- that is
///          how it opens them -- but it derives that path from the listing, so nothing on
///          that route is guarded.  **That enumeration is what this predicate is for.**
///          Without it, a `set.nfo` sitting in the working directory would be reported as
///          a record, and having a record is what decides whether the model keeps or
///          drops a set.  Both guards are pinned by tests in testKodi_v22_movie_set.cpp.
bool movieSetFolderIsConfigured()
{
    return Settings::instance()->movieSetArtworkType() == MovieSetArtworkType::SeparateArtworkFolder
           && Settings::instance()->movieSetArtworkDirectory().isValid();
}

/// \brief Opens and parses the movie set record at \p fileName.
/// \return false if it could not be opened, which is the caller's cue to refuse.
/// \details Every path that touches a `set.nfo` -- the read, the write, the removal and
///          the enumeration -- goes through this one function, so that they cannot drift
///          into asking different questions about the same file.  There are exactly four
///          of them and each one has to establish, before it acts, which set the file it
///          is about to touch belongs to; the answer is
///          MovieSetXmlReader::setNameOf(domDoc).
///
///          A file that cannot be opened yields *no* answer, and no caller may treat
///          that as permission to proceed.  On Unix, unlinking needs write permission on
///          the containing directory and nothing at all on the file, so a `set.nfo` that
///          cannot be read can still be deleted -- which is precisely the case where
///          falling through would destroy a file whose owner was never established.
bool readMovieSetRecord(const QString& fileName, QDomDocument& domDoc)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) {
        qCWarning(generic) << "[KodiXml] Cannot read movie set record" << fileName;
        return false;
    }
    domDoc.setContent(file.readAll());
    file.close();
    return true;
}

} // namespace

bool KodiXml::movieSetRecordsEnabled() const
{
    // A record lives in the movie set information folder and nowhere else, so having one
    // configured is the whole question.
    return movieSetFolderIsConfigured();
}

QString KodiXml::movieSetNfoFileName(const QString& setName)
{
    if (setName.isEmpty() || !movieSetRecordsEnabled()) {
        return {};
    }
    // A name that legalises away to nothing -- ".", "...", a run of spaces, all of which
    // the sets tab's rename field accepts -- would otherwise build "<msif>//set.nfo" and
    // drop the record straight into the movie set information folder's root.  Nothing
    // enumerates that, since the listing descends into subfolders, so the record would be
    // found by the direct probe and not by the listing: the same split answer that the
    // hidden-folder case produced.  Such a set simply has nowhere to keep a record.
    if (mediaelch::kodi::makeLegalFileName(setName).isEmpty()) {
        return {};
    }
    // The gate above has to come first.  movieSetFileName() does not resolve to nothing
    // in the other artwork layout: for a set that has members it happily returns
    // "<the first member's folder>/set.nfo", which is a real path Kodi never reads.
    //
    // "set.nfo" is Kodi's fixed name for this file, so the DataFile is built here rather
    // than read from Settings, where every other file name is user-configurable.
    DataFile dataFile(DataFileType::MovieSetNfo, "set.nfo", 0);
    return movieSetFileName(setName, &dataFile, LegalisePath::Yes);
}

QStringList KodiXml::movieSetsWithRecord()
{
    if (!movieSetRecordsEnabled()) {
        return {};
    }
    const QDir msif = Settings::instance()->movieSetArtworkDirectory().dir();
    QStringList setNames;
    // QDir::Hidden, because QDir::NoDotAndDotDot only drops "." and "..": without it a
    // set whose legalised folder begins with a dot -- ".hack Collection" is a real
    // collection -- is invisible here while loadMovieSet() opens its path directly and
    // finds it.  That split answer is exactly what the two halves below exist to close,
    // so the listing has to be able to see everything the probe can.
    const QStringList folders = msif.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QString& folder : folders) {
        // A folder holding artwork but no `set.nfo` is not a set.  The record is what
        // makes a set exist in its own right; treating a pile of images as one would
        // resurrect every set a user ever deliberately removed.
        const QString fileName = msif.absoluteFilePath(folder) + "/set.nfo";
        if (!QFileInfo::exists(fileName)) {
            continue;
        }
        QDomDocument domDoc;
        if (!readMovieSetRecord(fileName, domDoc)) {
            continue;
        }
        // Read from the file, not taken from the folder name: the folder name is the
        // set name run through Kodi's legalisation, which is lossy -- a set called
        // "Mission: Impossible" lives in "Mission_ Impossible" -- and the name has to
        // match the member NFOs' <set><name> byte for byte.
        const QString setName = mediaelch::kodi::MovieSetXmlReader::setNameOf(domDoc);
        if (setName.isEmpty()) {
            qCWarning(generic) << "[KodiXml] Movie set record names no set:" << fileName;
            continue;
        }
        // And only if the name resolves back to the folder it was found in.  This is
        // half of the one question every path here asks -- "is there a record whose
        // <originaltitle> is this name, where that name's own path puts it?" -- and
        // loadMovieSet() asks the other half.  Both halves are needed: a record reported
        // from a folder the name does not resolve to would be looked for elsewhere by
        // loadMovieSet(), written a second time elsewhere by saveMovieSet() and not
        // removed at all by removeMovieSetRecord(), so the set would flip between having
        // a record and not having one from one reload to the next.
        if (mediaelch::kodi::makeLegalFileName(setName) != folder) {
            qCWarning(generic) << "[KodiXml] Ignoring movie set record" << fileName << "-- it names" << setName
                               << "but that set's folder would be" << mediaelch::kodi::makeLegalFileName(setName);
            continue;
        }
        setNames.append(setName);
    }
    return setNames;
}

bool KodiXml::loadMovieSet(MovieSet& set)
{
    const QString fileName = movieSetNfoFileName(set.name());
    if (fileName.isEmpty()) {
        return false;
    }
    if (!QFileInfo::exists(fileName)) {
        return false;
    }
    QDomDocument domDoc;
    if (!readMovieSetRecord(fileName, domDoc)) {
        return false;
    }

    // Checked *before* anything is applied to the set.  The path is derived from the set
    // name through a lossy legalisation, so the file sitting there is not necessarily
    // this set's: two names can share one folder ("Mission: Impossible" and
    // "Mission_ Impossible" both legalise to the latter), a case-insensitive file system
    // hands back a folder whose name differs in case, and a folder can be renamed by
    // hand.  Answering "found" for someone else's record would read their overview and
    // id into this set and, worse, mark this set as having a record -- which is what
    // decides whether it survives losing its last member and whether removeSet() deletes
    // a file.
    const QString recordName = mediaelch::kodi::MovieSetXmlReader::setNameOf(domDoc);
    if (recordName.isEmpty()) {
        qCWarning(generic) << "[KodiXml] Movie set record names no set:" << fileName;
        return false;
    }
    if (recordName != set.name()) {
        qCWarning(generic) << "[KodiXml] Movie set record" << fileName << "names" << recordName << "and not"
                           << set.name() << "-- the two names share one folder; treating" << set.name()
                           << "as having no record.";
        return false;
    }

    mediaelch::kodi::MovieSetXmlReader reader(set);
    if (!reader.parseNfoDom(domDoc)) {
        qCWarning(generic) << "[KodiXml] Movie set record is not a <set> document:" << fileName;
        return false;
    }
    // The reader wrote the set's fields, and every one of those setters marks the set as
    // needing to be saved.  It does not: this *is* what is on disk.  Leaving the flag
    // set would have MediaElch warn about discarding unsaved changes to a set nobody
    // touched.
    set.setChanged(false);
    return true;
}

bool KodiXml::saveMovieSet(MovieSet& set)
{
    const QString fileName = movieSetNfoFileName(set.name());
    if (fileName.isEmpty()) {
        qCWarning(generic) << "[KodiXml] Not saving the record of movie set" << set.name()
                           << "-- it has no path of its own: either no movie set information folder is"
                           << "configured, or the name legalises away to nothing.";
        return false;
    }
    // The writer is the fourth path to this file and the last one to be brought under the
    // same question, which is how it managed to be the one that destroyed data: it
    // resolved the path and wrote whatever was there.  Two names can share one folder --
    // "Alien Collection" and "Alien Collection ", since legalisation chops the trailing
    // space, and a sloppy member NFO produces the padded one because the movie NFO reader
    // does not trim `<set><name>` either -- so saving the second silently replaced the
    // first's overview and id.  The owner's own read then failed the name check and the
    // enumeration reported the lodger, so the owner lost its record flag and became
    // droppable.
    //
    // The question is asked the other way round here, and the difference is not
    // cosmetic.  loadMovieSet() may demand that the file names this set, because a record
    // it cannot find simply does not exist.  A *write* has to be able to create a record
    // where there is no file at all, so the refusal is "a file is already there and it
    // names some other set" -- not "the file names this set".  Requiring a match would
    // make it impossible to write the first record for any set.
    if (QFileInfo::exists(fileName)) {
        QDomDocument existing;
        if (!readMovieSetRecord(fileName, existing)) {
            // Ownership cannot be established, so it is not ours to overwrite.
            qCWarning(generic) << "[KodiXml] Not saving the record of movie set" << set.name() << "--" << fileName
                               << "is already there and cannot be read.";
            return false;
        }
        const QString recordName = mediaelch::kodi::MovieSetXmlReader::setNameOf(existing);
        if (!recordName.isEmpty() && recordName != set.name()) {
            qCWarning(generic) << "[KodiXml] Not saving the record of movie set" << set.name() << "--" << fileName
                               << "is the record of" << recordName << "; the two names share one folder.";
            return false;
        }
    }

    const mediaelch::kodi::MovieSetXmlWriter writer(set);
    if (!saveFile(fileName, writer.getMovieSetXml())) {
        qCWarning(generic) << "[KodiXml] Cannot write movie set record" << fileName;
        return false;
    }
    set.setHasRecord(true);
    // The clearing edge for MovieSet::hasChanged().  Nothing else has ever cleared that
    // flag, which is why it could not be used to decide whether a set survives losing
    // its last member; it can be cleared now because there is finally a moment at which
    // the set and its file agree.
    set.setChanged(false);
    return true;
}

bool KodiXml::removeMovieSetRecord(const QString& setName)
{
    const QString fileName = movieSetNfoFileName(setName);
    if (fileName.isEmpty()) {
        return false;
    }
    QFile file(fileName);
    if (!file.exists()) {
        // Already gone, which is what the caller wanted.
        return true;
    }
    // The same lossy legalisation that loadMovieSet() guards against, at the one place
    // where being wrong destroys a file: two set names can share a folder, and only one
    // of them owns the record in it.
    //
    // This is a live guard, not a belt-and-braces one.  MovieSet::setName() does not
    // clear hasRecord(), and making the record follow a rename is deferred to D3a, so
    // renaming a set in the sets tab and then deleting it arrives here with a stale flag
    // and a name whose folder holds somebody else's file.  Do not read it as dead code.
    //
    // And it fails *closed*.  A record that cannot be opened yields no owner, and on Unix
    // unlinking needs write permission on the directory and nothing on the file, so an
    // unreadable `set.nfo` is perfectly deletable -- proceeding would destroy a file
    // whose owner was never established, and report success for it.
    QDomDocument domDoc;
    if (!readMovieSetRecord(fileName, domDoc)) {
        qCWarning(generic) << "[KodiXml] Not removing movie set record" << fileName
                           << "-- it cannot be read, so it cannot be shown to belong to" << setName;
        return false;
    }
    const QString recordName = mediaelch::kodi::MovieSetXmlReader::setNameOf(domDoc);
    if (recordName != setName) {
        qCWarning(generic) << "[KodiXml] Not removing movie set record" << fileName << "-- it names" << recordName
                           << "and not" << setName;
        return false;
    }
    if (!file.remove()) {
        qCWarning(generic) << "[KodiXml] Cannot remove movie set record" << fileName;
        return false;
    }
    // Only the record.  The folder and any artwork in it stay: they may still be wanted,
    // MediaElch has never deleted them, and a folder without a `set.nfo` is not a set,
    // so leaving it behind cannot bring the set back.
    return true;
}

namespace {

/// \brief Renames \p dir's set artwork from \p oldName's file names to \p newName's.
/// \details Artwork file names are user-configurable and may embed the set's name --
///          "<setName>-folder.jpg" is what the artwork-next-to-movies layout ships with,
///          and nothing stops a user configuring the same in the other layout.  Moving a
///          folder does not rename what is inside it, so this runs in both layouts: in
///          one it is the whole move, in the other it is the part the directory rename
///          could not do.  Where the configured name carries no placeholder the two file
///          names are equal and this does nothing, which is the common case.
///
///          These can genuinely half-succeed, one file at a time.  A file that did move
///          is individually correct, so the ones that did are kept and the caller is told
///          the whole thing failed; putting them back would be a second batch of renames
///          with the same failure mode.
bool renameSetArtworkFilesIn(const QDir& dir, const QString& oldName, const QString& newName)
{
    bool allMoved = true;
    for (const DataFileType type : {DataFileType::MovieSetPoster, DataFileType::MovieSetBackdrop}) {
        for (DataFile dataFile : Settings::instance()->dataFiles(type)) {
            const QString oldFileName = dir.absoluteFilePath(dataFile.saveFileName(oldName));
            const QString newFileName = dir.absoluteFilePath(dataFile.saveFileName(newName));
            if (oldFileName == newFileName || !QFileInfo::exists(oldFileName)) {
                continue;
            }
            if (QFileInfo::exists(newFileName)) {
                qCWarning(generic) << "[KodiXml] Not moving movie set artwork" << oldFileName << "to" << newFileName
                                   << "-- a file is already there.";
                allMoved = false;
                continue;
            }
            if (!QFile::rename(oldFileName, newFileName)) {
                qCWarning(generic) << "[KodiXml] Cannot move movie set artwork" << oldFileName << "to" << newFileName;
                allMoved = false;
            }
        }
    }
    return allMoved;
}

} // namespace

bool KodiXml::renameMovieSetFiles(const QString& oldName, const QString& newName)
{
    if (oldName.isEmpty() || newName.isEmpty()) {
        return true;
    }

    if (Settings::instance()->movieSetArtworkType() == MovieSetArtworkType::SeparateArtworkFolder) {
        if (!movieSetFolderIsConfigured()) {
            // No folder, so no record and no per-set artwork folder: nothing exists to
            // be moved, which is a success and not a refusal.
            return true;
        }
        const QString oldFolder = mediaelch::kodi::makeLegalFileName(oldName);
        const QString newFolder = mediaelch::kodi::makeLegalFileName(newName);
        if (oldFolder.isEmpty() || newFolder.isEmpty() || oldFolder == newFolder) {
            // Legalisation is lossy, so two different names can share one folder --
            // "Mission: Impossible" and "Mission_ Impossible".  Renaming between them
            // moves nothing on disk, and trying would rename a directory onto itself.
            return true;
        }
        QDir msif = Settings::instance()->movieSetArtworkDirectory().dir();
        if (!msif.exists(oldFolder)) {
            return true;
        }

        // The same ownership question removeMovieSetRecord() asks, at the one other
        // place where being wrong touches somebody else's files.  A folder is only this
        // set's to move if the record in it names this set; a record that cannot be read
        // yields no owner at all, and moving on that basis would carry away a folder
        // whose owner was never established.  A folder with no record is artwork alone
        // and has no other claimant, so it moves.
        const QString oldRecord = msif.absoluteFilePath(oldFolder) + "/set.nfo";
        if (QFileInfo::exists(oldRecord)) {
            QDomDocument domDoc;
            if (!readMovieSetRecord(oldRecord, domDoc)) {
                qCWarning(generic) << "[KodiXml] Not moving movie set folder" << oldFolder
                                   << "-- the record in it cannot be read, so it cannot be shown to belong to"
                                   << oldName;
                return false;
            }
            const QString recordName = mediaelch::kodi::MovieSetXmlReader::setNameOf(domDoc);
            if (recordName != oldName) {
                qCWarning(generic) << "[KodiXml] Not moving movie set folder" << oldFolder << "-- its record names"
                                   << recordName << "and not" << oldName;
                return false;
            }
        }

        if (QFileInfo::exists(msif.absoluteFilePath(newFolder))) {
            // Refused rather than merged.  The target holds either another set's record
            // -- which a rename may not walk over -- or its artwork, and folding two art
            // folders together would let this set's poster shadow the other's with no
            // way to tell afterwards which came from where.
            qCWarning(generic) << "[KodiXml] Not moving movie set folder" << oldFolder << "to" << newFolder
                               << "-- something is already there.";
            return false;
        }

        // One rename, not a copy-and-delete loop.  Within a file system this cannot half
        // succeed, and a partial move is worse than either endpoint: the record in one
        // folder and the artwork in another is a state nothing here can recognise.
        // Across file systems QDir::rename() fails outright rather than falling back to
        // a copy, which is exactly the failure worth having -- nothing moved.
        if (!msif.rename(oldFolder, newFolder)) {
            qCWarning(generic) << "[KodiXml] Cannot move movie set folder" << msif.absoluteFilePath(oldFolder) << "to"
                               << msif.absoluteFilePath(newFolder);
            return false;
        }
        // The folder moved; what is inside it did not get a new name.  A configured
        // artwork file name that embeds the set's name would still spell the old one,
        // and MediaElch would then look for this set's poster where it no longer is.
        return renameSetArtworkFilesIn(QDir(msif.absoluteFilePath(newFolder)), oldName, newName);
    }

    // Artwork next to movies.  There is no per-set folder here and no record at all; the
    // artwork sits beside a member movie under a file name built from the set's name, so
    // the files are renamed where they lie.  The directory is found through the *old*
    // name -- movieSetFileName() locates a movie whose set().name is still that -- which
    // is why this has to run before the members are reassigned; see the interface's
    // warning.
    DataFile probe(DataFileType::MovieSetPoster, "probe", 0);
    const QString anchor = movieSetFileName(oldName, &probe);
    if (anchor.isEmpty()) {
        // No member movie with a file, so this set has no artwork path in this layout at
        // all and there is nothing to move.
        return true;
    }
    return renameSetArtworkFilesIn(QFileInfo(anchor).dir(), oldName, newName);
}

QString KodiXml::movieSetFileName(QString setName, DataFile* dataFile, LegalisePath legalise)
{
    if (Settings::instance()->movieSetArtworkType() == MovieSetArtworkType::SeparateArtworkFolder) {
        if (!movieSetFolderIsConfigured()) {
            // Inside this branch that is exactly "no folder was ever chosen", and it is
            // asked through the shared predicate rather than by testing isValid() again,
            // so that there is one derivation of it and not two.
            //
            // Without it the line below hands back a real, writable path in whatever
            // directory MediaElch was started from -- see movieSetFolderIsConfigured() --
            // and every caller acts on it: the savers mkpath() and write there, and the
            // reader displays whatever it happens to find there as this set's artwork.
            //
            // Refused here, where the path is built, rather than in each caller: that
            // closes it for all four of them and for any added later.  It has to come
            // *before* the path is resolved and cannot be inferred from an empty result,
            // because the other layout below returns an empty string for an entirely
            // different reason.
            return {};
        }
        QDir dir = Settings::instance()->movieSetArtworkDirectory().dir();
        // Kodi legalises only the folder component of this path, so the file name keeps using
        // MediaElch's own sanitiser.  The two are intentionally different.
        QString fileName = dataFile->saveFileName(setName);
        QString folderName = legalise == LegalisePath::Yes ? mediaelch::kodi::makeLegalFileName(setName) : setName;
        return dir.absolutePath() + "/" + folderName + "/" + fileName;
    }
    if (Settings::instance()->movieSetArtworkType() == MovieSetArtworkType::ArtworkNextToMovies) {
        for (Movie* movie : Manager::instance()->movieModel()->movies()) {
            if (movie->set().name == setName && !movie->files().isEmpty()) {
                QFileInfo fi(movie->files().first().toString());
                QDir dir = fi.dir();
                if (movie->inSeparateFolder()) {
                    dir.cdUp();
                }
                if (movie->discType() == DiscType::Dvd || movie->discType() == DiscType::BluRay) {
                    dir.cdUp();
                }
                return dir.absolutePath() + "/" + dataFile->saveFileName(setName);
            }
        }
    }

    return QString();
}

QString KodiXml::imageFileName(const Movie* movie, ImageType type, QVector<DataFile> dataFiles, bool constructName)
{
    DataFileType fileType = [type]() {
        switch (type) {
        case ImageType::MoviePoster: return DataFileType::MoviePoster;
        case ImageType::MovieBackdrop: return DataFileType::MovieBackdrop;
        case ImageType::MovieLogo: return DataFileType::MovieLogo;
        case ImageType::MovieBanner: return DataFileType::MovieBanner;
        case ImageType::MovieThumb: return DataFileType::MovieThumb;
        case ImageType::MovieClearArt: return DataFileType::MovieClearArt;
        case ImageType::MovieCdArt: return DataFileType::MovieCdArt;
        default: return DataFileType::NoType;
        }
    }();

    if (fileType == DataFileType::NoType) {
        return "";
    }

    if (movie->files().isEmpty()) {
        qCWarning(generic) << "Movie has no files";
        return "";
    }

    if (!constructName) {
        dataFiles = Settings::instance()->dataFiles(fileType);
    }

    QString fileName;
    QFileInfo fi(movie->files().first().toString());
    for (DataFile dataFile : dataFiles) {
        QString file = dataFile.saveFileName(fi.fileName(), SeasonNumber::NoSeason, movie->files().count() > 1);
        if (movie->discType() == DiscType::BluRay || movie->discType() == DiscType::Dvd) {
            if (type == ImageType::MoviePoster) {
                file = "poster.jpg";
            } else if (type == ImageType::MovieBackdrop) {
                file = "fanart.jpg";
            }
        }
        mediaelch::DirectoryPath path = getPath(movie);
        QFileInfo pFi(path.filePath(file));
        if (pFi.isFile() || constructName) {
            fileName = path.filePath(file);
            break;
        }
    }

    return fileName;
}

QString KodiXml::imageFileName(const Concert* concert, ImageType type, QVector<DataFile> dataFiles, bool constructName)
{
    DataFileType fileType;
    switch (type) {
    case ImageType::ConcertPoster: fileType = DataFileType::ConcertPoster; break;
    case ImageType::ConcertBackdrop: fileType = DataFileType::ConcertBackdrop; break;
    case ImageType::ConcertLogo: fileType = DataFileType::ConcertLogo; break;
    case ImageType::ConcertClearArt: fileType = DataFileType::ConcertClearArt; break;
    case ImageType::ConcertCdArt: fileType = DataFileType::ConcertCdArt; break;
    default: return "";
    }

    if (concert->files().isEmpty()) {
        qCWarning(generic) << "[KodiXml] Concert has no files";
        return "";
    }

    if (!constructName) {
        dataFiles = Settings::instance()->dataFiles(fileType);
    }

    QString fileName;
    QFileInfo fi(concert->files().first().toString());
    for (DataFile dataFile : dataFiles) {
        QString file = dataFile.saveFileName(fi.fileName(), SeasonNumber::NoSeason, concert->files().count() > 1);
        if (concert->discType() == DiscType::BluRay || concert->discType() == DiscType::Dvd) {
            if (type == ImageType::ConcertPoster) {
                file = "poster.jpg";
            }
            if (type == ImageType::ConcertBackdrop) {
                file = "fanart.jpg";
            }
        }
        mediaelch::DirectoryPath path = getPath(concert);
        QFileInfo pFi(path.filePath(file));
        if (pFi.isFile() || constructName) {
            fileName = path.filePath(file);
            break;
        }
    }

    return fileName;
}

QString KodiXml::imageFileName(const TvShow* show,
    ImageType type,
    SeasonNumber season,
    QVector<DataFile> dataFiles,
    bool constructName)
{
    DataFileType fileType;
    switch (type) {
    case ImageType::TvShowPoster: fileType = DataFileType::TvShowPoster; break;
    case ImageType::TvShowBackdrop: fileType = DataFileType::TvShowBackdrop; break;
    case ImageType::TvShowLogos: fileType = DataFileType::TvShowLogo; break;
    case ImageType::TvShowBanner: fileType = DataFileType::TvShowBanner; break;
    case ImageType::TvShowThumb: fileType = DataFileType::TvShowThumb; break;
    case ImageType::TvShowClearArt: fileType = DataFileType::TvShowClearArt; break;
    case ImageType::TvShowCharacterArt: fileType = DataFileType::TvShowCharacterArt; break;
    case ImageType::TvShowSeasonPoster: fileType = DataFileType::TvShowSeasonPoster; break;
    case ImageType::TvShowSeasonBackdrop: fileType = DataFileType::TvShowSeasonBackdrop; break;
    case ImageType::TvShowSeasonBanner: fileType = DataFileType::TvShowSeasonBanner; break;
    case ImageType::TvShowSeasonThumb: fileType = DataFileType::TvShowSeasonThumb; break;
    default: return "";
    }

    if (!show->dir().isValid()) {
        return QString();
    }

    if (!constructName) {
        dataFiles = Settings::instance()->dataFiles(fileType);
    }

    QString fileName;
    for (DataFile dataFile : dataFiles) {
        QString loadFileName = dataFile.saveFileName("", season);
        QFileInfo fi(show->dir().filePath(loadFileName));
        if (fi.isFile() || constructName) {
            fileName = show->dir().filePath(loadFileName);
            break;
        }
    }
    return fileName;
}

QString saveDataFiles(mediaelch::DirectoryPath basePath,
    QString fileName,
    const QVector<DataFile>& dataFiles,
    bool constructName)
{
    for (DataFile dataFile : dataFiles) {
        QString file = dataFile.saveFileName(fileName);
        QFileInfo pFi(basePath.filePath(file));
        if (pFi.isFile() || constructName) {
            return basePath.filePath(file);
        }
    }
    return {};
}

QString
KodiXml::imageFileName(const TvShowEpisode* episode, ImageType type, QVector<DataFile> dataFiles, bool constructName)
{
    DataFileType fileType;
    switch (type) {
    case ImageType::TvShowEpisodeThumb: fileType = DataFileType::TvShowEpisodeThumb; break;
    default: return "";
    }

    if (episode->files().isEmpty()) {
        return "";
    }
    QFileInfo fi(episode->files().first().toString());

    if (helper::isBluRay(episode->files().first().toString()) || helper::isDvd(episode->files().first().toString())) {
        QDir dir = fi.dir();
        dir.cdUp();
        fi.setFile(dir.absolutePath() + "/thumb.jpg");
        return fi.exists() ? fi.absoluteFilePath() : "";
    }

    if (helper::isDvd(episode->files().at(0), true)) {
        fi.setFile(fi.dir().absolutePath() + "/thumb.jpg");
        return fi.exists() ? fi.absoluteFilePath() : "";
    }

    if (!constructName) {
        dataFiles = Settings::instance()->dataFiles(fileType);
    }

    return saveDataFiles(mediaelch::DirectoryPath(fi.absolutePath()), fi.fileName(), dataFiles, constructName);
}

bool KodiXml::loadArtist(Artist* artist, QString initialNfoContent)
{
    artist->clear();
    artist->setHasChanged(false);

    QString nfoContent;
    if (initialNfoContent.isEmpty()) {
        QString nfoFile = nfoFilePath(artist);
        if (nfoFile.isEmpty()) {
            return false;
        }

        QFile file(nfoFile);
        if (!file.exists()) {
            return false;
        }
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qCWarning(generic) << "[KodiXml] File" << nfoFile << "could not be opened for reading";
            return false;
        }
        nfoContent = QString::fromUtf8(file.readAll());
        artist->setNfoContent(nfoContent);
        file.close();
    } else {
        nfoContent = initialNfoContent;
    }

    QDomDocument domDoc;
    domDoc.setContent(nfoContent);

    mediaelch::kodi::ArtistXmlReader reader(*artist);
    return reader.parseNfoDom(domDoc);
}

bool KodiXml::loadAlbum(Album* album, QString initialNfoContent)
{
    if (album == nullptr) {
        return false;
    }
    album->clear();
    album->setHasChanged(false);

    QString nfoContent;
    if (initialNfoContent.isEmpty()) {
        QString nfoFile = nfoFilePath(album);
        if (nfoFile.isEmpty()) {
            return false;
        }

        QFile file(nfoFile);
        if (!file.exists()) {
            return false;
        }
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qCWarning(generic) << "[KodiXml] File" << nfoFile << "could not be opened for reading";
            return false;
        }
        nfoContent = QString::fromUtf8(file.readAll());
        album->setNfoContent(nfoContent);
        file.close();
    } else {
        nfoContent = initialNfoContent;
    }

    QDomDocument domDoc;
    domDoc.setContent(nfoContent);

    mediaelch::kodi::AlbumXmlReader reader(*album);
    return reader.parseNfoDom(domDoc);
}

QString KodiXml::imageFileName(const Artist* artist, ImageType type, QVector<DataFile> dataFiles, bool constructName)
{
    DataFileType fileType;
    switch (type) {
    case ImageType::ArtistThumb: fileType = DataFileType::ArtistThumb; break;
    case ImageType::ArtistFanart: fileType = DataFileType::ArtistFanart; break;
    case ImageType::ArtistLogo: fileType = DataFileType::ArtistLogo; break;
    default: return "";
    }

    if (!artist->path().isValid()) {
        return QString();
    }

    if (!constructName) {
        dataFiles = Settings::instance()->dataFiles(fileType);
    }

    return saveDataFiles(artist->path(), "", dataFiles, constructName);
}

QString KodiXml::imageFileName(const Album* album, ImageType type, QVector<DataFile> dataFiles, bool constructName)
{
    DataFileType fileType;
    switch (type) {
    case ImageType::AlbumThumb: fileType = DataFileType::AlbumThumb; break;
    case ImageType::AlbumCdArt: fileType = DataFileType::AlbumCdArt; break;
    default: return "";
    }

    if (!album->path().isValid()) {
        return QString();
    }

    if (!constructName) {
        dataFiles = Settings::instance()->dataFiles(fileType);
    }

    return saveDataFiles(album->path(), "", dataFiles, constructName);
}

QString KodiXml::nfoFilePath(Artist* artist)
{
    if (!artist->path().isValid()) {
        return QString();
    }

    return artist->path().filePath("artist.nfo");
}

QString KodiXml::nfoFilePath(Album* album)
{
    if (!album->path().isValid()) {
        return {};
    }
    return album->path().filePath("album.nfo");
}

bool KodiXml::saveArtist(Artist* artist)
{
    QByteArray xmlContent = getArtistXml(artist);

    if (!artist->path().isValid()) {
        return false;
    }

    artist->setNfoContent(xmlContent);
    m_persistence.music.update(artist);

    QString fileName = nfoFilePath(artist);
    if (fileName.isEmpty()) {
        return false;
    }

    QDir saveFileDir = QFileInfo(fileName).dir();
    if (!saveFileDir.exists()) {
        saveFileDir.mkpath(".");
    }
    {
        QFile file(fileName);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            qCWarning(generic) << "[KodiXml] File could not be opened";
            return false;
        }
        file.write(xmlContent);
        file.close();
    }
    for (const auto imageType : Artist::imageTypes()) {
        DataFileType dataFileType = DataFile::dataFileTypeForImageType(imageType);

        if (artist->imagesToRemove().contains(imageType)) {
            for (DataFile dataFile : Settings::instance()->dataFiles(dataFileType)) {
                QString saveFileName = dataFile.saveFileName(QString());
                if (!saveFileName.isEmpty()) {
                    QFile(artist->path().filePath(saveFileName)).remove();
                }
            }
        }

        if (!artist->rawImage(imageType).isNull()) {
            for (DataFile dataFile : Settings::instance()->dataFiles(dataFileType)) {
                QString saveFileName = dataFile.saveFileName(QString());
                saveFile(artist->path().filePath(saveFileName), artist->rawImage(imageType));
            }
        }
    }

    for (const QString& file : artist->extraFanartsToRemove()) {
        QFile::remove(file);
    }
    QDir dir(artist->path().subDir("extrafanart").toString());
    if (!dir.exists() && !artist->extraFanartImagesToAdd().isEmpty()) {
        QDir(artist->path().toString()).mkdir("extrafanart");
    }
    for (const QByteArray& img : artist->extraFanartImagesToAdd()) {
        int num = 1;
        while (QFileInfo::exists(dir.absolutePath() + "/" + QString("fanart%1.jpg").arg(num))) {
            ++num;
        }
        saveFile(dir.absolutePath() + "/" + QString("fanart%1.jpg").arg(num), img);
    }

    return true;
}

bool KodiXml::saveAlbum(Album* album)
{
    QByteArray xmlContent = getAlbumXml(album);

    if (!album->path().isValid()) {
        return false;
    }

    album->setNfoContent(xmlContent);
    m_persistence.music.update(album);

    QString nfoFileName = nfoFilePath(album);
    if (nfoFileName.isEmpty()) {
        return false;
    }

    QDir saveFileDir = QFileInfo(nfoFileName).dir();
    if (!saveFileDir.exists()) {
        saveFileDir.mkpath(".");
    }
    QFile nfo(nfoFileName);
    if (!nfo.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qCWarning(generic) << "[KodiXml] File could not be opened";
        return false;
    }
    nfo.write(xmlContent);
    nfo.close();

    for (const auto imageType : Album::imageTypes()) {
        DataFileType dataFileType = DataFile::dataFileTypeForImageType(imageType);

        if (album->imagesToRemove().contains(imageType)) {
            for (DataFile dataFile : Settings::instance()->dataFiles(dataFileType)) {
                QString saveFileName = dataFile.saveFileName(QString());
                if (!saveFileName.isEmpty()) {
                    QFile(album->path().filePath(saveFileName)).remove();
                }
            }
        }

        if (!album->rawImage(imageType).isNull()) {
            for (DataFile dataFile : Settings::instance()->dataFiles(dataFileType)) {
                QString saveFileName = dataFile.saveFileName(QString());
                saveFile(album->path().filePath(saveFileName), album->rawImage(imageType));
            }
        }
    }

    if (album->bookletModel()->hasChanged()) {
        QDir dir(album->path().subDir("booklet").toString());
        if (!dir.exists()) {
            QDir(album->path().toString()).mkdir("booklet");
        }

        // TODO: This coding is broken!
        //   It originally went through all images, and deleted those that marked "delete".
        //   Then it went through _all remaining images_ and saved them with a new name.
        //   If an image in the middle was removed, the file after it would take its place.
        //   But the "last" image, which was not marked to be deleted, but was renamed,
        //   still remained, creating duplicates in MediaElch.
        //   We can't simply rename (yet), because that would require e.g. sorting of filenames, etc.

        // \todo: get filename from settings
        for (Image* image : album->bookletModel()->images()) {
            if (!image->deletion()) {
                image->load(); // load to get binary
            }
            if (image->filePath().isValid()) { // TODO: `image->deletion() &&`
                QFile::remove(image->filePath().toString());
            }
        }
        int bookletNum = 1;
        for (Image* image : album->bookletModel()->images()) {
            if (!image->deletion()) {
                QString imageFileName = "booklet" + QString("%1").arg(bookletNum, 2, 10, QChar('0')) + ".jpg";
                QString imageFilePath = album->path().subDir("booklet").filePath(imageFileName);
                QFile file(imageFilePath);
                if (file.open(QIODevice::WriteOnly)) {
                    file.write(image->rawData());
                    file.close();
                }
                bookletNum++;
            }
        }
    }

    return true;
}

QByteArray KodiXml::getArtistXml(Artist* artist)
{
    auto version = m_settings.kodiVersion();
    auto writer = std::make_unique<mediaelch::kodi::ArtistXmlWriterGeneric>(version, *artist);
    writer->setWriteThumbUrlsToNfo(Settings::instance()->advanced()->writeThumbUrlsToNfo());
    return writer->getArtistXml();
}

QByteArray KodiXml::getAlbumXml(Album* album)
{
    auto version = m_settings.kodiVersion();
    auto writer = std::make_unique<mediaelch::kodi::AlbumXmlWriterGeneric>(version, *album);
    writer->setWriteThumbUrlsToNfo(Settings::instance()->advanced()->writeThumbUrlsToNfo());
    return writer->getAlbumXml();
}

void KodiXml::writeStringsAsOneTagEach(QXmlStreamWriter& xml, const QString& name, const QStringList& list)
{
    for (const QString& item : list) {
        xml.writeTextElement(name, item);
    }
}

void KodiXml::loadBooklets(Album* album)
{
    // \todo: get filename from settings
    if (!album->bookletModel()->images().isEmpty()) {
        return;
    }

    QDir dir(album->path().subDir("booklet").toString());
    QStringList filters{"*.jpg", "*.jpeg", "*.JPEG", "*.Jpeg", "*.JPeg"};
    for (const QString& file : dir.entryList(filters, QDir::Files | QDir::NoDotAndDotDot, QDir::Name)) {
        auto* img = new Image;
        img->setFilePath(mediaelch::FilePath(dir.path() + "/" + file));
        album->bookletModel()->addImage(img);
    }
    album->bookletModel()->setHasChanged(false);
}
