#include <Listfile.hpp>
#include <Exception.hpp>
#include <ClientData.hpp>
#include <fstream>
#include <sstream>
#include <cstdint>
#include <mutex>
#include <unordered_map>

using namespace BlizzardArchive::Listfile;

namespace
{
  // CASC listfiles can contain multiple FileDataIDs for the same historical
  // pathname. When a modern container gives us an authoritative FileDataID and
  // we resolve it to a path, remember that exact pairing so a later legacy-style
  // pathname lookup does not silently select a stale duplicate ID.
  std::mutex resolved_id_mutex;
  std::unordered_map<std::string, std::uint32_t> resolved_path_ids;
}

FileKey::FileKey()
: _file_data_id(0)
, _file_path("")
{
}

FileKey::FileKey(std::string const& filepath, std::uint32_t file_data_id)
: _file_data_id(file_data_id)
, _file_path(ClientData::normalizeFilenameInternal(filepath))
{}

FileKey::FileKey(std::string const& filepath, Listfile* listfile)
  : _file_path(ClientData::normalizeFilenameInternal(filepath))
{
  if (listfile)
  {
    deduceOtherComponent(listfile);
  }

}

FileKey::FileKey(const char* filepath, Listfile* listfile)
: _file_path(ClientData::normalizeFilenameInternal(filepath))
{
  if (listfile)
  {
    deduceOtherComponent(listfile);
  }
}

FileKey::FileKey(const char* filepath, std::uint32_t file_data_id)
  : _file_path(ClientData::normalizeFilenameInternal(filepath))
  , _file_data_id(file_data_id)
{}


FileKey::FileKey(std::uint32_t file_data_id, Listfile* listfile)
  : _file_data_id(file_data_id)
{
  if (listfile)
  {
    deduceOtherComponent(listfile);
  }
}

void Listfile::initFromCSV(std::string const& listfile_path)
{
  std::ifstream fstream;
  fstream.open(listfile_path);

  if (!fstream.is_open())
  {
    throw Exceptions::Listfile::ListfileNotFoundError();
  }
  else
  {
    std::string line = "";
    while (std::getline(fstream, line))
    {
      std::string uid_str;
      std::string filename;
      std::uint32_t uid;

      std::stringstream ss(line);

      getline(ss, uid_str, ';');
      getline(ss, filename);

      uid = std::atoi(uid_str.c_str());

      _path_to_fdid[ClientData::normalizeFilenameInternal(filename)] = uid;
      _fdid_to_path[uid] = ClientData::normalizeFilenameWoW(filename);

    }

  }
}

void Listfile::initFromFileList(std::vector<char> const& file_list_blob)
{
  std::string current;
  for (char c : file_list_blob)
  {
    if (c == '\r')
    {
      continue;
    }
    if (c == '\n')
    {
      _path_to_fdid[ClientData::normalizeFilenameInternal(current)] = 0;
      current.resize(0);
    }
    else
    {
      current += c;
    }
  }

  if (!current.empty())
  {
    _path_to_fdid[ClientData::normalizeFilenameInternal(current)] = 0;
  }
}

void BlizzardArchive::Listfile::Listfile::addFile(std::string const& filepath)
{
  _path_to_fdid[ClientData::normalizeFilenameInternal(filepath)] = 0;
}

std::uint32_t Listfile::getFileDataID(std::string const& filename) const
{
  auto const normalized = ClientData::normalizeFilenameInternal(filename);

  {
    std::lock_guard<std::mutex> lock(resolved_id_mutex);
    auto const resolved = resolved_path_ids.find(normalized);
    if (resolved != resolved_path_ids.end())
      return resolved->second;
  }

  auto it = _path_to_fdid.find(normalized);

  if (it != _path_to_fdid.end())
  {
    return it->second;
  }

  return 0;
}

std::string Listfile::getPath(std::uint32_t file_data_id) const
{
  auto it = _fdid_to_path.find(file_data_id);

  if (it != _fdid_to_path.end())
  {
    auto const normalized = ClientData::normalizeFilenameInternal(it->second);
    {
      std::lock_guard<std::mutex> lock(resolved_id_mutex);
      resolved_path_ids[normalized] = file_data_id;
    }
    return it->second;
  }

  return "";
}

bool FileKey::deduceOtherComponent(const Listfile* listfile)
{
  if (hasFileDataID() && !hasFilepath())
  {
    std::string path = listfile->getPath(fileDataID());

    if (path.empty())
    {
      return false;
    }

    _file_path = path;
    return true;

  }
  else if (hasFilepath() && !hasFileDataID())
  {
    std::uint32_t fdid = listfile->getFileDataID(filepath());

    if (!fdid)
    {
      return false;
    }

    _file_data_id = fdid;
    return true;
  }

  return false;
}

bool FileKey::operator==(const FileKey& rhs) const
{
  if (hasFileDataID() && rhs.hasFileDataID())
  {
    return _file_data_id == rhs.fileDataID();
  }
  else if (hasFilepath() && rhs.hasFilepath())
  {
    return filepath() == rhs.filepath();
  }

  return false;
}

FileKey::FileKey(FileKey&& other) noexcept
{
  std::swap(_file_data_id, other._file_data_id);
  std::swap(_file_path, other._file_path);
}

std::string FileKey::stringRepr() const
{
  return hasFilepath() ? _file_path.value() : std::to_string(_file_data_id);
}

bool FileKey::operator<(const FileKey& rhs) const
{
  if (hasFileDataID() && rhs.hasFileDataID())
  {
    return _file_data_id < rhs.fileDataID();
  }
  else if (hasFilepath() && rhs.hasFilepath())
  {
    return filepath() < rhs.filepath();
  }

  return false;
}

FileKey& FileKey::operator=(FileKey&& other) noexcept
{
  std::swap(_file_data_id, other._file_data_id);
  std::swap(_file_path, other._file_path);
  return *this;
}

FileKey::FileKey(FileKey const& other)
: _file_data_id(other._file_data_id)
, _file_path(other._file_path)
{
}

FileKey& FileKey::operator= (FileKey const& other)
{
  _file_data_id = other._file_data_id;
  _file_path = other._file_path;

  return *this;
}
