#include "ggc.h"

#include <stdlib.h>
#include <string.h>

#define GGC_BYTE_BITS 8
#define GGC_BYTE_VALUES 256
#define GGC_BYTE_MASK 0xff
#define GGC_WORD_BITS 16
#define GGC_WORD_HIGH_BIT 0x8000
#define GGC_WORD_MASK 0xffff
#define GGC_HEADER_SIZE 6
#define GGC_HEADER_SIZE_OFFSET 0
#define GGC_HEADER_TYPE_OFFSET 2
#define GGC_FILE_TYPE_SIZE 4
#define GGC_FILE_TYPE_INDEX_0 0
#define GGC_FILE_TYPE_INDEX_1 1
#define GGC_FILE_TYPE_INDEX_2 2
#define GGC_FILE_TYPE_INDEX_3 3
#define GGC_DEFAULT_FILE_TYPE_BYTE ' '
#define GGC_INITIAL_OUT_CAPACITY 256
#define GGC_OUT_CAPACITY_GROWTH_FACTOR 2

#define GGC_N 4096
#define GGC_F 60
#define GGC_THRESHOLD 2
#define GGC_N_CHAR (GGC_BYTE_VALUES - GGC_THRESHOLD + GGC_F)
#define GGC_HUFF_BRANCHES 2
#define GGC_T (GGC_N_CHAR * GGC_HUFF_BRANCHES - 1)
#define GGC_R (GGC_T - 1)
#define GGC_MAX_FREQ GGC_WORD_HIGH_BIT
#define GGC_HUFF_SENTINEL_FREQ GGC_WORD_MASK
#define GGC_LITERAL_LIMIT GGC_BYTE_VALUES
#define GGC_MATCH_CODE_BASE (GGC_BYTE_VALUES - GGC_THRESHOLD - 1)
#define GGC_RING_MASK (GGC_N - 1)
#define GGC_TREE_ROOT_FIRST (GGC_N + 1)
#define GGC_TREE_ROOT_LAST (GGC_N + GGC_BYTE_VALUES)
#define GGC_INITIAL_RING_POS (GGC_N - GGC_F)

#define GGC_POSITION_BITS 12
#define GGC_POSITION_HIGH_BITS 6
#define GGC_POSITION_LOW_BITS 6
#define GGC_POSITION_LOW_MASK 0x3f
#define GGC_POSITION_CODE_COUNT (1 << GGC_POSITION_HIGH_BITS)
#define GGC_POSITION_DECODE_COUNT GGC_BYTE_VALUES
#define GGC_POSITION_LEN_ADJUST 2

#define GGC_POSITION_CODE_0 0
#define GGC_POSITION_CODE_1 1
#define GGC_POSITION_CODE_2 2
#define GGC_POSITION_CODE_3 3
#define GGC_POSITION_CODE_4 4
#define GGC_POSITION_CODE_11 11
#define GGC_POSITION_CODE_12 12
#define GGC_POSITION_CODE_23 23
#define GGC_POSITION_CODE_24 24
#define GGC_POSITION_CODE_47 47
#define GGC_POSITION_CODE_48 48
#define GGC_POSITION_CODE_63 63
#define GGC_POSITION_RUN_1 1
#define GGC_POSITION_RUN_2 2
#define GGC_POSITION_RUN_4 4
#define GGC_POSITION_RUN_8 8
#define GGC_POSITION_RUN_16 16
#define GGC_POSITION_RUN_32 32
#define GGC_POSITION_LEN_3 3
#define GGC_POSITION_LEN_4 4
#define GGC_POSITION_LEN_5 5
#define GGC_POSITION_LEN_6 6
#define GGC_POSITION_LEN_7 7
#define GGC_POSITION_LEN_8 8
#define GGC_POSITION_LEN_RUN_48 48
#define GGC_POSITION_LEN_RUN_64 64

typedef struct GGCDecoder GGCDecoder;
struct GGCDecoder
{
  const unsigned char *src;
  size_t src_size;
  size_t src_pos;
  int underflow;
  unsigned short bit_buf;
  int bit_count;
  unsigned char text_buf[GGC_N + GGC_F - 1];
  int freq[GGC_T + 1];
  int parent[GGC_T + GGC_N_CHAR];
  int child[GGC_T];
  unsigned char position_code[GGC_POSITION_DECODE_COUNT];
  unsigned char position_len[GGC_POSITION_DECODE_COUNT];
};

typedef struct GGCEncoder GGCEncoder;
struct GGCEncoder
{
  const unsigned char *src;
  size_t src_size;
  size_t input_pos;
  unsigned char *out;
  size_t out_size;
  size_t out_capacity;
  unsigned char bit_buf;
  int bit_count;
  int match_position;
  int match_len;
  unsigned char text_buf[GGC_N + GGC_F - 1];
  int lson[GGC_N + 1];
  int rson[GGC_N + GGC_BYTE_VALUES + 1];
  int dad[GGC_N + 1];
  int freq[GGC_T + 1];
  int parent[GGC_T + GGC_N_CHAR];
  int child[GGC_T];
  unsigned char position_code[GGC_POSITION_CODE_COUNT];
  unsigned char position_len[GGC_POSITION_CODE_COUNT];
};

static
void
ggc_fill_run_256(unsigned char *table,
                 int           *pos,
                 unsigned char  value,
                 int            count)
{
  int i;

  for(i = 0; i < count; i++)
    table[(*pos)++] = value;
}

static
void
ggc_init_decode_position_tables(unsigned char *position_code,
                                unsigned char *position_len)
{
  int pos;
  int v;

  pos = 0;
  ggc_fill_run_256(position_code,&pos,GGC_POSITION_CODE_0,GGC_POSITION_RUN_32);
  ggc_fill_run_256(position_code,&pos,GGC_POSITION_CODE_1,GGC_POSITION_RUN_16);
  ggc_fill_run_256(position_code,&pos,GGC_POSITION_CODE_2,GGC_POSITION_RUN_16);
  ggc_fill_run_256(position_code,&pos,GGC_POSITION_CODE_3,GGC_POSITION_RUN_16);
  for(v = GGC_POSITION_CODE_4; v <= GGC_POSITION_CODE_11; v++)
    ggc_fill_run_256(position_code,&pos,(unsigned char)v,GGC_POSITION_RUN_8);
  for(v = GGC_POSITION_CODE_12; v <= GGC_POSITION_CODE_23; v++)
    ggc_fill_run_256(position_code,&pos,(unsigned char)v,GGC_POSITION_RUN_4);
  for(v = GGC_POSITION_CODE_24; v <= GGC_POSITION_CODE_47; v++)
    ggc_fill_run_256(position_code,&pos,(unsigned char)v,GGC_POSITION_RUN_2);
  for(v = GGC_POSITION_CODE_48; v <= GGC_POSITION_CODE_63; v++)
    ggc_fill_run_256(position_code,&pos,(unsigned char)v,GGC_POSITION_RUN_1);

  pos = 0;
  ggc_fill_run_256(position_len,&pos,GGC_POSITION_LEN_3,GGC_POSITION_RUN_32);
  ggc_fill_run_256(position_len,&pos,GGC_POSITION_LEN_4,GGC_POSITION_LEN_RUN_48);
  ggc_fill_run_256(position_len,&pos,GGC_POSITION_LEN_5,GGC_POSITION_LEN_RUN_64);
  ggc_fill_run_256(position_len,&pos,GGC_POSITION_LEN_6,GGC_POSITION_LEN_RUN_48);
  ggc_fill_run_256(position_len,&pos,GGC_POSITION_LEN_7,GGC_POSITION_LEN_RUN_48);
  ggc_fill_run_256(position_len,&pos,GGC_POSITION_LEN_8,GGC_POSITION_RUN_16);
}

static
void
ggc_start_huff(int *freq,
               int *parent,
               int *child)
{
  int i;
  int j;

  for(i = 0; i < GGC_N_CHAR; i++)
    {
      freq[i] = 1;
      child[i] = i + GGC_T;
      parent[i + GGC_T] = i;
    }

  i = 0;
  for(j = GGC_N_CHAR; j <= GGC_R; j++)
    {
      freq[j] = freq[i] + freq[i + 1];
      child[j] = i;
      parent[i] = j;
      parent[i + 1] = j;
      i += GGC_HUFF_BRANCHES;
    }

  freq[GGC_T] = GGC_HUFF_SENTINEL_FREQ;
  parent[GGC_R] = -1;
}

static
void
ggc_reconst(int *freq,
            int *parent,
            int *child)
{
  int i;
  int j;

  j = 0;
  for(i = 0; i < GGC_T; i++)
    {
      if(child[i] >= GGC_T)
        {
          freq[j] = (freq[i] + 1) / GGC_HUFF_BRANCHES;
          child[j] = child[i];
          j++;
        }
    }

  i = 0;
  for(j = GGC_N_CHAR; j < GGC_T; j++)
    {
      int f;
      int k;
      int n;

      k = i + 1;
      f = freq[i] + freq[k];
      freq[j] = f;
      k = j - 1;
      while(f < freq[k])
        k--;
      k++;

      for(n = j - k; n > 0; n--)
        {
          freq[k + n] = freq[k + n - 1];
          child[k + n] = child[k + n - 1];
        }
      freq[k] = f;
      child[k] = i;
      i += GGC_HUFF_BRANCHES;
    }

  for(i = 0; i < GGC_T; i++)
    {
      int k;

      k = child[i];
      if(k >= GGC_T)
        parent[k] = i;
      else
        {
          parent[k] = i;
          parent[k + 1] = i;
        }
    }

  parent[GGC_R] = -1;
}

static
void
ggc_update(int *freq,
           int *parent,
           int *child,
           int  c_)
{
  int c;

  if(freq[GGC_R] == GGC_MAX_FREQ)
    ggc_reconst(freq,parent,child);

  c = parent[c_ + GGC_T];
  while(c >= 0)
    {
      int k;
      int l;

      freq[c]++;
      k = freq[c];
      l = c + 1;
      if(k > freq[l])
        {
          int i;
          int j;
          int tmp;

          while(k > freq[l + 1])
            l++;

          tmp = freq[c];
          freq[c] = freq[l];
          freq[l] = tmp;

          i = child[c];
          parent[i] = l;
          if(i < GGC_T)
            parent[i + 1] = l;

          j = child[l];
          child[l] = i;
          parent[j] = c;
          if(j < GGC_T)
            parent[j + 1] = c;

          child[c] = j;
          c = l;
        }

      c = parent[c];
    }
}

static
unsigned char
ggc_decoder_get_byte(GGCDecoder *d)
{
  if(d->src_pos >= d->src_size)
    {
      d->underflow = 1;
      return 0;
    }

  return (unsigned char)(d->src[d->src_pos++] ^ GGC_BYTE_MASK);
}

static
int
ggc_decoder_get_bit(GGCDecoder *d)
{
  int bit;

  while(d->bit_count <= GGC_BYTE_BITS)
    {
      d->bit_buf = (unsigned short)(d->bit_buf | (ggc_decoder_get_byte(d) << (GGC_BYTE_BITS - d->bit_count)));
      d->bit_count += GGC_BYTE_BITS;
    }

  bit = (d->bit_buf & GGC_WORD_HIGH_BIT) ? 1 : 0;
  d->bit_buf = (unsigned short)(d->bit_buf << 1);
  d->bit_count--;
  return bit;
}

static
unsigned
char
ggc_decoder_get_byte_bits(GGCDecoder *d)
{
  unsigned char value;

  while(d->bit_count <= GGC_BYTE_BITS)
    {
      d->bit_buf = (unsigned short)(d->bit_buf | (ggc_decoder_get_byte(d) << (GGC_BYTE_BITS - d->bit_count)));
      d->bit_count += GGC_BYTE_BITS;
    }

  value = (unsigned char)(d->bit_buf >> GGC_BYTE_BITS);
  d->bit_buf = (unsigned short)(d->bit_buf << GGC_BYTE_BITS);
  d->bit_count -= GGC_BYTE_BITS;
  return value;
}

static
int
ggc_decode_char(GGCDecoder *d)
{
  int c;

  c = d->child[GGC_R];
  while(c < GGC_T)
    c = d->child[c + ggc_decoder_get_bit(d)];

  c -= GGC_T;
  ggc_update(d->freq,d->parent,d->child,c);

  return c;
}

static
int
ggc_decode_position(GGCDecoder *d)
{
  int value;
  int code;
  int i;

  value = ggc_decoder_get_byte_bits(d);
  code = d->position_code[value] << GGC_POSITION_LOW_BITS;
  for(i = d->position_len[value] - GGC_POSITION_LEN_ADJUST; i > 0; i--)
    value = ((value << 1) | ggc_decoder_get_bit(d)) & GGC_WORD_MASK;

  return code | (value & GGC_POSITION_LOW_MASK);
}

static
int
ggc_push_byte(GGCEncoder    *e,
              unsigned char  b)
{
  unsigned char *new_out;
  size_t new_capacity;

  if(e->out_size >= e->out_capacity)
    {
      new_capacity = (e->out_capacity == 0) ? GGC_INITIAL_OUT_CAPACITY : (e->out_capacity * GGC_OUT_CAPACITY_GROWTH_FACTOR);
      new_out = (unsigned char *)realloc(e->out,new_capacity);
      if(new_out == NULL)
        return GGC_ERR_NOMEM;
      e->out = new_out;
      e->out_capacity = new_capacity;
    }

  e->out[e->out_size++] = b;

  return GGC_OK;
}

static
int
ggc_put_bit(GGCEncoder *e,
            int         bit)
{
  e->bit_buf = (unsigned char)((e->bit_buf << 1) | (bit & 1));
  e->bit_count++;
  if(e->bit_count == GGC_BYTE_BITS)
    {
      int rv;

      rv = ggc_push_byte(e,(unsigned char)(e->bit_buf ^ GGC_BYTE_MASK));
      if(rv != GGC_OK)
        return rv;
      e->bit_buf = 0;
      e->bit_count = 0;
    }

  return GGC_OK;
}

static
int
ggc_put_bits(GGCEncoder   *e,
             unsigned int  value,
             int           count)
{
  int i;
  int rv;

  for(i = count - 1; i >= 0; i--)
    {
      rv = ggc_put_bit(e,(value >> i) & 1);
      if(rv != GGC_OK)
        return rv;
    }

  return GGC_OK;
}

static
int
ggc_flush_bits(GGCEncoder *e)
{
  if(e->bit_count != 0)
    {
      e->bit_buf = (unsigned char)(e->bit_buf << (GGC_BYTE_BITS - e->bit_count));
      return ggc_push_byte(e,(unsigned char)(e->bit_buf ^ GGC_BYTE_MASK));
    }

  return GGC_OK;
}

static
void
ggc_init_encode_position_tables(unsigned char *position_code,
                                unsigned char *position_len)
{
  unsigned char decode_code[GGC_POSITION_DECODE_COUNT];
  unsigned char decode_len[GGC_POSITION_DECODE_COUNT];
  unsigned char seen[GGC_POSITION_CODE_COUNT];
  int i;
  int code;

  memset(decode_code,0,sizeof(decode_code));
  memset(decode_len,0,sizeof(decode_len));
  memset(seen,0,sizeof(seen));
  ggc_init_decode_position_tables(decode_code,decode_len);

  for(i = 0; i < GGC_POSITION_DECODE_COUNT; i++)
    {
      code = decode_code[i];
      if(!seen[code])
        {
          position_code[code] = (unsigned char)i;
          position_len[code] = decode_len[i];
          seen[code] = 1;
        }
    }
}

static
void
ggc_init_tree(GGCEncoder *e)
{
  int i;

  for(i = GGC_TREE_ROOT_FIRST; i <= GGC_TREE_ROOT_LAST; i++)
    e->rson[i] = GGC_N;
  for(i = 0; i < GGC_N; i++)
    e->dad[i] = GGC_N;
}

static
int
ggc_encode_char(GGCEncoder *e,
                int         c)
{
  unsigned int code;
  int len;
  int node;
  int rv;

  code = 0;
  len = 0;
  node = e->parent[c + GGC_T];
  do
    {
      code >>= 1;
      if(node & 1)
        code += GGC_WORD_HIGH_BIT;
      len++;
      node = e->parent[node];
    }
  while(node != GGC_R);

  rv = ggc_put_bits(e,code >> (GGC_WORD_BITS - len),len);
  if(rv != GGC_OK)
    return rv;

  ggc_update(e->freq,e->parent,e->child,c);
  return GGC_OK;
}

static
int
ggc_encode_position(GGCEncoder *e,
                    int         position)
{
  int high;
  int low;
  int extra_bits;
  unsigned char prefix;
  int rv;

  high = (position >> GGC_POSITION_LOW_BITS) & GGC_POSITION_LOW_MASK;
  low = position & GGC_POSITION_LOW_MASK;
  extra_bits = e->position_len[high] - GGC_POSITION_LEN_ADJUST;
  prefix = (unsigned char)(e->position_code[high] + (low >> extra_bits));

  rv = ggc_put_bits(e,prefix,GGC_BYTE_BITS);
  if(rv != GGC_OK)
    return rv;
  return ggc_put_bits(e,(unsigned int)low,extra_bits);
}

static
void
ggc_insert_node(GGCEncoder *e,
                int         r)
{
  int cmp;
  int key;
  int p;

  cmp = 1;
  key = r;
  p = GGC_TREE_ROOT_FIRST + e->text_buf[key];
  e->rson[r] = GGC_N;
  e->lson[r] = GGC_N;
  e->match_len = 0;

  for(;;)
    {
      int i;

      if(cmp >= 0)
        {
          if(e->rson[p] != GGC_N)
            p = e->rson[p];
          else
            {
              e->rson[p] = r;
              e->dad[r] = p;
              return;
            }
        }
      else
        {
          if(e->lson[p] != GGC_N)
            p = e->lson[p];
          else
            {
              e->lson[p] = r;
              e->dad[r] = p;
              return;
            }
        }

      for(i = 1; i < GGC_F; i++)
        {
          cmp = e->text_buf[key + i] - e->text_buf[p + i];
          if(cmp != 0)
            break;
        }

      if(i > GGC_THRESHOLD)
        {
          int distance;

          distance = ((r - p) & GGC_RING_MASK) - 1;
          if((i > e->match_len) || ((i == e->match_len) && (distance < e->match_position)))
            {
              e->match_position = distance;
              e->match_len = i;
              if(e->match_len >= GGC_F)
                break;
            }
        }
    }

  e->dad[r] = e->dad[p];
  e->lson[r] = e->lson[p];
  e->rson[r] = e->rson[p];
  e->dad[e->lson[p]] = r;
  e->dad[e->rson[p]] = r;

  if(e->rson[e->dad[p]] == p)
    e->rson[e->dad[p]] = r;
  else
    e->lson[e->dad[p]] = r;

  e->dad[p] = GGC_N;
}

static
void
ggc_delete_node(GGCEncoder *e,
                int         p)
{
  int q;

  if(e->dad[p] == GGC_N)
    return;

  if(e->rson[p] == GGC_N)
    q = e->lson[p];
  else if(e->lson[p] == GGC_N)
    q = e->rson[p];
  else
    {
      q = e->lson[p];
      if(e->rson[q] != GGC_N)
        {
          do
            q = e->rson[q];
          while(e->rson[q] != GGC_N);

          e->rson[e->dad[q]] = e->lson[q];
          e->dad[e->lson[q]] = e->dad[q];
          e->lson[q] = e->lson[p];
          e->dad[e->lson[p]] = q;
        }

      e->rson[q] = e->rson[p];
      e->dad[e->rson[p]] = q;
    }

  e->dad[q] = e->dad[p];
  if(e->rson[e->dad[p]] == p)
    e->rson[e->dad[p]] = q;
  else
    e->lson[e->dad[p]] = q;

  e->dad[p] = GGC_N;
}

static
int
ggc_do_encode(GGCEncoder *e)
{
  int len;
  int last_match_len;
  int r;
  int s;
  int i;
  int rv;

  ggc_init_tree(e);
  e->input_pos = 0;
  s = 0;
  r = GGC_INITIAL_RING_POS;
  len = 0;

  while((len < GGC_F) && (e->input_pos < e->src_size))
    e->text_buf[r + len++] = (unsigned char)(e->src[e->input_pos++] ^ GGC_BYTE_MASK);

  if(len == 0)
    return ggc_flush_bits(e);

  for(i = 1; i <= GGC_F; i++)
    ggc_insert_node(e,r - i);
  ggc_insert_node(e,r);

  do
    {
      if(e->match_len > len)
        e->match_len = len;

      if(e->match_len <= GGC_THRESHOLD)
        {
          e->match_len = 1;
          rv = ggc_encode_char(e,e->text_buf[r]);
        }
      else
        {
          rv = ggc_encode_char(e,e->match_len + GGC_MATCH_CODE_BASE);
          if(rv == GGC_OK)
            rv = ggc_encode_position(e,e->match_position);
        }
      if(rv != GGC_OK)
        return rv;

      last_match_len = e->match_len;
      i = 0;
      while((i < last_match_len) && (e->input_pos < e->src_size))
        {
          unsigned char c;

          c = (unsigned char)(e->src[e->input_pos++] ^ GGC_BYTE_MASK);
          ggc_delete_node(e,s);
          e->text_buf[s] = c;
          if(s < GGC_F - 1)
            e->text_buf[s + GGC_N] = c;
          s = (s + 1) & GGC_RING_MASK;
          r = (r + 1) & GGC_RING_MASK;
          ggc_insert_node(e,r);
          i++;
        }

      while(i < last_match_len)
        {
          ggc_delete_node(e,s);
          s = (s + 1) & GGC_RING_MASK;
          r = (r + 1) & GGC_RING_MASK;
          len--;
          if(len != 0)
            ggc_insert_node(e,r);
          i++;
        }
    }
  while(len > 0);

  return ggc_flush_bits(e);
}

void
ggc_free(void *ptr)
{
  free(ptr);
}

int
ggc_decompress(const unsigned char  *src,
               size_t                src_size,
               unsigned char       **dst,
               size_t               *dst_size)
{
  GGCDecoder d;
  unsigned char *out;
  size_t output_size;
  size_t out_pos;
  int r;

  if(src == NULL || dst == NULL || dst_size == NULL)
    return GGC_ERR_BADPTR;
  if(src_size < GGC_HEADER_SIZE)
    return GGC_ERR_BADSIZE;

  output_size = ((size_t)src[GGC_HEADER_SIZE_OFFSET] << GGC_BYTE_BITS) |
                src[GGC_HEADER_SIZE_OFFSET + 1];
  if(output_size != 0 && src_size == GGC_HEADER_SIZE)
    return GGC_ERR_TRUNCATED;

  out = (unsigned char *)malloc(output_size ? output_size : 1);
  if(out == NULL)
    return GGC_ERR_NOMEM;

  memset(&d,0,sizeof(d));
  d.src = src + GGC_HEADER_SIZE;
  d.src_size = src_size - GGC_HEADER_SIZE;
  memset(d.text_buf,GGC_DEFAULT_FILE_TYPE_BYTE,sizeof(d.text_buf));
  ggc_init_decode_position_tables(d.position_code,d.position_len);
  ggc_start_huff(d.freq,d.parent,d.child);

  out_pos = 0;
  r = GGC_INITIAL_RING_POS;
  while(out_pos < output_size)
    {
      int c;

      c = ggc_decode_char(&d);
      if(c < GGC_LITERAL_LIMIT)
        {
          d.text_buf[r] = (unsigned char)c;
          out[out_pos++] = (unsigned char)(c ^ GGC_BYTE_MASK);
          r = (r + 1) & GGC_RING_MASK;
        }
      else
        {
          int i;
          int length;
          int k;

          i = (r - ggc_decode_position(&d) - 1) & GGC_RING_MASK;
          length = c - GGC_MATCH_CODE_BASE;
          for(k = 0; (k < length) && (out_pos < output_size); k++)
            {
              unsigned char b;

              b = d.text_buf[(i + k) & GGC_RING_MASK];
              d.text_buf[r] = b;
              out[out_pos++] = (unsigned char)(b ^ GGC_BYTE_MASK);
              r = (r + 1) & GGC_RING_MASK;
            }
        }

      if(d.underflow && (out_pos < output_size))
        {
          free(out);
          return GGC_ERR_TRUNCATED;
        }
    }

  *dst = out;
  *dst_size = output_size;
  return GGC_OK;
}

int
ggc_compress(const unsigned char  *src,
             size_t                src_size,
             const unsigned char   file_type[GGC_FILE_TYPE_SIZE],
             unsigned char       **dst,
             size_t               *dst_size)
{
  GGCEncoder e;
  unsigned char type[GGC_FILE_TYPE_SIZE];
  unsigned char *result;
  size_t i;
  int rv;

  if(((src == NULL) && (src_size != 0)) || dst == NULL || dst_size == NULL)
    return GGC_ERR_BADPTR;
  if(src_size > GGC_WORD_MASK)
    return GGC_ERR_BADSIZE;

  if(file_type != NULL)
    memcpy(type,file_type,GGC_FILE_TYPE_SIZE);
  else
    {
      type[GGC_FILE_TYPE_INDEX_0] = GGC_DEFAULT_FILE_TYPE_BYTE;
      type[GGC_FILE_TYPE_INDEX_1] = GGC_DEFAULT_FILE_TYPE_BYTE;
      type[GGC_FILE_TYPE_INDEX_2] = GGC_DEFAULT_FILE_TYPE_BYTE;
      type[GGC_FILE_TYPE_INDEX_3] = GGC_DEFAULT_FILE_TYPE_BYTE;
    }

  memset(&e,0,sizeof(e));
  e.src = src;
  e.src_size = src_size;
  memset(e.text_buf,GGC_DEFAULT_FILE_TYPE_BYTE,sizeof(e.text_buf));
  ggc_init_encode_position_tables(e.position_code,e.position_len);
  ggc_start_huff(e.freq,e.parent,e.child);

  rv = ggc_do_encode(&e);
  if(rv != GGC_OK)
    {
      free(e.out);
      return rv;
    }

  result = (unsigned char *)malloc(e.out_size + GGC_HEADER_SIZE);
  if(result == NULL)
    {
      free(e.out);
      return GGC_ERR_NOMEM;
    }

  result[GGC_HEADER_SIZE_OFFSET] = (unsigned char)((src_size >> GGC_BYTE_BITS) & GGC_BYTE_MASK);
  result[GGC_HEADER_SIZE_OFFSET + 1] = (unsigned char)(src_size & GGC_BYTE_MASK);
  for(i = 0; i < GGC_FILE_TYPE_SIZE; i++)
    result[i + GGC_HEADER_TYPE_OFFSET] = (unsigned char)(type[i] ^ GGC_BYTE_MASK);
  memcpy(result + GGC_HEADER_SIZE,e.out,e.out_size);
  *dst = result;
  *dst_size = e.out_size + GGC_HEADER_SIZE;

  free(e.out);
  return GGC_OK;
}
