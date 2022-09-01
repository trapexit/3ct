#pragma once

#include <filesystem>

struct Options
{
  std::filesystem::path input_filepath;
  std::filesystem::path output_filepath;
};
