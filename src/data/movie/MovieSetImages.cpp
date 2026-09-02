#include "data/movie/MovieSetImages.h"

#include "data/movie/MovieSet.h"

#include <utility>

MovieSetImages::MovieSetImages(MovieSet& set) : m_set{set}
{
}

const QVector<ImageType>& MovieSetImages::supportedImageTypes()
{
    // Adding one of the further Kodi set art types is an entry here.
    static const QVector<ImageType> types{ImageType::MovieSetPoster, ImageType::MovieSetBackdrop};
    return types;
}

bool MovieSetImages::isSupportedImageType(ImageType imageType)
{
    return supportedImageTypes().contains(imageType);
}

QByteArray MovieSetImages::image(ImageType imageType) const
{
    return m_images.value(imageType, QByteArray());
}

bool MovieSetImages::hasImage(ImageType imageType) const
{
    return m_hasImage.value(imageType, false);
}

bool MovieSetImages::imageHasChanged(ImageType imageType) const
{
    return m_hasImageChanged.value(imageType, false);
}

QSet<ImageType> MovieSetImages::imagesToRemove() const
{
    return m_imagesToRemove;
}

void MovieSetImages::setImage(ImageType imageType, QByteArray image)
{
    m_images.insert(imageType, std::move(image));
    m_hasImageChanged.insert(imageType, true);
    m_imagesToRemove.remove(imageType);
    setHasImage(imageType, true);
    m_set.setChanged(true);
}

void MovieSetImages::setHasImage(ImageType imageType, bool has)
{
    m_hasImage.insert(imageType, has);
}

void MovieSetImages::removeImage(ImageType imageType)
{
    if (!m_images.value(imageType, QByteArray()).isNull()) {
        // Only downloaded but not yet written: forget it, nothing to delete on disk.
        m_images.remove(imageType);
        m_hasImageChanged.insert(imageType, false);
    } else {
        m_imagesToRemove.insert(imageType);
    }
    setHasImage(imageType, false);
    m_set.setChanged(true);
}

void MovieSetImages::clearImages()
{
    m_images.clear();
    m_hasImageChanged.clear();
}
