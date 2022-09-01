#include "subcmd_decompress.hpp"

#include "decompress.hpp"
#include "fmt.hpp"

#include <errno.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace fs = std::filesystem;

namespace l
{
  static
  bool
  multiple_of_4(std::size_t v_)
  {
    return ((v_ & 0x3) == 0);
  }

  static
  std::size_t
  file_size(FILE *f_)
  {
    long orig_pos;
    long end_pos;

    orig_pos = ftell(f_);
    fseek(f_,0L,SEEK_END);
    end_pos = ftell(f_);
    fseek(f_,orig_pos,SEEK_SET);

    return end_pos;
  }

  static
  void
  write_word(void     *f_,
             uint32_t  word_)
  {
    FILE *f = (FILE*)f_;
    fwrite(&word_,1,sizeof(word_),f);
  }

  static
  void
  decompress(FILE *src_,
             FILE *dst_)
  {
    int rv;
    Decompressor *decomp;

    rv = CreateDecompressor(&decomp,(CompFunc)l::write_word,NULL,(void*)dst_);
    if(rv < 0)
      throw std::runtime_error("CreateDecompressor failed");

    while(true)
      {
        uint32_t w;

        w = 0;
        rv = fread(&w,1,sizeof(w),src_);
        if((rv == 0) && (feof(src_) || ferror(src_)))
          break;
        FeedDecompressor(decomp,&w,1);
      }

    rv = DeleteDecompressor(decomp);
  }
}

void
SubCmd::decompress(Options const &opts_)
{
  FILE *src;
  FILE *dst;
  fs::path src_filepath;
  fs::path dst_filepath;
  std::size_t src_file_size;
  std::size_t dst_file_size;

  src_filepath = opts_.input_filepath;
  dst_filepath = opts_.output_filepath;
  if(dst_filepath.empty())
    {
      dst_filepath  = src_filepath;
      dst_filepath += ".decompressed";
    }

  src = fopen(src_filepath.string().c_str(),"rb");
  if(src == NULL)
    throw fmt::exception("ERROR: failed to open {} - {}",src_filepath,strerror(errno));

  dst = fopen(dst_filepath.string().c_str(),"wb");
  if(dst == NULL)
    throw fmt::exception("ERROR: failed to open {} - {}",dst_filepath,strerror(errno));

  src_file_size = l::file_size(src);
  if(!l::multiple_of_4(src_file_size))
    fmt::print(stderr,
               "WARNING - input file is not a multiple of 4 bytes. "
               "The file may be corrupted or not a 3DO compressed file.\n");

  l::decompress(src,dst);

  dst_file_size = l::file_size(dst);
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

  fclose(src);
  fclose(dst);
}
