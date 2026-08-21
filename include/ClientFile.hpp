#ifndef BLIZZARDARCHIVE_CLIENTFILE_HPP
#define BLIZZARDARCHIVE_CLIENTFILE_HPP

#include <ClientData.hpp>
#include <BaseArchive.hpp>
#include <filesystem>
#include <cstdint>
#include <string>
#include <vector>

namespace BlizzardArchive
{

  namespace Listfile
  {
    class FileKey;
  }

  class ClientFile
  {
  public:

    struct NEW_FILE_T {};
    inline static constexpr NEW_FILE_T NEW_FILE {};

    explicit ClientFile(Listfile::FileKey const& file_key, ClientData* client_data);
    explicit ClientFile(Listfile::FileKey const& file_key, ClientData* client_data, NEW_FILE_T);

    ClientFile() = delete;
    ClientFile(ClientFile const&) = delete;
    ClientFile(ClientFile&&) = delete;
    ClientFile& operator=(ClientFile const&) = delete;
    ClientFile& operator=(ClientFile&&) = delete;

    // Modern ADT/WMO/M2 references carry an authoritative FileDataID. Legacy
    // Noggit call sites often retain only the pathname, so remember the pairing
    // and restore the FileDataID when a later ClientFile is opened by path.
    static void registerModernFileDataID(std::string const& filepath, std::uint32_t file_data_id);

    std::size_t read(void* dest, std::size_t bytes);

    [[nodiscard]]
    std::size_t getSize() const;

    [[nodiscard]]
    std::size_t getPos() const;

    [[nodiscard]]
    char const* getBuffer() const;

    [[nodiscard]]
    char const* getPointer() const;

    [[nodiscard]]
    bool isEof() const;

    [[nodiscard]]
    std::vector<std::uint32_t> const& m2TextureFileDataIDs() const { return _m2_texture_file_data_ids; }

    [[nodiscard]]
    std::vector<std::uint32_t> const& m2SkinFileDataIDs() const { return _m2_skin_file_data_ids; }

    void seek(std::size_t offset);
    void seekRelative(std::size_t offset);
    void close();

    [[nodiscard]]
    bool isExternal() const
    {
      return _external;
    }

    template<typename T>
    const T* get(size_t offset) const
    {
      return reinterpret_cast<T const*>(_buffer.data() + offset);
    }

    void setBuffer(std::vector<char> const& vec) { _buffer = vec; }

    void save();

  private:
    bool _eof;
    std::vector<char> _buffer;
    std::vector<std::uint32_t> _m2_texture_file_data_ids;
    std::vector<std::uint32_t> _m2_skin_file_data_ids;
    size_t _pointer;
    bool _external;
    std::filesystem::path _disk_path;
    Listfile::FileKey _file_key;
  };
}

#endif // BLIZZARDARCHIVE_CLIENTFILE_HPP
