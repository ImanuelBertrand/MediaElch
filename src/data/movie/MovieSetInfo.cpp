#include "data/movie/MovieSetInfo.h"

#include <utility>

MovieSetInfo MovieSetInfo::renamedTo(QString newName) const
{
    MovieSetInfo renamed = *this;
    renamed.name = std::move(newName);
    return renamed;
}

bool MovieSetInfo::operator==(const MovieSetInfo& other) const
{
    return tmdbId == other.tmdbId && name == other.name && overview == other.overview;
}

bool MovieSetInfo::operator!=(const MovieSetInfo& other) const
{
    return !(*this == other);
}
