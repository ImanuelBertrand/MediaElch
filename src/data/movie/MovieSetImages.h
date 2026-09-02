#pragma once

#include "globals/Globals.h"
#include "utils/Meta.h"

#include <QByteArray>
#include <QMap>
#include <QSet>
#include <QVector>

class MovieSet;

/// \brief The artwork of a movie set.
/// \details Follows MovieImages: images are stored as the raw bytes that were
///          downloaded, keyed by their ImageType, so that set artwork is written
///          verbatim instead of being re-encoded to JPEG.
///
///          Which types a set supports is data (see supportedImageTypes()), not structure,
///          so further Kodi set art types can be added without touching the members.
class MovieSetImages
{
public:
    explicit MovieSetImages(MovieSet& set);

    /// \brief All image types a movie set can hold.
    ELCH_NODISCARD static const QVector<ImageType>& supportedImageTypes();
    /// \brief Whether \p imageType is artwork of a set (as opposed to a movie).
    ELCH_NODISCARD static bool isSupportedImageType(ImageType imageType);

    ELCH_NODISCARD QByteArray image(ImageType imageType) const;
    ELCH_NODISCARD bool hasImage(ImageType imageType) const;
    ELCH_NODISCARD bool imageHasChanged(ImageType imageType) const;
    ELCH_NODISCARD QSet<ImageType> imagesToRemove() const;

    void setImage(ImageType imageType, QByteArray image);
    void setHasImage(ImageType imageType, bool has);
    void removeImage(ImageType imageType);
    /// \brief Drops the stored bytes to free memory; keeps the has-image flags.
    void clearImages();

private:
    QMap<ImageType, QByteArray> m_images;
    QMap<ImageType, bool> m_hasImage;
    QMap<ImageType, bool> m_hasImageChanged;
    QSet<ImageType> m_imagesToRemove;

    MovieSet& m_set;
};
