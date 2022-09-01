#include "CLI11.hpp"
#include "compress.hpp"
#include "fmt.hpp"
#include "options.hpp"
#include "subcmd_check.hpp"
#include "subcmd_compress.hpp"
#include "subcmd_decompress.hpp"
#include "version.hpp"

#include <unistd.h>

#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;


static
void
generate_compress_argparser(CLI::App &app_,
                            Options  &opts_)
{
  CLI::App *subcmd;

  subcmd = app_.add_subcommand("compress","Compress input file");
  subcmd->add_option("input-filepath",opts_.input_filepath)
    ->description("Path to input file")
    ->type_name("PATH")
    ->check(CLI::ExistingFile)
    ->required();
  subcmd->add_option("output-filepath",opts_.output_filepath)
    ->description("Path to output file (default: input + '.compressed')")
    ->type_name("PATH")
    ->option_text("PATH:FILE");

  auto func = std::bind(SubCmd::compress,std::cref(opts_));
  subcmd->callback(func);
}

static
void
generate_decompress_argparser(CLI::App &app_,
                              Options  &opts_)
{
  CLI::App *subcmd;

  subcmd = app_.add_subcommand("decompress","Decompress input file");
  subcmd->add_option("input-filepath",opts_.input_filepath)
    ->description("Path to input file")
    ->type_name("PATH")
    ->check(CLI::ExistingFile)
    ->required();
  subcmd->add_option("output-filepath",opts_.output_filepath)
    ->description("Path to output file (default: input + '.decompressed')")
    ->type_name("PATH")
    ->option_text("PATH:FILE");

  auto func = std::bind(SubCmd::decompress,std::cref(opts_));
  subcmd->callback(func);
}

static
void
generate_check_argparser(CLI::App &app_,
                         Options  &opts_)
{
  CLI::App *subcmd;

  subcmd = app_.add_subcommand("check");
  subcmd->description("Checks the compressor and decompressor against data "
                      "generated by the 3DO SDK compression library");

  subcmd->callback(SubCmd::check);
}

static
void
generate_argparser(CLI::App &app_,
                   Options  &opts_)
{
  std::string desc;

  app_.set_help_all_flag("--help-all","List help for all subcommands");
  app_.require_subcommand();

  desc = fmt::format("3ct: 3DO Compression Tool (v{}.{}.{})",
                     VERSION_MAJOR,
                     VERSION_MINOR,
                     VERSION_PATCH);
  app_.description(desc);

  generate_compress_argparser(app_,opts_);
  generate_decompress_argparser(app_,opts_);
  generate_check_argparser(app_,opts_);
}

int
main(int    argc_,
     char **argv_)
{
  Options opts;
  CLI::App app;

  generate_argparser(app,opts);

  try
    {
      app.parse(argc_,argv_);
    }
  catch(const CLI::ParseError &e_)
    {
      return app.exit(e_);
    }
  catch(const std::system_error &e_)
    {
      fmt::print("{} ({})\n",e_.what(),e_.code().message());
    }
  catch(const std::runtime_error &e_)
    {
      fmt::print("{}\n",e_.what());
    }

  return 0;
}