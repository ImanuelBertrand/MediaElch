#pragma once

#include "utils/Meta.h"

#include <QString>

namespace mediaelch {
namespace kodi {

/// \brief Port of Kodi's `CUtil::MakeLegalFileName(name, LegalPath::WIN32_COMPAT)` (xbmc/Util.cpp).
///
/// Kodi derives the movie set information folder's name from the set name this way, using the
/// WIN32_COMPAT variant on every platform, so a library written on Linux has to use the identical
/// mapping.  helper::sanitizeFileName() is not a substitute: it maps ':' to a space and drops
/// '?' and '*'.
ELCH_NODISCARD QString makeLegalFileName(QString name);

} // namespace kodi
} // namespace mediaelch
