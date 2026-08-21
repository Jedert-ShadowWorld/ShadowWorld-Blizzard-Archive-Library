#include <ClientFile.hpp>
#include <Exception.hpp>
#include <fstream>
#include <iostream>
#include <system_error>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <unordered_map>

using namespace BlizzardArchive;

namespace
{
  constexpr std::size_t md20_version_offset = 4;
  constexpr std::size_t md20_n_textures_offset = 80;
  constexpr std::size_t md20_ofs_textures_offset = 84;
  constexpr std::size_t md20_texture_def_size = 16;
  constexpr std::size_t md20_texture_type_offset = 0;
  constexpr std::size_t md20_texture_name_len_offset = 8;
  constexpr std::size_t md20_texture_name_ofs_offset = 12;

  std::mutex modern_file_id_mutex;
  std::unordered_map<std::string, std::uint32_t> modern_file_ids;

  std::string normalize_path(std::string path)
  {
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c)
    {
      if (c == '\\')
        return '/';
      return static_cast<char>(std::tolower(c));
    });
    return path;
  }

  void remember_modern_file_id(std::string const& filepath, std::uint32_t file_data_id)
  {
    if (filepath.empty() || !file_data_id)
      return;

    std::lock_guard<std::mutex> lock(modern_file_id_mutex);
    modern_file_ids[normalize_path(filepath)] = file_data_id;
  }

  std::uint32_t remembered_modern_file_id(std::string const& filepath)
  {
    if (filepath.empty())
      return 0;

    std::lock_guard<std::mutex> lock(modern_file_id_mutex);
    auto const it = modern_file_ids.find(normalize_path(filepath));
    return it == modern_file_ids.end() ? 0u : it->second;
  }

  void restore_registered_file_id(Listfile::FileKey& file_key)
  {
    if (file_key.hasFileDataID() || !file_key.hasFilepath())
      return;

    if (auto const file_data_id = remembered_modern_file_id(file_key.filepath()))
      file_key.setFileDataID(file_data_id);
  }

  bool has_m2_extension(Listfile::FileKey const& file_key)
  {
    if (!file_key.hasFilepath())
      return false;

    auto const& path = file_key.filepath();
    if (path.size() < 3)
      return false;

    auto const n = path.size();
    return path[n - 3] == '.'
      && (path[n - 2] == 'm' || path[n - 2] == 'M')
      && path[n - 1] == '2';
  }

  bool read_u32(std::vector<char> const& buffer, std::size_t offset, std::uint32_t& value)
  {
    if (offset + sizeof(value) > buffer.size())
      return false;
    std::memcpy(&value, buffer.data() + offset, sizeof(value));
    return true;
  }

  bool write_u32(std::vector<char>& buffer, std::size_t offset, std::uint32_t value)
  {
    if (offset + sizeof(value) > buffer.size())
      return false;
    std::memcpy(buffer.data() + offset, &value, sizeof(value));
    return true;
  }

  void inject_txid_paths(std::vector<char>& md20,
                         std::vector<std::uint32_t> const& texture_file_data_ids,
                         ClientData* client_data)
  {
    if (!client_data || texture_file_data_ids.empty())
      return;

    std::uint32_t n_textures = 0;
    std::uint32_t ofs_textures = 0;
    if (!read_u32(md20, md20_n_textures_offset, n_textures)
        || !read_u32(md20, md20_ofs_textures_offset, ofs_textures))
      return;

    if (ofs_textures > md20.size())
      return;

    auto const available_defs = (md20.size() - ofs_textures) / md20_texture_def_size;
    auto const count = std::min<std::size_t>(
      { static_cast<std::size_t>(n_textures), texture_file_data_ids.size(), available_defs });

    for (std::size_t i = 0; i < count; ++i)
    {
      auto const def = static_cast<std::size_t>(ofs_textures) + i * md20_texture_def_size;

      std::uint32_t type = 0;
      std::uint32_t name_len = 0;
      if (!read_u32(md20, def + md20_texture_type_offset, type)
          || !read_u32(md20, def + md20_texture_name_len_offset, name_len))
        continue;

      if (type != 0 || name_len != 0 || texture_file_data_ids[i] == 0)
        continue;

      auto path = client_data->listfile()->getPath(texture_file_data_ids[i]);
      if (path.empty())
        continue;

      remember_modern_file_id(path, texture_file_data_ids[i]);

      auto const name_ofs = static_cast<std::uint32_t>(md20.size());
      auto const stored_len = static_cast<std::uint32_t>(path.size() + 1);

      md20.insert(md20.end(), path.begin(), path.end());
      md20.push_back('\0');

      write_u32(md20, def + md20_texture_name_len_offset, stored_len);
      write_u32(md20, def + md20_texture_name_ofs_offset, name_ofs);
    }
  }

  void register_sfid_paths(Listfile::FileKey const& model_key,
                           std::vector<std::uint32_t> const& skin_file_data_ids,
                           ClientData* client_data)
  {
    if (!model_key.hasFilepath() || skin_file_data_ids.empty())
      return;

    auto const& model_path = model_key.filepath();
    if (model_path.size() < 3)
      return;

    auto const base = model_path.substr(0, model_path.size() - 3);
    for (std::size_t i = 0; i < skin_file_data_ids.size(); ++i)
    {
      auto const file_data_id = skin_file_data_ids[i];
      if (!file_data_id)
        continue;

      // Prefer the listfile's canonical name when available. Also register the
      // legacy Noggit-generated 00.skin/01.skin name because Model::initCommon
      // opens views by that name.
      if (client_data && client_data->listfile())
      {
        auto const canonical = client_data->listfile()->getPath(file_data_id);
        if (!canonical.empty())
          remember_modern_file_id(canonical, file_data_id);
      }

      char suffix[16] = {};
      std::snprintf(suffix, sizeof(suffix), "%02zu.skin", i);
      remember_modern_file_id(base + suffix, file_data_id);
    }
  }

  bool unwrap_chunked_m2_md21(Listfile::FileKey const& file_key,
                              std::vector<char>& buffer,
                              std::vector<std::uint32_t>& texture_file_data_ids,
                              std::vector<std::uint32_t>& skin_file_data_ids,
                              ClientData* client_data)
  {
    if (buffer.size() < 12)
      return false;

    std::size_t pos = 0;
    std::size_t md21_payload = 0;
    std::uint32_t md21_size = 0;

    while (pos + 8 <= buffer.size())
    {
      auto const* id = buffer.data() + pos;

      std::uint32_t size = 0;
      std::memcpy(&size, buffer.data() + pos + 4, sizeof(size));

      auto const payload = pos + 8;
      if (size > buffer.size() - payload)
        return false;

      if (id[0] == 'T' && id[1] == 'X' && id[2] == 'I' && id[3] == 'D')
      {
        if ((size % sizeof(std::uint32_t)) == 0)
        {
          auto const count = size / sizeof(std::uint32_t);
          texture_file_data_ids.resize(count);
          if (count)
            std::memcpy(texture_file_data_ids.data(), buffer.data() + payload, size);
        }
      }
      else if (id[0] == 'S' && id[1] == 'F' && id[2] == 'I' && id[3] == 'D')
      {
        if ((size % sizeof(std::uint32_t)) == 0)
        {
          auto const count = size / sizeof(std::uint32_t);
          skin_file_data_ids.resize(count);
          if (count)
            std::memcpy(skin_file_data_ids.data(), buffer.data() + payload, size);
        }
      }
      else if (id[0] == 'M' && id[1] == 'D' && id[2] == '2' && id[3] == '1')
      {
        md21_payload = payload;
        md21_size = size;
      }

      pos = payload + size;
    }

    if (!md21_payload || md21_size < 8 || md21_size > buffer.size() - md21_payload)
      return false;

    auto const* body = buffer.data() + md21_payload;
    if (!(body[0] == 'M' && body[1] == 'D' && body[2] == '2' && body[3] == '0'))
      return false;

    std::vector<char> md20(body, body + md21_size);

    std::uint32_t version = 0;
    if (read_u32(md20, md20_version_offset, version) && version == 274)
      write_u32(md20, md20_version_offset, 272);

    inject_txid_paths(md20, texture_file_data_ids, client_data);
    register_sfid_paths(file_key, skin_file_data_ids, client_data);

    buffer.swap(md20);
    return true;
  }

  void adapt_modern_m2_buffer(Listfile::FileKey const& file_key,
                              std::vector<char>& buffer,
                              std::vector<std::uint32_t>& texture_file_data_ids,
                              std::vector<std::uint32_t>& skin_file_data_ids,
                              ClientData* client_data)
  {
    texture_file_data_ids.clear();
    skin_file_data_ids.clear();
    if (has_m2_extension(file_key))
      unwrap_chunked_m2_md21(file_key, buffer, texture_file_data_ids, skin_file_data_ids, client_data);
  }
}

void ClientFile::registerModernFileDataID(std::string const& filepath, std::uint32_t file_data_id)
{
  remember_modern_file_id(filepath, file_data_id);
}

ClientFile::ClientFile(Listfile::FileKey const& file_key, ClientData* client_data)
  : _file_key(file_key)
  , _eof(true)
  , _pointer(0)
  , _external(false)
{
  if (client_data->version() != ClientVersion::WOTLK)
  {
    // The placement/SFID bridge is authoritative when present. Only then ask
    // the listfile to fill whichever half of the key is still missing.
    restore_registered_file_id(_file_key);
    _file_key.deduceOtherComponent(client_data->listfile());
    restore_registered_file_id(_file_key);
  }

  _disk_path = client_data->getDiskPath(_file_key);

  std::ifstream input(_disk_path.string(), std::ios_base::binary | std::ios_base::in);
  if (input.is_open())
  {
    _external = true;
    _eof = false;

    input.seekg(0, std::ios::end);
    _buffer.resize(input.tellg());
    input.seekg(0, std::ios::beg);
    input.read(_buffer.data(), _buffer.size());
    input.close();
    adapt_modern_m2_buffer(_file_key, _buffer, _m2_texture_file_data_ids, _m2_skin_file_data_ids, client_data);
    return;
  }

  if (client_data->readFile(_file_key, _buffer))
  {
    _eof = false;
    adapt_modern_m2_buffer(_file_key, _buffer, _m2_texture_file_data_ids, _m2_skin_file_data_ids, client_data);
    return;
  }

  throw Exceptions::FileReadFailedError(
    "File '"
    + (_file_key.hasFilepath() ? _file_key.filepath() : std::to_string(_file_key.fileDataID()))
    + "' does not exist or some other error occured.");
}

ClientFile::ClientFile(Listfile::FileKey const& file_key, ClientData* client_data, NEW_FILE_T)
: _file_key(file_key)
, _eof(true)
, _pointer(0)
, _external(false)
{
  if (client_data->version() != ClientVersion::WOTLK)
  {
    restore_registered_file_id(_file_key);
    _file_key.deduceOtherComponent(client_data->listfile());
    restore_registered_file_id(_file_key);
  }

  _disk_path = client_data->getDiskPath(_file_key);
}

std::size_t ClientFile::read(void* dest, size_t bytes)
{
  if (_eof || !bytes)
    return 0;

  size_t rpos = _pointer + bytes;
  if (rpos > _buffer.size()) {
    bytes = _buffer.size() - _pointer;
    _eof = true;
  }

  std::memcpy(dest, &(_buffer[_pointer]), bytes);
  _pointer = rpos;
  return bytes;
}

bool ClientFile::isEof() const { return _eof; }

void ClientFile::seek(std::size_t offset)
{
  _pointer = offset;
  _eof = (_pointer >= _buffer.size());
}

void ClientFile::seekRelative(std::size_t offset)
{
  _pointer += offset;
  _eof = (_pointer >= _buffer.size());
}

void ClientFile::close() { _eof = true; }

std::size_t ClientFile::getSize() const { return _buffer.size(); }
std::size_t ClientFile::getPos() const { return _pointer; }
char const* ClientFile::getBuffer() const { return _buffer.data(); }
char const* ClientFile::getPointer() const { return _buffer.data() + _pointer; }

void ClientFile::save()
{
  std::cout << "Saving file to: " << _disk_path << std::endl;

  auto const directory_name(_disk_path.parent_path());
  std::error_code ec;
  std::filesystem::create_directories(directory_name, ec);

  if (ec)
  {
    std::cout << "Error: Creating directory \"" << directory_name << "\" failed: " << ec << ". Saving is highly likely to fail." << std::endl;
  }

  std::ofstream output(_disk_path.string(), std::ios_base::binary | std::ios_base::out);
  if (output.is_open())
  {
    output.write(_buffer.data(), _buffer.size());
    output.close();
    _external = true;
  }
  else
  {
    std::cout << "Error saving file to: " << _disk_path << std::endl;
  }
}
