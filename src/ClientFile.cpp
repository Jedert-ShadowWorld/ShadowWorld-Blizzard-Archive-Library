#include <ClientFile.hpp>
#include <Exception.hpp>
#include <fstream>
#include <iostream>
#include <system_error>
#include <cstring>
#include <cstdint>

using namespace BlizzardArchive;

namespace
{
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

  // Legion and later M2 files are a forward-FourCC chunk stream. The actual
  // offset-addressed MD20 image consumed by Noggit's existing M2 reader lives
  // inside the MD21 chunk; offsets in that image are relative to the MD21
  // payload, not to the outer chunk container. Expose that payload as the
  // ClientFile buffer so legacy M2 code can keep using its normal offsets.
  bool unwrap_chunked_m2_md21(std::vector<char>& buffer)
  {
    if (buffer.size() < 12)
      return false;

    std::size_t pos = 0;
    while (pos + 8 <= buffer.size())
    {
      auto const* id = buffer.data() + pos;

      std::uint32_t size = 0;
      std::memcpy(&size, buffer.data() + pos + 4, sizeof(size));

      auto const payload = pos + 8;
      if (size > buffer.size() - payload)
        return false;

      if (id[0] == 'M' && id[1] == 'D' && id[2] == '2' && id[3] == '1')
      {
        if (size < 8)
          return false;

        // The embedded image itself should start with MD20. Refuse to unwrap
        // unknown data instead of handing a corrupt buffer to the old parser.
        auto const* body = buffer.data() + payload;
        if (!(body[0] == 'M' && body[1] == 'D' && body[2] == '2' && body[3] == '0'))
          return false;

        std::vector<char> md20(body, body + size);
        buffer.swap(md20);
        return true;
      }

      pos = payload + size;
    }

    return false;
  }

  void adapt_modern_m2_buffer(Listfile::FileKey const& file_key, std::vector<char>& buffer)
  {
    if (has_m2_extension(file_key))
      unwrap_chunked_m2_md21(buffer);
  }
}

ClientFile::ClientFile(Listfile::FileKey const& file_key, ClientData* client_data)
  : _file_key(file_key)
  , _eof(true)
  , _pointer(0)
  , _external(false)
{

  if (client_data->version() != ClientVersion::WOTLK)
  {
    _file_key.deduceOtherComponent(client_data->listfile());
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
    adapt_modern_m2_buffer(_file_key, _buffer);
    return;
  }

  if (client_data->readFile(file_key, _buffer))
  {
    _eof = false;
    adapt_modern_m2_buffer(_file_key, _buffer);
    return;
  }
 
  throw Exceptions::FileReadFailedError(
    "File '"
    + (file_key.hasFilepath() ? file_key.filepath() : std::to_string(file_key.fileDataID()))
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
    _file_key.deduceOtherComponent(client_data->listfile());
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

bool ClientFile::isEof() const
{
  return _eof;
}

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

void ClientFile::close()
{
  _eof = true;
}

std::size_t ClientFile::getSize() const
{
  return _buffer.size();
}

std::size_t ClientFile::getPos() const
{
  return _pointer;
}

char const* ClientFile::getBuffer() const
{
  return _buffer.data();
}

char const* ClientFile::getPointer() const
{
  return _buffer.data() + _pointer;
}

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