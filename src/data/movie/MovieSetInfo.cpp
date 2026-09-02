#include "data/movie/MovieSetInfo.h"

#include <utility>

MovieSetInfo MovieSetInfo::renamedTo(QString newName) const
{
    MovieSetInfo renamed = *this;
    renamed.name = std::move(newName);
    return renamed;
}
