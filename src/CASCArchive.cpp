#include <CASCArchive.hpp>

#include <ClientData.hpp>
#include <Exception.hpp>
#include <CascLib.h>
#include <array>
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string>


using namespace BlizzardArchive::Archive;

namespace
{
  bool casc_open_file_with_wow_locale_fallback(HANDLE storage, const void* file_name, DWORD open_flags,
                                               bool neutral_locale_first, HANDLE* file_handle,
                                               std::uint32_t& last_error)
  {
    if (neutral_locale_first)
    {
      if (CascOpenFile(storage, file_name, CASC_LOCALE_NONE, open_flags, file_handle))
      {
        last_error = ERROR_SUCCESS;
        return true;
      }

      last_error = GetCascError();
    }

    static constexpr std::array<DWORD, 4> locale_masks =
    {
      CASC_LOCALE_ALL_WOW,
      CASC_LOCALE_ENUS,
      CASC_LOCALE_NONE,
      CASC_LOCALE_ALL
    };

    for (auto const locale_mask : locale_masks)
    {
      if (CascOpenFile(storage, file_name, locale_mask, open_flags, file_handle))
      {
        last_error = ERROR_SUCCESS;
        return true;
      }

      last_error = GetCascError();
    }

    return false;
  }

  std::string wow_casc_path(std::string path)
  {
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c)
    {
      return static_cast<char>(std::tolower(c));
    });

    std::replace(path.begin(), path.end(), '/', '\\');
    return path;
  }

  std::string describe_find_data(CASC_FIND_DATA const& find_data)
  {
    std::ostringstream stream;
    stream << "name=" << find_data.szFileName
           << ", fdid=" << find_data.dwFileDataId
           << ", size=" << find_data.FileSize
           << ", locale=0x" << std::hex << find_data.dwLocaleFlags
           << ", content=0x" << find_data.dwContentFlags << std::dec
           << ", available=" << find_data.bFileAvailable
           << ", name_type=" << static_cast<int>(find_data.NameType);
    return stream.str();
  }

  std::string preload_wow_root_listfile(HANDLE storage, BlizzardArchive::Listfile::Listfile* listfile)
  {
    std::ostringstream diagnostics;

    if (!storage || !listfile || listfile->csvPath().empty() || !std::filesystem::exists(listfile->csvPath()))
    {
      diagnostics << "listfile preload skipped"
                  << " storage=" << (storage ? "yes" : "no")
                  << " listfile=" << (listfile ? "yes" : "no");
      if (listfile)
        diagnostics << " csv=" << listfile->csvPath()
                    << " exists=" << (std::filesystem::exists(listfile->csvPath()) ? "yes" : "no");
      return diagnostics.str();
    }

    DWORD features = 0;
    if (CascGetStorageInfo(storage, CascStorageFeatures, &features, sizeof(features), nullptr))
      diagnostics << "features=0x" << std::hex << features << std::dec << "; ";
    else
      diagnostics << "features_error=" << GetCascError() << "; ";

    char path_product[MAX_PATH * 2] = {};
    if (CascGetStorageInfo(storage, CascStoragePathProduct, path_product, sizeof(path_product), nullptr))
      diagnostics << "path_product=" << path_product << "; ";
    else
      diagnostics << "path_product_error=" << GetCascError() << "; ";

    diagnostics << "csv=" << listfile->csvPath() << "; ";

    CASC_FIND_DATA find_data = {};
    HANDLE find_handle = CascFindFirstFile(storage, "*", &find_data, listfile->csvPath().c_str());
    if (find_handle)
    {
      diagnostics << "preload_first={" << describe_find_data(find_data) << "}; ";

      int sampled = 0;
      do
      {
        std::string name = find_data.szFileName;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c)
        {
          return static_cast<char>(std::tolower(c));
        });

        if (name.find("world\\maps\\azeroth\\azeroth.wdt") != std::string::npos
            || name.find("world/maps/azeroth/azeroth.wdt") != std::string::npos
            || find_data.dwFileDataId == 775971)
        {
          diagnostics << "azeroth_in_scan={" << describe_find_data(find_data) << "}; ";
          break;
        }

        sampled++;
      } while (sampled < 25000 && CascFindNextFile(find_handle, &find_data));

      if (sampled >= 25000)
        diagnostics << "azeroth_scan_limit=25000; ";

      CascFindClose(find_handle);
    }
    else
    {
      diagnostics << "preload_find_error=" << GetCascError() << "; ";
    }

    CASC_FIND_DATA azeroth_data = {};
    HANDLE azeroth_handle = CascFindFirstFile(storage, "world\\maps\\azeroth\\azeroth.wdt", &azeroth_data, listfile->csvPath().c_str());
    if (azeroth_handle)
    {
      diagnostics << "azeroth_direct_find={" << describe_find_data(azeroth_data) << "}; ";
      CascFindClose(azeroth_handle);
    }
    else
    {
      diagnostics << "azeroth_direct_find_error=" << GetCascError() << "; ";
    }

    return diagnostics.str();
  }
}

CASCArchive::CASCArchive(std::string const& path
                         , std::string const& cache_path
                         , Locale locale
                         , OpenMode open_mode
                         , bool neutral_locale_first
                         , Listfile::Listfile* listfile)
  : BaseArchive(path, locale, listfile)
  , _neutral_locale_first(neutral_locale_first)
{
  switch (open_mode)
  {
    case OpenMode::REMOTE:
    {
      CASC_OPEN_STORAGE_ARGS args = { sizeof(CASC_OPEN_STORAGE_ARGS) };
      args.szLocalPath = cache_path.c_str();
      args.szCodeName = "wow";
      args.szRegion = "us";
      args.PfnProgressCallback = nullptr;
      args.PtrProgressParam = nullptr;
      args.PfnProductCallback = nullptr;
      args.PtrProductParam = nullptr;
      args.dwLocaleMask = _neutral_locale_first ? CASC_LOCALE_NONE : CASC_LOCALE_ALL_WOW;
      args.szBuildKey = nullptr;
      args.szCdnHostUrl = path.c_str();


      if (!CascOpenStorageEx(nullptr, &args, true, &_handle))
      {
        throw Exceptions::Archive::ArchiveOpenError("Error opening CASC archive: " + path
                                                    + ". Error code: " + std::to_string(GetCascError()));
      }

      break;
    }
    case OpenMode::LOCAL:
    {
      CASC_OPEN_STORAGE_ARGS args = { sizeof(CASC_OPEN_STORAGE_ARGS) };
      args.szLocalPath = path.c_str();
      args.szCodeName = "wow";
      args.szRegion = nullptr;
      args.PfnProgressCallback = nullptr;
      args.PtrProgressParam = nullptr;
      args.PfnProductCallback = nullptr;
      args.PtrProductParam = nullptr;
      args.dwLocaleMask = _neutral_locale_first ? CASC_LOCALE_NONE : CASC_LOCALE_ALL_WOW;
      args.szBuildKey = nullptr;
      args.szCdnHostUrl = nullptr;


      if (!CascOpenStorageEx(nullptr, &args, false, &_handle))
      {
        throw Exceptions::Archive::ArchiveOpenError("Error opening CASC archive: " + path
                                                    + ". Error code: " + std::to_string(GetCascError()));
      }

      break;
    }
  }

  _diagnostics = preload_wow_root_listfile(_handle, listfile);
}

bool CASCArchive::openFile(Listfile::FileKey const& file_key, Locale locale, HANDLE* file_handle) const
{
  assert(file_key.hasFileDataID() || file_key.hasFilepath());

  if (file_key.hasFileDataID())
  {
    assert(file_key.fileDataID());

    if (casc_open_file_with_wow_locale_fallback(_handle, CASC_FILE_DATA_ID(file_key.fileDataID()), CASC_OPEN_BY_FILEID,
                                                _neutral_locale_first, file_handle, _last_error))
      return true;
  }

  if (file_key.hasFilepath())
  {
    auto const casc_path = wow_casc_path(file_key.filepath());
    if (casc_open_file_with_wow_locale_fallback(_handle, casc_path.c_str(), CASC_OPEN_BY_NAME,
                                                _neutral_locale_first, file_handle, _last_error))
      return true;

    auto const wow_path = BlizzardArchive::ClientData::normalizeFilenameWoW(file_key.filepath());
    if (wow_path != casc_path && casc_open_file_with_wow_locale_fallback(
          _handle, wow_path.c_str(), CASC_OPEN_BY_NAME, _neutral_locale_first, file_handle, _last_error))
      return true;

    auto const file_data_id = _listfile->getFileDataID(file_key.filepath());
    if (file_data_id)
    {
      if (casc_open_file_with_wow_locale_fallback(_handle, CASC_FILE_DATA_ID(file_data_id), CASC_OPEN_BY_FILEID,
                                                  _neutral_locale_first, file_handle, _last_error))
        return true;
    }
  }

  return false;
}

bool CASCArchive::readFile(HANDLE file_handle, char* buffer, std::size_t buf_size) const
{
  assert(file_handle);
  return CascReadFile(file_handle, buffer, static_cast<DWORD>(buf_size), nullptr);
}

bool CASCArchive::closeFile(HANDLE file_handle) const
{
  assert(file_handle);
  return CascCloseFile(file_handle);
}

std::uint64_t CASCArchive::getFileSize(HANDLE file_handle) const
{
  assert(file_handle);
  unsigned long long size = 0;

  if (!CascGetFileSize64(file_handle, static_cast<PULONGLONG>(&size)))
    return 0;

  return size;
}

bool CASCArchive::exists(Listfile::FileKey const& file_key, Locale locale) const
{
  HANDLE file_handle = nullptr;

  assert(file_key.hasFileDataID() || file_key.hasFilepath());

  if (file_key.hasFileDataID())
  {
    assert(file_key.fileDataID());

    if (casc_open_file_with_wow_locale_fallback(_handle, CASC_FILE_DATA_ID(file_key.fileDataID()), CASC_OPEN_BY_FILEID,
                                                _neutral_locale_first, &file_handle, _last_error))
    {
      CascCloseFile(file_handle);
      return true;
    }
  }

  if (file_key.hasFilepath())
  {
    auto const casc_path = wow_casc_path(file_key.filepath());
    if (casc_open_file_with_wow_locale_fallback(_handle, casc_path.c_str(), CASC_OPEN_BY_NAME,
                                                _neutral_locale_first, &file_handle, _last_error))
    {
      CascCloseFile(file_handle);
      return true;
    }

    auto const wow_path = BlizzardArchive::ClientData::normalizeFilenameWoW(file_key.filepath());
    if (wow_path != casc_path && casc_open_file_with_wow_locale_fallback(
          _handle, wow_path.c_str(), CASC_OPEN_BY_NAME, _neutral_locale_first, &file_handle, _last_error))
    {
      CascCloseFile(file_handle);
      return true;
    }

    auto const file_data_id = _listfile->getFileDataID(file_key.filepath());
    if (file_data_id && casc_open_file_with_wow_locale_fallback(
          _handle, CASC_FILE_DATA_ID(file_data_id), CASC_OPEN_BY_FILEID,
          _neutral_locale_first, &file_handle, _last_error))
    {
      CascCloseFile(file_handle);
      return true;
    }
  }

  return false;

}

std::string CASCArchive::lastErrorString() const
{
  if (_last_error == ERROR_SUCCESS && _diagnostics.empty())
    return {};

  std::string error = _last_error == ERROR_SUCCESS ? "CASC ok" : "CASC error " + std::to_string(_last_error);
  if (!_diagnostics.empty())
    error += "; " + _diagnostics;
  return error;
}

CASCArchive::~CASCArchive()
{
  if (_handle)
    CascCloseStorage(_handle);
}
