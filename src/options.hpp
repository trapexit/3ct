#pragma once

#include <filesystem>
#include <string>

struct Options
{
  std::filesystem::path input_filepath;
  std::filesystem::path output_filepath;
  std::string ggc_file_type;
};
