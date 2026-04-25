#include "subcmd_ggc_compress.hpp"

#include "fmt.hpp"
#include "ggc.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;

bool
_plausible_file_type_byte(uint8_t c_)
{
  return (((c_ >= 'A') && (c_ <= 'Z')) ||
          ((c_ >= '0') && (c_ <= '9')) ||
          (c_ == ' '));
}

std::vector<uint8_t>
_read_file(const fs::path &path_)
{
  FILE *f;
  long size;
  std::vector<uint8_t> data;

  f = fopen(path_.string().c_str(),"rb");
  if(f == NULL)
    throw fmt::exception("ERROR: failed to open {} - {}",path_,strerror(errno));

  fseek(f,0L,SEEK_END);
  size = ftell(f);
  fseek(f,0L,SEEK_SET);

  if(size < 0)
    {
      fclose(f);
      throw fmt::exception("ERROR: failed to size {}",path_);
    }

  data.resize(static_cast<std::size_t>(size));
  if(!data.empty() && fread(data.data(),1,data.size(),f) != data.size())
    {
      fclose(f);
      throw fmt::exception("ERROR: failed to read {}",path_);
    }

  fclose(f);

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
      fclose(f);
      throw fmt::exception("ERROR: failed to write {}",path_);
    }

  fclose(f);
}

std::vector<uint8_t>
compress_ggc(const std::vector<uint8_t> &src_,
             const unsigned char         file_type_[4])
{
  int rv;
  unsigned char *dst;
  size_t dst_size;
  std::vector<uint8_t> out;

  dst = NULL;
  dst_size = 0;

  rv = ggc_compress(src_.data(),src_.size(),file_type_,&dst,&dst_size);
  if(rv != GGC_OK)
    throw std::runtime_error("GGC compression failed");

  out.assign(dst,dst + dst_size);
  ggc_free(dst);

  return out;
}


void
SubCmd::ggc_compress(Options const &opts_)
{
  fs::path src_filepath;
  fs::path dst_filepath;
  std::vector<uint8_t> src;
  std::vector<uint8_t> dst;
  unsigned char file_type[4] = {' ',' ',' ',' '};

  src_filepath = opts_.input_filepath;
  dst_filepath = opts_.output_filepath;
  if(dst_filepath.empty())
    {
      dst_filepath = src_filepath;
      dst_filepath += ".COMP";
    }

  src = ::_read_file(src_filepath);
  if(opts_.ggc_file_type.empty())
    {
      if((src.size() >= 4) &&
         ::_plausible_file_type_byte(src[0]) &&
         ::_plausible_file_type_byte(src[1]) &&
         ::_plausible_file_type_byte(src[2]) &&
         ::_plausible_file_type_byte(src[3]))
        {
          file_type[0] = src[0];
          file_type[1] = src[1];
          file_type[2] = src[2];
          file_type[3] = src[3];
        }
    }
  else
    {
      if(opts_.ggc_file_type.size() != 4)
        throw fmt::exception("ERROR: --file-type must be exactly 4 bytes");

      for(std::size_t i = 0; i < sizeof(file_type); i++)
        file_type[i] = static_cast<unsigned char>(opts_.ggc_file_type[i]);
    }

  dst = compress_ggc(src,file_type);
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
