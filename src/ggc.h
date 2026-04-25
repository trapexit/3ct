#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GGC_OK 0
#define GGC_ERR_BADPTR -1
#define GGC_ERR_NOMEM -2
#define GGC_ERR_BADSIZE -3
#define GGC_ERR_TRUNCATED -4

int ggc_compress(const unsigned char  *src,
                 size_t                src_size,
                 const unsigned char   file_type[4],
                 unsigned char       **dst,
                 size_t               *dst_size);

int ggc_decompress(const unsigned char  *src,
                   size_t                src_size,
                   unsigned char       **dst,
                   size_t               *dst_size);

void ggc_free(void *ptr);

#ifdef __cplusplus
}
#endif
