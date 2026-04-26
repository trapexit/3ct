#pragma once

#include "errors.hpp"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t  CompInt32;
typedef uint32_t CompUInt32;
typedef void (*CompFunc)(void *userData, uint32_t word);

typedef struct Compressor Compressor;

int CreateCompressor(Compressor **comp,
                     CompFunc     cf,
                     void        *workbuf,
                     void        *userdata);
int DeleteCompressor(Compressor *comp);
int FeedCompressor(Compressor *comp, void *data, CompUInt32 numDataWords);
int GetCompressorWorkBufferSize(void);

int SimpleCompress(void      *source,
                   CompUInt32 sourceWords,
                   void      *result,
                   CompUInt32 resultWords);

#ifdef __cplusplus
}
#endif
