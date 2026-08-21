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
      if (c == '\\') return '/';
      return static_cast<char>(std::tolower(c));
    });
    return path;
  }

  bool ends_with_ci(std::string const& path, char const* suffix)
  {
    std::string s(suffix);
    if (path.size() < s.size()) return false;
    auto const start = path.size() - s.size();
    for (std::size_t i = 0; i < s.size(); ++i)
      if (std::tolower(static_cast<unsigned char>(path[start + i])) != std::tolower(static_cast<unsigned char>(s[i]))) return false;
    return true;
  }

  bool is_model_asset(Listfile::FileKey const& key)
  {
    if (!key.hasFilepath()) return false;
    auto const& p = key.filepath();
    return ends_with_ci(p, ".m2") || ends_with_ci(p, ".skin") || ends_with_ci(p, ".blp") || ends_with_ci(p, ".anim");
  }

  void remember_modern_file_id(std::string const& filepath, std::uint32_t file_data_id)
  {
    if (filepath.empty() || !file_data_id) return;
    std::lock_guard<std::mutex> lock(modern_file_id_mutex);
    modern_file_ids[normalize_path(filepath)] = file_data_id;
  }

  std::uint32_t remembered_modern_file_id(std::string const& filepath)
  {
    if (filepath.empty()) return 0;
    std::lock_guard<std::mutex> lock(modern_file_id_mutex);
    auto const it = modern_file_ids.find(normalize_path(filepath));
    return it == modern_file_ids.end() ? 0u : it->second;
  }

  void restore_registered_file_id(Listfile::FileKey& file_key)
  {
    if (file_key.hasFileDataID() || !file_key.hasFilepath()) return;
    if (auto const id = remembered_modern_file_id(file_key.filepath())) file_key.setFileDataID(id);
  }

  bool has_m2_extension(Listfile::FileKey const& file_key)
  {
    return file_key.hasFilepath() && ends_with_ci(file_key.filepath(), ".m2");
  }

  bool read_u32(std::vector<char> const& buffer, std::size_t offset, std::uint32_t& value)
  {
    if (offset + sizeof(value) > buffer.size()) return false;
    std::memcpy(&value, buffer.data() + offset, sizeof(value));
    return true;
  }

  bool write_u32(std::vector<char>& buffer, std::size_t offset, std::uint32_t value)
  {
    if (offset + sizeof(value) > buffer.size()) return false;
    std::memcpy(buffer.data() + offset, &value, sizeof(value));
    return true;
  }

  void inject_txid_paths(std::vector<char>& md20, std::vector<std::uint32_t> const& ids, ClientData* client_data)
  {
    if (!client_data || ids.empty()) return;
    std::uint32_t n = 0, ofs = 0;
    if (!read_u32(md20, md20_n_textures_offset, n) || !read_u32(md20, md20_ofs_textures_offset, ofs) || ofs > md20.size()) return;
    auto const available = (md20.size() - ofs) / md20_texture_def_size;
    auto const count = std::min<std::size_t>({static_cast<std::size_t>(n), ids.size(), available});
    for (std::size_t i = 0; i < count; ++i)
    {
      auto const def = static_cast<std::size_t>(ofs) + i * md20_texture_def_size;
      std::uint32_t type = 0, name_len = 0;
      if (!read_u32(md20, def + md20_texture_type_offset, type) || !read_u32(md20, def + md20_texture_name_len_offset, name_len)) continue;
      if (type != 0 || name_len != 0 || ids[i] == 0) continue;
      auto path = client_data->listfile()->getPath(ids[i]);
      if (path.empty()) continue;
      remember_modern_file_id(path, ids[i]);
      auto const name_ofs = static_cast<std::uint32_t>(md20.size());
      auto const stored_len = static_cast<std::uint32_t>(path.size() + 1);
      md20.insert(md20.end(), path.begin(), path.end());
      md20.push_back('\0');
      write_u32(md20, def + md20_texture_name_len_offset, stored_len);
      write_u32(md20, def + md20_texture_name_ofs_offset, name_ofs);
    }
  }

  void register_sfid_paths(Listfile::FileKey const& model_key, std::vector<std::uint32_t> const& ids, ClientData* client_data)
  {
    if (!model_key.hasFilepath() || ids.empty()) return;
    auto const& model_path = model_key.filepath();
    if (model_path.size() < 3) return;
    auto const base = model_path.substr(0, model_path.size() - 3);
    for (std::size_t i = 0; i < ids.size(); ++i)
    {
      auto const id = ids[i];
      if (!id) continue;
      if (client_data && client_data->listfile())
      {
        auto canonical = client_data->listfile()->getPath(id);
        if (!canonical.empty()) remember_modern_file_id(canonical, id);
      }
      char suffix[16] = {};
      std::snprintf(suffix, sizeof(suffix), "%02zu.skin", i);
      remember_modern_file_id(base + suffix, id);
    }
  }

  bool unwrap_chunked_m2_md21(Listfile::FileKey const& file_key, std::vector<char>& buffer,
                              std::vector<std::uint32_t>& texture_ids, std::vector<std::uint32_t>& skin_ids,
                              ClientData* client_data)
  {
    if (buffer.size() < 12) return false;
    std::size_t pos = 0, md21_payload = 0;
    std::uint32_t md21_size = 0;
    while (pos + 8 <= buffer.size())
    {
      auto const* id = buffer.data() + pos;
      std::uint32_t size = 0;
      std::memcpy(&size, buffer.data() + pos + 4, sizeof(size));
      auto const payload = pos + 8;
      if (size > buffer.size() - payload) return false;
      if (id[0]=='T'&&id[1]=='X'&&id[2]=='I'&&id[3]=='D' && size % 4 == 0)
      {
        texture_ids.resize(size / 4);
        if (size) std::memcpy(texture_ids.data(), buffer.data() + payload, size);
      }
      else if (id[0]=='S'&&id[1]=='F'&&id[2]=='I'&&id[3]=='D' && size % 4 == 0)
      {
        skin_ids.resize(size / 4);
        if (size) std::memcpy(skin_ids.data(), buffer.data() + payload, size);
      }
      else if (id[0]=='M'&&id[1]=='D'&&id[2]=='2'&&id[3]=='1')
      {
        md21_payload = payload; md21_size = size;
      }
      pos = payload + size;
    }
    if (!md21_payload || md21_size < 8 || md21_size > buffer.size() - md21_payload) return false;
    auto const* body = buffer.data() + md21_payload;
    if (!(body[0]=='M'&&body[1]=='D'&&body[2]=='2'&&body[3]=='0')) return false;
    std::vector<char> md20(body, body + md21_size);
    std::uint32_t version = 0;
    if (read_u32(md20, md20_version_offset, version) && version == 274) write_u32(md20, md20_version_offset, 272);
    inject_txid_paths(md20, texture_ids, client_data);
    register_sfid_paths(file_key, skin_ids, client_data);
    buffer.swap(md20);
    return true;
  }

  void adapt_modern_m2_buffer(Listfile::FileKey const& key, std::vector<char>& buffer,
                              std::vector<std::uint32_t>& texture_ids, std::vector<std::uint32_t>& skin_ids,
                              ClientData* client_data)
  {
    texture_ids.clear(); skin_ids.clear();
    if (has_m2_extension(key)) unwrap_chunked_m2_md21(key, buffer, texture_ids, skin_ids, client_data);
  }
}

void ClientFile::registerModernFileDataID(std::string const& filepath, std::uint32_t file_data_id)
{
  remember_modern_file_id(filepath, file_data_id);
}

ClientFile::ClientFile(Listfile::FileKey const& file_key, ClientData* client_data)
  : _file_key(file_key), _eof(true), _pointer(0), _external(false)
{
  bool const model_asset = is_model_asset(file_key) || (file_key.hasFilepath() && remembered_modern_file_id(file_key.filepath()) != 0);
  if (client_data->version() != ClientVersion::WOTLK)
  {
    if (model_asset) restore_registered_file_id(_file_key);
    _file_key.deduceOtherComponent(client_data->listfile());
    if (model_asset) restore_registered_file_id(_file_key);
  }

  _disk_path = client_data->getDiskPath(_file_key);
  std::ifstream input(_disk_path.string(), std::ios_base::binary | std::ios_base::in);
  if (input.is_open())
  {
    _external = true; _eof = false;
    input.seekg(0, std::ios::end); _buffer.resize(input.tellg());
    input.seekg(0, std::ios::beg); input.read(_buffer.data(), _buffer.size()); input.close();
    if (model_asset) adapt_modern_m2_buffer(_file_key, _buffer, _m2_texture_file_data_ids, _m2_skin_file_data_ids, client_data);
    return;
  }

  // Preserve the exact pre-M2 behavior for DB2/ADT/WDT and every other shared
  // ClientFile consumer. Only model assets use the resolved/registered FileDataID.
  auto const& read_key = model_asset ? _file_key : file_key;
  if (client_data->readFile(read_key, _buffer))
  {
    _eof = false;
    if (model_asset) adapt_modern_m2_buffer(_file_key, _buffer, _m2_texture_file_data_ids, _m2_skin_file_data_ids, client_data);
    return;
  }

  throw Exceptions::FileReadFailedError("File '" + (read_key.hasFilepath() ? read_key.filepath() : std::to_string(read_key.fileDataID())) + "' does not exist or some other error occured.");
}

ClientFile::ClientFile(Listfile::FileKey const& file_key, ClientData* client_data, NEW_FILE_T)
  : _file_key(file_key), _eof(true), _pointer(0), _external(false)
{
  if (client_data->version() != ClientVersion::WOTLK) _file_key.deduceOtherComponent(client_data->listfile());
  _disk_path = client_data->getDiskPath(_file_key);
}

std::size_t ClientFile::read(void* dest, size_t bytes)
{
  if (_eof || !bytes) return 0;
  size_t rpos = _pointer + bytes;
  if (rpos > _buffer.size()) { bytes = _buffer.size() - _pointer; _eof = true; }
  std::memcpy(dest, &(_buffer[_pointer]), bytes); _pointer = rpos; return bytes;
}
bool ClientFile::isEof() const { return _eof; }
void ClientFile::seek(std::size_t offset) { _pointer = offset; _eof = (_pointer >= _buffer.size()); }
void ClientFile::seekRelative(std::size_t offset) { _pointer += offset; _eof = (_pointer >= _buffer.size()); }
void ClientFile::close() { _eof = true; }
std::size_t ClientFile::getSize() const { return _buffer.size(); }
std::size_t ClientFile::getPos() const { return _pointer; }
char const* ClientFile::getBuffer() const { return _buffer.data(); }
char const* ClientFile::getPointer() const { return _buffer.data() + _pointer; }
void ClientFile::save()
{
  std::cout << "Saving file to: " << _disk_path << std::endl;
  auto const directory_name(_disk_path.parent_path()); std::error_code ec;
  std::filesystem::create_directories(directory_name, ec);
  if (ec) std::cout << "Error: Creating directory \"" << directory_name << "\" failed: " << ec << ". Saving is highly likely to fail." << std::endl;
  std::ofstream output(_disk_path.string(), std::ios_base::binary | std::ios_base::out);
  if (output.is_open()) { output.write(_buffer.data(), _buffer.size()); output.close(); _external = true; }
  else std::cout << "Error saving file to: " << _disk_path << std::endl;
}
