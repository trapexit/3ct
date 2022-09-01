#pragma once

#include "errors.hpp"
#include "types.hpp"

#include <cstdint>

typedef struct Decompressor Decompressor;

int CreateDecompressor(Decompressor **decomp, CompFunc cf, void *workbuf, void *userdata);
int DeleteDecompressor(Decompressor *decomp);
int FeedDecompressor(Decompressor *decomp, void *data, uint32_t numDataWords);
int32_t GetDecompressorWorkBufferSize();

int SimpleDecompress(void *source, uint32_t sourceWords, void *result, uint32_t resultWords);
