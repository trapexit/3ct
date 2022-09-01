#include "byteswap.hpp"
#include "errors.hpp"
#include "lzss.h"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

/*****************************************************************************/


/* This is the compression module which implements an LZ77-style (LZSS)
 * compression algorithm. As implemented here it uses a 12 bit index into the
 * sliding window, and a 4 bit length, which is adjusted to reflect phrase
 * lengths of between 3 and 18 bytes.
 *
 * WARNING: Check with the legal department before making any changes to
 *          the algorithms used herein in order to ensure that the code
 *          doesn't infrige on the multiple gratuitous patents on
 *          compression code.
 */


/*****************************************************************************/

/*
 * The tree[] structure contains the binary tree of all of the strings in the
 * window sorted in order.
 */
struct CompNode
{
  uint32_t cn_Parent;
  uint32_t cn_LeftChild;
  uint32_t cn_RightChild;
};

typedef void (*CompFuncClone)(void *userData, uint32_t word);

typedef struct CompressBitStream
{
  CompFuncClone  bs_OutputWord;
  void          *bs_UserData;
  uint32_t       bs_BitsLeft;
  uint32_t       bs_BitBuffer;
} CompressBitStream;


/*****************************************************************************/


typedef struct Compressor
{
  unsigned char      ch_Window[WINDOW_SIZE];
  CompNode           ch_Tree[WINDOW_SIZE + 1];
  int32_t            ch_LookAhead;
  int32_t            ch_MatchLen;
  uint32_t           ch_MatchPos;
  uint32_t           ch_CurrentPos;
  uint32_t           ch_ReplaceCnt;
  CompressBitStream  ch_BitStream;
  bool               ch_SecondPass;
  bool               ch_AllocatedStructure;
  void              *ch_Cookie;
} Compressor;


/*****************************************************************************/

typedef void *Decompressor;

/*****************************************************************************/

/* This is where most of the encoder's work is done. This routine is
 * responsible for adding the new node to the binary tree. It also has to
 * find the best match among all the existing nodes in the tree, and return
 * that to the calling routine. To make matters even more complicated, if
 * the newNode has a duplicate in the tree, the oldNode is deleted, for
 * reasons of efficiency.
 */

static
uint32_t
AddString(CompNode      *tree,
          unsigned char *window,
          uint32_t       newNode,
          uint32_t      *matchPos)
{
  uint32_t  i;
  uint32_t  testNode;
  uint32_t  parentNode;
  int32_t   delta;
  uint32_t  matchLen;
  CompNode *node;
  CompNode *parent;
  CompNode *test;

  if(newNode == END_OF_STREAM)
    return (0);

  testNode = tree[TREE_ROOT].cn_RightChild;
  node     = &tree[newNode];
  matchLen = 0;

  while(true)
    {
      for(i = 0; i < LOOK_AHEAD_SIZE; i++)
        {
          delta = window[MOD_WINDOW(newNode + i)] - window[MOD_WINDOW(testNode + i)];
          if(delta)
            break;
        }

      test = &tree[testNode];

      if(i >= matchLen)
        {
          matchLen  = i;
          *matchPos = testNode;
          if(matchLen >= LOOK_AHEAD_SIZE)
            {
              parentNode = test->cn_Parent;
              parent = &tree[parentNode];

              if(parent->cn_LeftChild == testNode)
                parent->cn_LeftChild = newNode;
              else
                parent->cn_RightChild = newNode;

              *node                               = *test;
              tree[node->cn_LeftChild].cn_Parent  = newNode;
              tree[node->cn_RightChild].cn_Parent = newNode;
              test->cn_Parent                     = UNUSED;

              return (matchLen);
            }
        }

      if(delta >= 0)
        {
          if(test->cn_RightChild == UNUSED)
            {
              test->cn_RightChild = newNode;
              node->cn_Parent     = testNode;
              node->cn_LeftChild  = UNUSED;
              node->cn_RightChild = UNUSED;
              return (matchLen);
            }
          testNode = test->cn_RightChild;
        }
      else
        {
          if(test->cn_LeftChild == UNUSED)
            {
              test->cn_LeftChild  = newNode;
              node->cn_Parent     = testNode;
              node->cn_LeftChild  = UNUSED;
              node->cn_RightChild = UNUSED;
              return (matchLen);
            }
          testNode = test->cn_LeftChild;
        }
    }
}

static
void
InitBitStream(CompressBitStream *bs,
              CompFuncClone      cf,
              void              *userData)
{
  bs->bs_OutputWord = cf;
  bs->bs_UserData   = userData;
  bs->bs_BitsLeft   = 32;
  bs->bs_BitBuffer  = 0;
}

static
void
CleanupBitStream(CompressBitStream *bs)
{
  if(bs->bs_BitsLeft != 32)
    (*bs->bs_OutputWord)(bs->bs_UserData,
                         ::byteswap_if_little_endian(bs->bs_BitBuffer));
}

/* This routine outputs a single header bit, followed by numBits of code */
static
void
WriteBits(CompressBitStream *bs,
          uint32_t           headBit,
          uint32_t           code,
          uint32_t           numBits)
{
  bs->bs_BitsLeft--;
  bs->bs_BitBuffer |= (headBit << bs->bs_BitsLeft);

  if(numBits >= bs->bs_BitsLeft)
    {
      numBits         -= bs->bs_BitsLeft;
      (*bs->bs_OutputWord)(bs->bs_UserData,
                           byteswap_if_little_endian(((code >> numBits) | bs->bs_BitBuffer)));
      bs->bs_BitsLeft  = 32 - numBits;

      if(!numBits)
        bs->bs_BitBuffer = 0;
      else
        bs->bs_BitBuffer = (code << bs->bs_BitsLeft);
    }
  else
    {
      bs->bs_BitsLeft  -= numBits;
      bs->bs_BitBuffer |= (code << bs->bs_BitsLeft);
    }
}

/* This routine performs a classic binary tree deletion.
 * If the node to be deleted has a null link in either direction, we
 * just pull the non-null link up one to replace the existing link.
 * If both links exist, we instead delete the next link in order, which
 * is guaranteed to have a null link, then replace the node to be deleted
 * with the next link.
 */
static
void
DeleteString(CompNode *tree,
             uint32_t  node)
{
  uint32_t parent;
  uint32_t newNode;
  uint32_t next;

  parent = tree[node].cn_Parent;
  if(parent != UNUSED)
    {
      if(tree[node].cn_LeftChild == UNUSED)
        {
          newNode                 = tree[node].cn_RightChild;
          tree[newNode].cn_Parent = parent;
        }
      else if(tree[node].cn_RightChild == UNUSED)
        {
          newNode                 = tree[node].cn_LeftChild;
          tree[newNode].cn_Parent = parent;
        }
      else
        {
          newNode = tree[node].cn_LeftChild;
          next    = tree[newNode].cn_RightChild;
          if(next != UNUSED)
            {
              do
                {
                  newNode = next;
                  next    = tree[newNode].cn_RightChild;
                }
              while(next != UNUSED);

              tree[tree[newNode].cn_Parent].cn_RightChild = UNUSED;
              tree[newNode].cn_Parent                     = tree[node].cn_Parent;
              tree[newNode].cn_LeftChild                  = tree[node].cn_LeftChild;
              tree[newNode].cn_RightChild                 = tree[node].cn_RightChild;
              tree[tree[newNode].cn_LeftChild].cn_Parent  = newNode;
              tree[tree[newNode].cn_RightChild].cn_Parent = newNode;
            }
          else
            {
              tree[newNode].cn_Parent                     = parent;
              tree[newNode].cn_RightChild                 = tree[node].cn_RightChild;
              tree[tree[newNode].cn_RightChild].cn_Parent = newNode;
            }
        }

      if(tree[parent].cn_LeftChild == node)
        tree[parent].cn_LeftChild = newNode;
      else
        tree[parent].cn_RightChild = newNode;

      tree[node].cn_Parent = UNUSED;
    }
}

int
CreateCompressor(Compressor **comp,
                 CompFunc     cf,
                 void        *workbuf_,
                 void        *userdata_)
{
  bool  allocated;
  void *buffer;
  void *userData;

  if(!comp)
    return COMP_ERR_BADPTR;

  *comp = NULL;

  if(!cf)
    return COMP_ERR_BADPTR;

  buffer   = workbuf_;
  userData = userdata_;

  allocated = false;
  if(!buffer)
    {
      buffer = malloc(sizeof(Compressor));
      if(!buffer)
        return COMP_ERR_NOMEM;

      allocated = true;
    }

  (*comp)                        = (Compressor*)buffer;
  (*comp)->ch_LookAhead          = 1;
  (*comp)->ch_CurrentPos         = 1;
  (*comp)->ch_MatchPos           = 0;
  (*comp)->ch_MatchLen           = 0;
  (*comp)->ch_ReplaceCnt         = 0;
  (*comp)->ch_SecondPass         = false;
  (*comp)->ch_Cookie             = *comp;
  (*comp)->ch_AllocatedStructure = allocated;

  InitBitStream(&(*comp)->ch_BitStream,cf,userData);

  /* To make the tree usable, everything must be set to UNUSED, and a
   * single phrase has to be added to the tree so it has a root node.
   */
  memset((*comp)->ch_Tree, UNUSED, sizeof((*comp)->ch_Tree));
  (*comp)->ch_Tree[TREE_ROOT].cn_RightChild = 1;
  (*comp)->ch_Tree[1].cn_Parent             = TREE_ROOT;

  return (0);
}

static
void
FlushCompressor(Compressor *comp)
{
  int32_t            lookAhead;
  uint32_t           currentPos;
  uint32_t           replaceCnt;
  int32_t            matchLen;
  uint32_t           matchPos;
  CompNode          *tree;
  unsigned char     *window;
  CompressBitStream *bs;
  uint32_t           temp;

  tree         = comp->ch_Tree;
  window       = comp->ch_Window;
  bs           = &comp->ch_BitStream;
  lookAhead    = comp->ch_LookAhead;
  currentPos   = comp->ch_CurrentPos;
  matchLen     = comp->ch_MatchLen;
  matchPos     = comp->ch_MatchPos;
  replaceCnt   = comp->ch_ReplaceCnt;

  if(comp->ch_SecondPass)
    goto newData;

  while(lookAhead >= 0)
    {
      if(matchLen > lookAhead)
        matchLen = lookAhead;

      if(matchLen <= BREAK_EVEN)
        {
          WriteBits(bs, 1, (uint32_t) window[currentPos], 8);
          replaceCnt = 1;
        }
      else
        {
          temp = (matchPos << LENGTH_BIT_COUNT) | (matchLen - (BREAK_EVEN + 1));
          WriteBits(bs, 0, temp, INDEX_BIT_COUNT + LENGTH_BIT_COUNT);
          replaceCnt = matchLen;
        }

      while(replaceCnt--)
        {
          DeleteString(tree, MOD_WINDOW(currentPos + LOOK_AHEAD_SIZE));
          lookAhead--;
        newData:
          currentPos = MOD_WINDOW(currentPos + 1);

          if(lookAhead)
            matchLen = AddString(tree, window, currentPos, &matchPos);
        }
    }
}

int
DeleteCompressor(Compressor *comp)
{
  if(!comp || (comp->ch_Cookie != comp))
    return (COMP_ERR_BADPTR);

  comp->ch_Cookie = NULL;

  FlushCompressor(comp);
  WriteBits(&comp->ch_BitStream, 0, END_OF_STREAM, INDEX_BIT_COUNT);
  CleanupBitStream(&comp->ch_BitStream);

  if(comp->ch_AllocatedStructure)
    free(comp);

  return (0);
}

/*
 * This is the compression routine. It has to first load up the look
 * ahead buffer, then go into the main compression loop. The main loop
 * decides whether to output a single character or an index/length
 * token that defines a phrase. Once the character or phrase has been
 * sent out, another loop has to run. The second loop reads in new
 * characters, deletes the strings that are overwritten by the new
 * character, then adds the strings that are created by the new
 * character.
 */

int
FeedCompressor(Compressor *comp,
               void       *data,
               uint32_t    numDataWords)
{
  int32_t            lookAhead;
  uint32_t           currentPos;
  uint32_t           replaceCnt;
  int32_t            matchLen;
  uint32_t           matchPos;
  uint8_t           *src;
  CompNode          *tree;
  unsigned char     *window;
  CompressBitStream *bs;
  uint32_t           numDataBytes;
  uint32_t           temp;

  if(!comp || (comp->ch_Cookie != comp))
    return (COMP_ERR_BADPTR);

  tree         = comp->ch_Tree;
  window       = comp->ch_Window;
  bs           = &comp->ch_BitStream;
  lookAhead    = comp->ch_LookAhead;
  currentPos   = comp->ch_CurrentPos;
  matchLen     = comp->ch_MatchLen;
  matchPos     = comp->ch_MatchPos;
  replaceCnt   = comp->ch_ReplaceCnt;
  numDataBytes = numDataWords * sizeof(uint32_t);
  src          = (uint8_t*)data;

  if(!numDataBytes)
    return (0);

  if(comp->ch_SecondPass)
    goto newData;

  while(lookAhead <= LOOK_AHEAD_SIZE)
    {
      if(!numDataBytes)
        {
          comp->ch_LookAhead = lookAhead;
          return (0);
        }

      window[lookAhead++] = *src++;
      numDataBytes--;
    }

  lookAhead--;
  while(true)
    {
      if(matchLen > lookAhead)
        matchLen = lookAhead;

      if(matchLen <= BREAK_EVEN)
        {
          WriteBits(bs, 1, (uint32_t) window[currentPos], 8);
          replaceCnt = 1;
        }
      else
        {
          temp = (matchPos << LENGTH_BIT_COUNT) | (matchLen - (BREAK_EVEN + 1));
          WriteBits(bs, 0, temp, INDEX_BIT_COUNT + LENGTH_BIT_COUNT);
          replaceCnt = matchLen;
        }

      while(replaceCnt--)
        {
          DeleteString(tree, MOD_WINDOW(currentPos + LOOK_AHEAD_SIZE));

          if(!numDataBytes)
            {
              /* We ran out of data. Save all the state, and exit. If
               * we are called with more data, we'll jump right back in
               * this loop, and continue processing
               */

              comp->ch_LookAhead  = lookAhead;
              comp->ch_CurrentPos = currentPos;
              comp->ch_MatchLen   = matchLen;
              comp->ch_MatchPos   = matchPos;
              comp->ch_ReplaceCnt = replaceCnt;
              comp->ch_SecondPass = true;
              return (0);
            }
        newData:
          window[MOD_WINDOW(currentPos + LOOK_AHEAD_SIZE)] = *src++;
          numDataBytes--;

          currentPos = MOD_WINDOW(currentPos + 1);

          if(lookAhead)
            matchLen = AddString(tree, window, currentPos, &matchPos);
        }
    }
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

int32_t
GetCompressorWorkBufferSize()
{
  return sizeof(Compressor);
}

int
SimpleCompress(void     *source_,
               uint32_t  sourceWords_,
               void     *result_,
               uint32_t  resultWords_)
{
  int err;
  Compressor *comp;
  Context ctx;

  ctx.dest = (uint32_t*)result_;
  ctx.max  = (uint32_t*)((uint64_t)result_ + resultWords_ * sizeof(uint32_t));
  ctx.overflow = false;

  err = CreateCompressor(&comp,(CompFunc)PutWord,NULL,(void*)&ctx);
  if(err < 0)
    return err;

  FeedCompressor(comp,source_,sourceWords_);

  err = DeleteCompressor(comp);
  if(err == 0)
    {
      if(ctx.overflow)
        err = COMP_ERR_OVERFLOW;
      else
        err = ((uint64_t)ctx.dest - (uint64_t)result_) / sizeof(uint32_t);
    }

  return err;
}
