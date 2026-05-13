#include "subcmd_compress.hpp"

#include "compress.h"
#include "fmt.hpp"

#include <errno.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

namespace fs = std::filesystem;

namespace l
{
  struct FileCloser
  {
    void
    operator()(FILE *f_) const
    {
      if(f_ != NULL)
        fclose(f_);
    }
  };

  using FilePtr = std::unique_ptr<FILE,FileCloser>;

  struct OutputFile
  {
    FILE *f;
    bool  write_failed;
    int   write_errno;
  };

  static
  FilePtr
  open_file(const fs::path &path_,
            const char     *mode_)
  {
    FILE *f;

    f = fopen(path_.string().c_str(),mode_);
    if(f == NULL)
      throw fmt::exception("ERROR: failed to open {} - {}",path_,strerror(errno));

    return FilePtr(f);
  }

  static
  bool
  multiple_of_4(std::size_t v_)
  {
    return ((v_ & 0x3) == 0);
  }

  static
  std::size_t
  file_size(FILE           *f_,
            const fs::path &path_)
  {
    long orig_pos;
    long end_pos;

    orig_pos = ftell(f_);
    if(orig_pos < 0)
      throw fmt::exception("ERROR: failed to tell {} - {}",path_,strerror(errno));

    if(fseek(f_,0L,SEEK_END) != 0)
      throw fmt::exception("ERROR: failed to seek {} - {}",path_,strerror(errno));

    end_pos = ftell(f_);
    if(end_pos < 0)
      throw fmt::exception("ERROR: failed to tell {} - {}",path_,strerror(errno));

    if(fseek(f_,orig_pos,SEEK_SET) != 0)
      throw fmt::exception("ERROR: failed to seek {} - {}",path_,strerror(errno));

    return end_pos;
  }

  static
  void
  close_file(FilePtr        &f_,
             const fs::path &path_)
  {
    if(fclose(f_.release()) != 0)
      throw fmt::exception("ERROR: failed to close {} - {}",path_,strerror(errno));
  }

  static
  void
  write_word(void     *output_,
             uint32_t  word_)
  {
    OutputFile *output = (OutputFile*)output_;

    if(output->write_failed)
      return;

    if(fwrite(&word_,1,sizeof(word_),output->f) != sizeof(word_))
      {
        output->write_failed = true;
        output->write_errno = errno ? errno : EIO;
      }
  }

  static
  void
  check_result(int         rv_,
               const char *operation_)
  {
    if(rv_ < 0)
      throw fmt::exception("ERROR: {} failed ({})",operation_,rv_);
  }

  static
  void
  check_write(OutputFile    const &output_,
              const fs::path      &path_)
  {
    if(output_.write_failed)
      throw fmt::exception("ERROR: failed to write {} - {}",path_,strerror(output_.write_errno));
  }

  static
  void
  compress(FILE           *src_,
           OutputFile     *dst_,
           const fs::path &src_path_,
           const fs::path &dst_path_)
  {
    int rv;
    bool read_failed;
    int read_errno;
    Compressor *comp;

    comp = NULL;
    read_failed = false;
    read_errno = 0;

    rv = CreateCompressor(&comp,(CompFunc)l::write_word,NULL,(void*)dst_);
    if(rv < 0)
      throw std::runtime_error("CreateCompressor failed");

    while(true)
      {
        uint32_t w;
        std::size_t bytes_read;

        w = 0;
        bytes_read = fread(&w,1,sizeof(w),src_);
        if(bytes_read == 0)
          {
            if(ferror(src_))
              {
                read_failed = true;
                read_errno = errno ? errno : EIO;
              }
            break;
          }

        rv = FeedCompressor(comp,&w,1);
        check_result(rv,"FeedCompressor");

        if(bytes_read < sizeof(w))
          {
            if(ferror(src_))
              {
                read_failed = true;
                read_errno = errno ? errno : EIO;
              }
            break;
          }
      }

    rv = DeleteCompressor(comp);
    check_result(rv,"DeleteCompressor");

    if(read_failed)
      throw fmt::exception("ERROR: failed to read {} - {}",src_path_,strerror(read_errno));

    check_write(*dst_,dst_path_);
  }
}

void
SubCmd::compress(Options const &opts_)
{
  l::FilePtr src;
  l::FilePtr dst;
  l::OutputFile output;
  fs::path src_filepath;
  fs::path dst_filepath;
  std::size_t src_file_size;
  std::size_t dst_file_size;

  src_filepath = opts_.input_filepath;
  dst_filepath = opts_.output_filepath;
  if(dst_filepath.empty())
    {
      dst_filepath  = src_filepath;
      dst_filepath += ".compressed";
    }

  src = l::open_file(src_filepath,"rb");
  dst = l::open_file(dst_filepath,"wb");
  output.f = dst.get();
  output.write_failed = false;
  output.write_errno = 0;

  src_file_size = l::file_size(src.get(),src_filepath);
  if(!l::multiple_of_4(src_file_size))
    fmt::print(stderr,
               "WARNING - input file is not a multiple of 4 bytes. "
               "Uncompressing this file will result in a file padded with zeros.\n");

  l::compress(src.get(),&output,src_filepath,dst_filepath);

  dst_file_size = l::file_size(dst.get(),dst_filepath);
  l::close_file(dst,dst_filepath);
  l::close_file(src,src_filepath);

  fmt::print("- input:\n"
             "  - filepath: {}\n"
             "  - size_in_bytes: {}\n"
             "  - size_in_words: {}\n"
             "- output:\n"
             "  - filepath: {}\n"
             "  - size_in_bytes: {}\n"
             "  - size_in_words: {}\n"
             ,
             src_filepath,
             src_file_size,
             src_file_size / sizeof(uint32_t),
             dst_filepath,
             dst_file_size,
             dst_file_size / sizeof(uint32_t));
}
