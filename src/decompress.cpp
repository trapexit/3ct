#include "byteswap.hpp"
#include "errors.hpp"
#include "lzss.h"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>

/*****************************************************************************/


/* WARNING: Check with the legal department before making any changes to
 *          the algorithms used herein in order to ensure that the code
 *          doesn't infringe on the multiple gratuitous patents on
 *          compression code.
 */


/*****************************************************************************/


typedef struct DecompressBitStream
{
  uint32_t *bs_Data;
  uint32_t  bs_NumDataWords;
  uint32_t  bs_BitsLeft;
  uint32_t  bs_BitBuffer;
  bool      bs_Error;
} DecompressBitStream;


/*****************************************************************************/


typedef void (*CompFuncClone)(void *userData, uint32_t word);

typedef struct Decompressor
{
  CompFuncClone        dh_OutputWord;
  void                *dh_UserData;
  uint32_t             dh_WordBuffer;
  uint32_t             dh_BytesLeft;
  uint32_t             dh_Pos;
  unsigned char        dh_Window[WINDOW_SIZE];
  DecompressBitStream  dh_BitStream;
  bool                 dh_AllocatedStructure;
  void                *dh_Cookie;
} Decompressor;


/*****************************************************************************/


static void InitBitStream(DecompressBitStream *bs)
{
  bs->bs_BitsLeft  = 0;
  bs->bs_BitBuffer = 0;
  bs->bs_Error     = false;
}


/*****************************************************************************/


static void FeedBitStream(DecompressBitStream *bs, const void *data, uint32_t numDataWords)
{
  bs->bs_Data         = (uint32_t *)data;
  bs->bs_NumDataWords = numDataWords;
}


/*****************************************************************************/


static uint32_t ReadBits(DecompressBitStream *bs, uint32_t numBits)
{
  uint32_t result;

  result = 0;

  if (numBits > bs->bs_BitsLeft)
    {
      if (bs->bs_BitsLeft)
        {
          result = (bs->bs_BitBuffer << (numBits - bs->bs_BitsLeft)) & ((1 << numBits) - 1);
          numBits -= bs->bs_BitsLeft;
        }

      if (!bs->bs_NumDataWords)
        {
          bs->bs_Error = true;
          return (0);
        }

      bs->bs_NumDataWords--;
      bs->bs_BitBuffer = ::byteswap_if_little_endian(*bs->bs_Data++);
      bs->bs_BitsLeft  = 32;
    }

  bs->bs_BitsLeft -= numBits;
  result |= (bs->bs_BitBuffer >> bs->bs_BitsLeft) & ((1 << numBits) - 1);

  return (result);
}


/*****************************************************************************/


/* All this decompression routine has to do is read in flag bits, decide
 * whether to read in a character or an index/length pair, and take the
 * appropriate action.
 */

static
int
internalFeedDecompressor(Decompressor *decomp,
                         void         *data,
                         uint32_t      numDataWords)
{
  uint32_t       i;
  uint32_t       pos;
  uint32_t       c;
  uint32_t       matchLen;
  uint32_t       matchPos;
  DecompressBitStream     *bs;
  uint32_t       wordBuffer;
  uint32_t       bytesLeft;
  CompFunc       cf;
  unsigned char *window;
  void          *userData;

  cf         = decomp->dh_OutputWord;
  wordBuffer = decomp->dh_WordBuffer;
  bytesLeft  = decomp->dh_BytesLeft;
  window     = decomp->dh_Window;
  userData   = decomp->dh_UserData;
  bs         = &decomp->dh_BitStream;
  pos        = decomp->dh_Pos;

  FeedBitStream(bs, data, numDataWords);

  while (bs->bs_NumDataWords)
    {
      if (ReadBits(bs,1))
        {
          c = ReadBits(bs, 8);
          if (bytesLeft == 0)
            {
              (*cf)(userData,::byteswap_if_little_endian(wordBuffer));
              wordBuffer = c;
              bytesLeft  = 3;
            }
          else
            {
              wordBuffer = (wordBuffer << 8) | c;
              bytesLeft--;
            }

          window[pos] = (unsigned char) c;
          pos = MOD_WINDOW(pos + 1);
        }
      else
        {
          matchPos = ReadBits(bs, INDEX_BIT_COUNT);
          if (matchPos == END_OF_STREAM)
            break;

          matchLen = ReadBits(bs, LENGTH_BIT_COUNT) + BREAK_EVEN;

          for (i = matchPos; i <= matchLen + matchPos; i++)
            {
              c = window[MOD_WINDOW(i)];

              if (bytesLeft == 0)
                {
                  (*cf)(userData,::byteswap_if_little_endian(wordBuffer));
                  wordBuffer = c;
                  bytesLeft  = 3;
                }
              else
                {
                  wordBuffer = (wordBuffer << 8) | c;
                  bytesLeft--;
                }

              window[pos] = (unsigned char) c;
              pos = MOD_WINDOW(pos + 1);
            }
        }
    }

  decomp->dh_BytesLeft  = bytesLeft;
  decomp->dh_WordBuffer = wordBuffer;
  decomp->dh_Pos        = pos;

  return (0);
}


/*****************************************************************************/


int
CreateDecompressor(Decompressor **decomp,
                   CompFunc cf,
                   void *workbuf_,
                   void *userdata_)
{
  bool    allocated;
  void   *buffer;
  void   *userData;

  if (!decomp)
    return COMP_ERR_BADPTR;

  *decomp = NULL;

  if(!cf)
    return (COMP_ERR_BADPTR);

  buffer   = NULL;
  userData = NULL;

  buffer = workbuf_;
  userData = userdata_;

  allocated = false;
  if (!buffer)
    {
      buffer = calloc(1,sizeof(Decompressor));
      if (!buffer)
        return COMP_ERR_NOMEM;

      allocated = true;
    }

  (*decomp)                        = (Decompressor *)buffer;
  (*decomp)->dh_OutputWord         = cf;
  (*decomp)->dh_UserData           = userData;
  (*decomp)->dh_WordBuffer         = 0;
  (*decomp)->dh_BytesLeft          = 4;
  (*decomp)->dh_Pos                = 1;
  (*decomp)->dh_Cookie             = *decomp;
  (*decomp)->dh_AllocatedStructure = allocated;
  InitBitStream(&(*decomp)->dh_BitStream);

  return (0);
}


/*****************************************************************************/


int
DeleteDecompressor(Decompressor *decomp)
{
  int result;

  if (!decomp || (decomp->dh_Cookie != decomp))
    return (COMP_ERR_BADPTR);

  decomp->dh_Cookie = NULL;

  result = 0;

  if (decomp->dh_BytesLeft == 0)
    (*decomp->dh_OutputWord)(decomp->dh_UserData,
                             ::byteswap_if_little_endian(decomp->dh_WordBuffer));

  if (decomp->dh_BitStream.bs_NumDataWords)
    result = COMP_ERR_DATAREMAINS;

  if (decomp->dh_BitStream.bs_Error)
    result = COMP_ERR_DATAMISSING;

  if (decomp->dh_AllocatedStructure)
    free(decomp);

  return (result);
}


/*****************************************************************************/


int FeedDecompressor(Decompressor *decomp, void *data, uint32_t numDataWords)
{
  if (!decomp || (decomp->dh_Cookie != decomp))
    return (COMP_ERR_BADPTR);

  return (internalFeedDecompressor(decomp, data, numDataWords));
}


/*****************************************************************************/


int32_t
GetDecompressorWorkBufferSize()
{
  return sizeof(Decompressor);
}


struct Context
{
  uint32_t *dest;
  uint32_t *max;
  bool      overflow;
};


static
void
PutWord(Context  *ctx_,
        uint32_t  word_)
{
  if(ctx_->dest >= ctx_->max)
    ctx_->overflow = true;
  else
    *ctx_->dest++ = word_;
}


int
SimpleDecompress(void     *source_,
                 uint32_t  sourceWords_,
                 void     *result_,
                 uint32_t  resultWords_)
{
  Decompressor *decomp;
  Context       ctx;
  int           err;

  ctx.dest     = (uint32_t*)result_;
  ctx.max      = (uint32_t*)((uint64_t)result_ + resultWords_ * sizeof(uint32_t));
  ctx.overflow = false;

  err = CreateDecompressor(&decomp,(CompFunc)PutWord,NULL,&ctx);
  if(err < 0)
    return err;

  FeedDecompressor(decomp,source_,sourceWords_);
  err = DeleteDecompressor(decomp);

  if (err == 0)
    {
      if (ctx.overflow)
        err = COMP_ERR_OVERFLOW;
      else
        err = ((uint64_t)ctx.dest - (uint64_t)result_) / sizeof(uint32_t);
    }

  return err;
}
