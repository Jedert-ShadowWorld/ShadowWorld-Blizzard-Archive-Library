#ifndef BLIZZARDARCHIVE_CASCARCHIVE_HPP
#define BLIZZARDARCHIVE_CASCARCHIVE_HPP

#include <BaseArchive.hpp>

namespace BlizzardArchive::Listfile
{
  class Listfile;
}

namespace BlizzardArchive::Archive
{

  class CASCArchive : public BaseArchive
  {
  public:
    CASCArchive(std::string const& path, std::string const& cache_path, Locale locale, OpenMode open_mode,
                bool neutral_locale_first, Listfile::Listfile* listfile);
    ~CASCArchive() override;

    [[nodiscard]]
    bool openFile(Listfile::FileKey const& file_key, Locale locale, HANDLE* file_handle) const override;

    bool readFile(HANDLE file_handle, char* buffer, std::size_t buf_size) const override;
    bool closeFile(HANDLE file_handle) const override;

    [[nodiscard]]
    std::uint64_t getFileSize(HANDLE file_handle) const override;

    [[nodiscard]]
    bool exists(Listfile::FileKey const& file_key, Locale locale) const override;

    [[nodiscard]]
    std::string lastErrorString() const override;

  private:
    HANDLE _handle = nullptr;
    bool _neutral_locale_first = false;
    mutable std::uint32_t _last_error = 0;
    mutable std::string _diagnostics;
  };

}

#endif //BLIZZARDARCHIVE_CASCARCHIVE_HPP
