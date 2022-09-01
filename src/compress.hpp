#pragma once

#include "errors.hpp"
#include "types.hpp"

#include <cstdint>

typedef struct Compressor Compressor;

int CreateCompressor(Compressor **comp, CompFunc cf, void *workbuf, void *userdata);
int DeleteCompressor(Compressor *comp);
int FeedCompressor(Compressor *comp, void *data, uint32_t numDataWords);
int32_t GetCompressorWorkBufferSize();

int SimpleCompress(void *source, uint32_t sourceWords, void *result, uint32_t resultWords);
