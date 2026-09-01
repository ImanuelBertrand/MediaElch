#pragma once

#include "settings/DataFile.h"
#include "settings/Settings.h"
#include "third_party/catch2/catch.hpp"

namespace test {

/// \brief Gives Settings the data file list a real MediaElch has, and takes it away again.
/// \details Artwork file names are user-configurable, so KodiXml reads them out of
///          Settings -- and no test binary calls Settings::loadSettings(), so that list
///          is empty and every artwork path is a silent no-op.  A test written without
///          this passes because nothing happens, which is the opposite of what it
///          claims to show.
///
///          The record paths never needed it: "set.nfo" is Kodi's fixed name for that
///          file, so KodiXml::movieSetNfoFileName() builds its DataFile itself instead
///          of reading one out of Settings.
class DataFileGuard
{
public:
    DataFileGuard()
    {
        // Checked rather than assumed.  If a test binary ever does load real settings,
        // restoring an empty list below would throw the user's own file names away.
        REQUIRE(Settings::instance()->dataFiles(DataFileType::MovieSetPoster).isEmpty());
        Settings::instance()->setDataFiles(Settings::instance()->dataFilesFrodo());
    }
    ~DataFileGuard() { Settings::instance()->setDataFiles({}); }
    DataFileGuard(const DataFileGuard&) = delete;
    DataFileGuard& operator=(const DataFileGuard&) = delete;
};

} // namespace test
