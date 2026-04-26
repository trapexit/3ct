#include "compress.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/*****************************************************************************/


/*
 * This is the compression module which implements an LZ77-style (LZSS)
 * compression algorithm. As implemented here it uses a 12 bit index into the
 * sliding window, and a 4 bit length, which is adjusted to reflect phrase
 * lengths of between 3 and 18 bytes.
 */


/*****************************************************************************/

#define INDEX_BIT_COUNT  12
#define LENGTH_BIT_COUNT 4
#define WINDOW_SIZE      (1 << INDEX_BIT_COUNT)
#define BREAK_EVEN       2
#define END_OF_STREAM    0
#define MOD_WINDOW(a)    ((a) & (WINDOW_SIZE - 1))

#define LOOK_AHEAD_SIZE  ((1 << LENGTH_BIT_COUNT) + BREAK_EVEN)
#define TREE_ROOT        WINDOW_SIZE
#define UNUSED           0

#define COMP_TRUE        1
#define COMP_FALSE       0

typedef unsigned char uint8_t;

/*
 * The tree[] structure contains the binary tree of all of the strings in the
 * window sorted in order.
 */
typedef struct CompNode
{
  CompUInt32 cn_Parent;
  CompUInt32 cn_LeftChild;
  CompUInt32 cn_RightChild;
} CompNode;

typedef struct CompressBitStream
{
  CompFunc       bs_OutputWord;
  void          *bs_UserData;
  CompUInt32     bs_BitsLeft;
  CompUInt32     bs_BitBuffer;
} CompressBitStream;


/*****************************************************************************/


struct Compressor
{
  unsigned char      ch_Window[WINDOW_SIZE];
  CompNode           ch_Tree[WINDOW_SIZE + 1];
  CompInt32          ch_LookAhead;
  CompInt32          ch_MatchLen;
  CompUInt32         ch_MatchPos;
  CompUInt32         ch_CurrentPos;
  CompUInt32         ch_ReplaceCnt;
  CompressBitStream  ch_BitStream;
  int                ch_SecondPass;
  int                ch_AllocatedStructure;
  void              *ch_Cookie;
};


/*****************************************************************************/

static
CompUInt32
Byteswap32(CompUInt32 v)
{
  return (((v & 0x000000FFU) << 24) |
          ((v & 0x0000FF00U) <<  8) |
          ((v & 0x00FF0000U) >>  8) |
          ((v & 0xFF000000U) >> 24));
}

static
int
IsLittleEndian(void)
{
  union
  {
    CompUInt32 i;
    char c[sizeof(CompUInt32)];
  } u;

  u.i = 0x01020304U;

  return (u.c[0] == 0x04);
}

static
CompUInt32
ByteswapIfLittleEndian(CompUInt32 v)
{
  if(IsLittleEndian())
    return Byteswap32(v);
  return v;
}

/*****************************************************************************/

/* This is where most of the encoder's work is done. This routine is
 * responsible for adding the new node to the binary tree. It also has to
 * find the best match among all the existing nodes in the tree, and return
 * that to the calling routine. To make matters even more complicated, if
 * the newNode has a duplicate in the tree, the oldNode is deleted, for
 * reasons of efficiency.
 */

static
CompUInt32
AddString(CompNode      *tree,
          unsigned char *window,
          CompUInt32     newNode,
          CompUInt32    *matchPos)
{
  CompUInt32  i;
  CompUInt32  testNode;
  CompUInt32  parentNode;
  CompInt32   delta;
  CompUInt32  matchLen;
  CompNode   *node;
  CompNode   *parent;
  CompNode   *test;
  CompUInt32 *child;

  if(newNode == END_OF_STREAM)
    return (0);

  testNode = tree[TREE_ROOT].cn_RightChild;
  node     = &tree[newNode];
  matchLen = 0;

  for(;;)
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
        child = &test->cn_RightChild;
      else
        child = &test->cn_LeftChild;

      if(*child == UNUSED)
        {
          *child              = newNode;
          node->cn_Parent     = testNode;
          node->cn_LeftChild  = UNUSED;
          node->cn_RightChild = UNUSED;
          return (matchLen);
        }

      testNode = *child;
    }
}

static
void
InitBitStream(CompressBitStream *bs,
              CompFunc           cf,
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
                         ByteswapIfLittleEndian(bs->bs_BitBuffer));
}

/* This routine outputs a single header bit, followed by numBits of code */
static
void
WriteBits(CompressBitStream *bs,
          CompUInt32         headBit,
          CompUInt32         code,
          CompUInt32         numBits)
{
  bs->bs_BitsLeft--;
  bs->bs_BitBuffer |= (headBit << bs->bs_BitsLeft);

  if(numBits >= bs->bs_BitsLeft)
    {
      numBits         -= bs->bs_BitsLeft;
      (*bs->bs_OutputWord)(bs->bs_UserData,
                           ByteswapIfLittleEndian(((code >> numBits) | bs->bs_BitBuffer)));
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
             CompUInt32 node)
{
  CompUInt32 parent;
  CompUInt32 newNode;
  CompUInt32 next;

  parent = tree[node].cn_Parent;
  if(parent == UNUSED)
    return;

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

static
void
WriteNextToken(CompressBitStream *bs,
               unsigned char     *window,
               CompUInt32         currentPos,
               CompInt32         *matchLen,
               CompInt32          lookAhead,
               CompUInt32         matchPos,
               CompUInt32        *replaceCnt)
{
  CompUInt32 temp;

  if(*matchLen > lookAhead)
    *matchLen = lookAhead;

  if(*matchLen <= BREAK_EVEN)
    {
      WriteBits(bs, 1, (CompUInt32) window[currentPos], 8);
      *replaceCnt = 1;
    }
  else
    {
      temp = (matchPos << LENGTH_BIT_COUNT) | (*matchLen - (BREAK_EVEN + 1));
      WriteBits(bs, 0, temp, INDEX_BIT_COUNT + LENGTH_BIT_COUNT);
      *replaceCnt = *matchLen;
    }
}

int
CreateCompressor(Compressor **comp,
                 CompFunc     cf,
                 void        *workbuf_,
                 void        *userdata_)
{
  int   allocated;
  void *buffer;
  void *userData;

  if(!comp)
    return COMP_ERR_BADPTR;

  *comp = NULL;

  if(!cf)
    return COMP_ERR_BADPTR;

  buffer   = workbuf_;
  userData = userdata_;

  allocated = COMP_FALSE;
  if(!buffer)
    {
      buffer = malloc(sizeof(Compressor));
      if(!buffer)
        return COMP_ERR_NOMEM;

      allocated = COMP_TRUE;
    }

  (*comp)                        = (Compressor*)buffer;
  (*comp)->ch_LookAhead          = 1;
  (*comp)->ch_CurrentPos         = 1;
  (*comp)->ch_MatchPos           = 0;
  (*comp)->ch_MatchLen           = 0;
  (*comp)->ch_ReplaceCnt         = 0;
  (*comp)->ch_SecondPass         = COMP_FALSE;
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
  CompInt32          lookAhead;
  CompUInt32         currentPos;
  CompUInt32         replaceCnt;
  CompInt32          matchLen;
  CompUInt32         matchPos;
  CompNode          *tree;
  unsigned char     *window;
  CompressBitStream *bs;

  tree         = comp->ch_Tree;
  window       = comp->ch_Window;
  bs           = &comp->ch_BitStream;
  lookAhead    = comp->ch_LookAhead;
  currentPos   = comp->ch_CurrentPos;
  matchLen     = comp->ch_MatchLen;
  matchPos     = comp->ch_MatchPos;
  replaceCnt   = comp->ch_ReplaceCnt;

  /* Continue inside the replacement loop if the previous feed ran out of data. */
  if(comp->ch_SecondPass)
    goto resume_replacement;

  while(lookAhead >= 0)
    {
      WriteNextToken(bs,
                     window,
                     currentPos,
                     &matchLen,
                     lookAhead,
                     matchPos,
                     &replaceCnt);

      while(replaceCnt--)
        {
          DeleteString(tree, MOD_WINDOW(currentPos + LOOK_AHEAD_SIZE));
          lookAhead--;
        resume_replacement:
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
               CompUInt32  numDataWords)
{
  CompInt32          lookAhead;
  CompUInt32         currentPos;
  CompUInt32         replaceCnt;
  CompInt32          matchLen;
  CompUInt32         matchPos;
  uint8_t           *src;
  CompNode          *tree;
  unsigned char     *window;
  CompressBitStream *bs;
  CompUInt32         numDataBytes;

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
  numDataBytes = numDataWords * sizeof(CompUInt32);
  src          = (uint8_t*)data;

  if(!numDataBytes)
    return (0);

  /* Continue inside the replacement loop if the previous feed ran out of data. */
  if(comp->ch_SecondPass)
    goto resume_replacement;

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
  for(;;)
    {
      WriteNextToken(bs,
                     window,
                     currentPos,
                     &matchLen,
                     lookAhead,
                     matchPos,
                     &replaceCnt);

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
              comp->ch_SecondPass = COMP_TRUE;
              return (0);
            }
        resume_replacement:
          window[MOD_WINDOW(currentPos + LOOK_AHEAD_SIZE)] = *src++;
          numDataBytes--;

          currentPos = MOD_WINDOW(currentPos + 1);

          if(lookAhead)
            matchLen = AddString(tree, window, currentPos, &matchPos);
        }
    }
}

typedef struct Context
{
  CompUInt32 *dest;
  CompUInt32 *max;
  int         overflow;
} Context;

static
void
PutWord(void     *ctx__,
        CompUInt32 word_)
{
  Context *ctx_;

  ctx_ = (Context*)ctx__;

  if(ctx_->dest >= ctx_->max)
    ctx_->overflow = COMP_TRUE;
  else
    *ctx_->dest++ = word_;
}

int
GetCompressorWorkBufferSize(void)
{
  return sizeof(Compressor);
}

int
SimpleCompress(void         *source_,
               CompUInt32    sourceWords_,
               void         *result_,
               CompUInt32    resultWords_)
{
  int err;
  Compressor *comp;
  Context ctx;

  ctx.dest = (CompUInt32*)result_;
  ctx.max  = ctx.dest + resultWords_;
  ctx.overflow = COMP_FALSE;

  err = CreateCompressor(&comp,PutWord,NULL,(void*)&ctx);
  if(err < 0)
    return err;

  FeedCompressor(comp,source_,sourceWords_);

  err = DeleteCompressor(comp);
  if(err == 0)
    {
      if(ctx.overflow)
        err = COMP_ERR_OVERFLOW;
      else
        err = ctx.dest - (CompUInt32*)result_;
    }

  return err;
}
