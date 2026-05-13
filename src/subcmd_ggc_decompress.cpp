#include "subcmd_ggc_decompress.hpp"

#include "fmt.hpp"
#include "ggc.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;

namespace
{
  std::vector<uint8_t>
  read_file(const fs::path &path_)
  {
    FILE *f;
    long size;
    std::vector<uint8_t> data;

    f = fopen(path_.string().c_str(),"rb");
    if(f == NULL)
      throw fmt::exception("ERROR: failed to open {} - {}",path_,strerror(errno));

    if(fseek(f,0L,SEEK_END) != 0)
      {
        int err = errno ? errno : EIO;
        fclose(f);
        throw fmt::exception("ERROR: failed to seek {} - {}",path_,strerror(err));
      }

    size = ftell(f);
    if(size < 0)
      {
        int err = errno ? errno : EIO;
        fclose(f);
        throw fmt::exception("ERROR: failed to size {} - {}",path_,strerror(err));
      }

    if(fseek(f,0L,SEEK_SET) != 0)
      {
        int err = errno ? errno : EIO;
        fclose(f);
        throw fmt::exception("ERROR: failed to seek {} - {}",path_,strerror(err));
      }

    data.resize(static_cast<std::size_t>(size));
    if(!data.empty() && fread(data.data(),1,data.size(),f) != data.size())
      {
        int err = ferror(f) ? (errno ? errno : EIO) : EIO;
        fclose(f);
        throw fmt::exception("ERROR: failed to read {} - {}",path_,strerror(err));
      }

    if(fclose(f) != 0)
      throw fmt::exception("ERROR: failed to close {} - {}",path_,strerror(errno ? errno : EIO));

    return data;
  }

  void
  write_file(const fs::path             &path_,
             const std::vector<uint8_t> &data_)
  {
    FILE *f;

    f = fopen(path_.string().c_str(),"wb");
    if(f == NULL)
      throw fmt::exception("ERROR: failed to open {} - {}",path_,strerror(errno));

    if(!data_.empty() && fwrite(data_.data(),1,data_.size(),f) != data_.size())
      {
        int err = errno ? errno : EIO;
        fclose(f);
        throw fmt::exception("ERROR: failed to write {} - {}",path_,strerror(err));
      }

    if(fclose(f) != 0)
      throw fmt::exception("ERROR: failed to close {} - {}",path_,strerror(errno ? errno : EIO));
  }

  std::vector<uint8_t>
  decompress_ggc(const std::vector<uint8_t> &src_)
  {
    int rv;
    unsigned char *dst;
    size_t dst_size;
    std::vector<uint8_t> out;

    dst = NULL;
    dst_size = 0;

    rv = ggc_decompress(src_.data(),src_.size(),&dst,&dst_size);
    if(rv != GGC_OK)
      throw std::runtime_error("GGC decompression failed");

    out.assign(dst,dst + dst_size);
    ggc_free(dst);

    return out;
  }
}

void
SubCmd::ggc_decompress(Options const &opts_)
{
  fs::path src_filepath;
  fs::path dst_filepath;
  std::vector<uint8_t> src;
  std::vector<uint8_t> dst;

  src_filepath = opts_.input_filepath;
  dst_filepath = opts_.output_filepath;
  if(dst_filepath.empty())
    {
      dst_filepath = src_filepath;
      dst_filepath += ".decompressed";
    }

  src = read_file(src_filepath);
  dst = decompress_ggc(src);
  write_file(dst_filepath,dst);

  fmt::print("- input:\n"
             "  - filepath: {}\n"
             "  - size_in_bytes: {}\n"
             "- output:\n"
             "  - filepath: {}\n"
             "  - size_in_bytes: {}\n",
             src_filepath,
             src.size(),
             dst_filepath,
             dst.size());
}
