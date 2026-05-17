# Game Guru GGC Compression Format

This document describes the Game Guru `.COMP` format supported by
`3ct ggc-compress` and `3ct ggc-decompress`. The format is unrelated to the raw
3DO SDK `comp3do` stream documented in `3do-compression-format.md`.

Game Guru `.COMP` files have a 6-byte wrapper followed by a byte-inverted
adaptive-Huffman/LZSS payload. The payload is close to Haruhiko Okumura's
`lzhuf` design, but byte inversion, header layout, root-parent handling, and
match-position details must match Game Guru for byte-exact output.

The details here are derived from `src/ggc.c` and the CLI wrappers in
`src/subcmd_ggc_compress.cpp` and `src/subcmd_ggc_decompress.cpp`.

## Format Summary

| Property | Value |
| --- | --- |
| File extension convention | `.COMP` |
| Header size | 6 bytes. |
| Stored decompressed size | Unsigned 16-bit big-endian byte count. |
| Maximum decompressed size | 65535 bytes. |
| Header file-type field | 4 bytes, each XORed with `0xff`. |
| Payload byte transform | Every stored payload byte is XORed with `0xff`. |
| Internal data transform | Source bytes are XORed with `0xff` before LZ/Huffman coding; decoded bytes are XORed with `0xff` before output. |
| Entropy coding | Adaptive binary Huffman over 314 symbols. |
| Dictionary coding | LZSS ring buffer, 4096 bytes, max match length 60. |
| End-of-stream marker | None. Decoder stops after header byte count is produced. |
| Bit order | Most-significant bit first inside the de-inverted payload bytes. |

## Container Layout

```text
offset  size  field
0x00    2     decompressed_size_be
0x02    4     file_type_xor_ff
0x06    ...   compressed_payload_xor_ff
```

Header fields:

| Offset | Size | Encoding | Meaning |
| ---: | ---: | --- | --- |
| `0x00` | 2 | Plain big-endian | Decompressed byte count, `0..65535`. These two bytes are not XORed. |
| `0x02` | 4 | Each byte XOR `0xff` | Original 3DO filesystem file type. This is metadata and is not used by the decompressor. |
| `0x06` | rest | Each byte XOR `0xff` | Adaptive-Huffman/LZSS bitstream. |

The decompressor in this repository ignores the decoded file type and only uses
the size field and payload. A compressor must still write the file-type field to
produce Game Guru-compatible files.

Examples of file-type encoding:

| Decoded type | ASCII bytes | Header bytes |
| --- | --- | --- |
| Four spaces | `20 20 20 20` | `df df df df` |
| `NVRT` | `4e 56 52 54` | `b1 a9 ad ab` |

An empty file with default four-space type is exactly six bytes:

```text
00 00 df df df df
```

## File Type Selection In `3ct`

The low-level `ggc_compress` API accepts an optional 4-byte file type. If it is
`NULL`, it uses four spaces.

The `3ct ggc-compress` command defaults to four spaces unless the first four
input bytes look like a plausible 3DO filesystem type. A byte is considered
plausible if it is `A..Z`, `0..9`, or a space. The `--file-type XXXX` option
overrides this and must be exactly four bytes.

This inference is CLI policy, not part of the compression payload.

## Byte Inversion Layers

There are two distinct XOR transforms.

| Location | Transform |
| --- | --- |
| Header file type | Stored byte = file_type_byte XOR `0xff`. |
| Payload storage | Stored byte = bitstream_byte XOR `0xff`. |
| Compressor input to LZ/Huffman | Internal byte = source byte XOR `0xff`. |
| Decompressor output from LZ/Huffman | Output byte = internal byte XOR `0xff`. |

These transforms do not cancel globally. The LZSS window and Huffman literals
operate on the internal inverted byte values, while the physical payload bytes
are also inverted at the bitstream byte boundary.

For example, if the source file contains byte `0x00`, the LZ/Huffman layer sees
literal value `0xff`. The bits representing symbol `255` are packed into bytes,
and each full packed byte is XORed with `0xff` before being written to the
payload.

## Payload Bitstream

After the 6-byte header, each payload byte is de-inverted before bit parsing.

```text
physical_byte = src[pos]
bitstream_byte = physical_byte XOR 0xff
```

Bits are consumed MSB-first from `bitstream_byte`: bit 7, bit 6, ..., bit 0.

The encoder writes bits MSB-first to an 8-bit accumulator. When the accumulator
has 8 bits, it writes `accumulator XOR 0xff` to the payload. If the final
payload byte is partial, the encoder pads the remaining low bits with zeroes,
then XORs the byte with `0xff` and writes it.

There is no EOS token. The decoder stops immediately after producing the number
of output bytes stored in the header, even if unread padding bits or extra
payload bytes remain.

## LZSS Layer Constants

```c
N         = 4096          // ring buffer size
F         = 60            // maximum match length
THRESHOLD = 2             // matches <= 2 are literals
N_CHAR    = 256 - THRESHOLD + F  // 314 symbols
T         = 2 * N_CHAR - 1       // 627
R         = T - 1                // 626, Huffman root
MAX_FREQ  = 0x8000
```

Ring-buffer constants:

```c
RING_MASK        = N - 1       // 0x0fff
INITIAL_RING_POS = N - F       // 4036
INITIAL_BYTE     = 0x20        // space, in the internal inverted domain
```

The initial ring buffer is filled with byte `0x20`. Because actual file bytes
are inverted before coding, an initial-buffer match emits output byte `0xdf`
after the decompressor's final XOR. This is intentional and must not be changed
to `0xdf` internally.

## Huffman Symbols

The adaptive Huffman tree encodes 314 symbols.

| Symbol range | Meaning |
| ---: | --- |
| `0..255` | Literal internal byte value. Store it in the ring buffer and output `symbol XOR 0xff`. |
| `256..313` | Match-length code. Decode a position after the symbol, then copy from the ring buffer. |

Match length mapping:

```text
MATCH_CODE_BASE = 256 - THRESHOLD - 1 = 253
length = symbol - MATCH_CODE_BASE
```

Thus symbol `256` means length `3`, and symbol `313` means length `60`.

The compressor emits a match only when `match_len > THRESHOLD`. Otherwise it
emits the current byte as a literal.

## Match Position Semantics

Match positions are stored as 12-bit distances, but with a static variable-bit
position code described later.

Decoder semantics:

```text
position = decode_position()              // logically 0..4095
source   = (ring_pos - position - 1) & 0x0fff
length   = symbol - 253

for k from 0 to length - 1:
    b = text_buf[(source + k) & 0x0fff]
    text_buf[ring_pos] = b
    output b XOR 0xff
    ring_pos = (ring_pos + 1) & 0x0fff
```

`position == 0` copies from the byte immediately before `ring_pos`. Larger
positions copy farther back in the ring buffer. Copies are overlap-aware because
each byte is written back to the ring before the next byte is copied.

The byte-exact compressor computes candidate position as:

```text
distance = ((r - candidate_node) & 0x0fff) - 1
```

When two matches have the same length, Game Guru keeps the smaller encoded
distance.

## Adaptive Huffman Tree Representation

The tree uses array indexes rather than heap-allocated nodes.

```text
N_CHAR = 314
T      = 627
R      = 626
```

Arrays:

| Array | Size in source | Meaning |
| --- | ---: | --- |
| `freq` | `T + 1` | Frequencies for internal tree nodes plus one sentinel slot. |
| `child` | `T` | For leaves in the ordered list, stores `symbol + T`; for internal nodes, stores the left child index. |
| `parent` | `T + N_CHAR` | Parent for internal nodes and leaves. Leaf parent is indexed by `symbol + T`. |

Leaf nodes are represented as indexes `symbol + T`, so valid leaf indexes are
`627..940`.

Internal node `i` has children `child[i]` and `child[i] + 1` in the current
ordered node array. During decoding, this is used as:

```text
c = child[R]
while c < T:
    bit = get_bit()
    c = child[c + bit]
symbol = c - T
```

During encoding, the branch bit is recovered from the parity of the node index
while walking from the leaf's parent to the root. Odd child indexes encode bit
`1`; even child indexes encode bit `0`.

## Initial Huffman Tree

Initialization must be exact.

```text
for i in 0..N_CHAR - 1:
    freq[i] = 1
    child[i] = i + T
    parent[i + T] = i

i = 0
for j in N_CHAR..R:
    freq[j] = freq[i] + freq[i + 1]
    child[j] = i
    parent[i] = j
    parent[i + 1] = j
    i += 2

freq[T] = 0xffff
parent[R] = -1
```

The `parent[R] = -1` root-parent sentinel is a Game Guru compatibility detail.
Do not use an unsigned root parent or the common `lzhuf.c` sentinel behavior if
you need byte-exact output.

## Huffman Decode And Encode

Decode one symbol:

```text
decode_char():
    c = child[R]
    while c < T:
        c = child[c + get_bit()]
    symbol = c - T
    update(symbol)
    return symbol
```

Encode one symbol:

```text
encode_char(symbol):
    code = 0
    length = 0
    node = parent[symbol + T]

    do:
        code >>= 1
        if node is odd:
            code += 0x8000
        length += 1
        node = parent[node]
    while node != R

    write the top `length` bits of code, MSB-first
    update(symbol)
```

This encoding process looks unusual because it constructs the code in reverse
while walking from the leaf toward the root. The final right shift selects the
high `length` bits for output.

## Huffman Update

After every literal or match-length symbol, update the adaptive tree.

```text
update(symbol):
    if freq[R] == MAX_FREQ:
        reconstruct()

    c = parent[symbol + T]
    while c >= 0:
        freq[c] += 1
        k = freq[c]
        l = c + 1

        if k > freq[l]:
            while k > freq[l + 1]:
                l += 1

            swap freq[c] with freq[l]
            swap child[c] with child[l]
            repair parent links for both swapped children
            c = l

        c = parent[c]
```

The swap moves a node forward in the frequency-ordered list when its incremented
frequency exceeds following nodes. Parent links for internal sibling pairs must
be updated exactly as the node children move.

## Huffman Reconstruction

When the root frequency reaches `0x8000`, frequencies are reduced and the tree
is rebuilt while preserving the current leaf order.

```text
reconstruct():
    j = 0
    for i in 0..T - 1:
        if child[i] is a leaf index (child[i] >= T):
            freq[j] = (freq[i] + 1) / 2
            child[j] = child[i]
            j += 1

    i = 0
    for j in N_CHAR..T - 1:
        k = i + 1
        f = freq[i] + freq[k]
        freq[j] = f

        k = j - 1
        while f < freq[k]:
            k -= 1
        k += 1

        shift freq[k..j - 1] and child[k..j - 1] right by one
        freq[k] = f
        child[k] = i
        i += 2

    for i in 0..T - 1:
        k = child[i]
        if k is a leaf index:
            parent[k] = i
        else:
            parent[k] = i
            parent[k + 1] = i

    parent[R] = -1
```

Use integer division for `(freq + 1) / 2`.

## Position Coding

The 12-bit match position is split conceptually into a 6-bit high part and a
6-bit low part.

```text
high = (position >> 6) & 0x3f
low  = position & 0x3f
```

The high part is encoded by a fixed 256-entry decode table. The first position
field is always the next 8 bits at the current bit offset; it is not byte-aligned
unless the surrounding Huffman code happened to end on a byte boundary. Some
low-position bits are embedded in that 8-bit prefix value, and the remaining low
bits follow as an extra suffix.

### Decode Tables

Build `position_code[256]` by filling runs in this order:

| High value(s) | Repeated prefix entries per high value |
| --- | ---: |
| `0` | 32 |
| `1..3` | 16 |
| `4..11` | 8 |
| `12..23` | 4 |
| `24..47` | 2 |
| `48..63` | 1 |

Build `position_len[256]` by filling runs in this order:

| Entry bit length | Number of prefix entries |
| ---: | ---: |
| `3` | 32 |
| `4` | 48 |
| `5` | 64 |
| `6` | 48 |
| `7` | 48 |
| `8` | 16 |

Equivalent 8-bit prefix value ranges:

| Prefix value range | High value mapping | `position_len` | Extra suffix bits |
| --- | --- | ---: | ---: |
| `0..31` | `0` | 3 | 1 |
| `32..47` | `1` | 4 | 2 |
| `48..63` | `2` | 4 | 2 |
| `64..79` | `3` | 4 | 2 |
| `80..87` | `4` | 5 | 3 |
| `88..95` | `5` | 5 | 3 |
| `...` | `...` | `...` | `...` |
| `144..147` | `12` | 6 | 4 |
| `...` | `...` | `...` | `...` |
| `192..193` | `24` | 7 | 5 |
| `...` | `...` | `...` | `...` |
| `240` | `48` | 8 | 6 |
| `...` | `...` | `...` | `...` |
| `255` | `63` | 8 | 6 |

The `...` ranges continue the same repeated-entry pattern from the first table.

### Decode Procedure

```text
decode_position():
    value = get_byte_bits()               // 8 bits, MSB-first
    high_part = position_code[value] << 6

    extra_count = position_len[value] - 2
    repeat extra_count times:
        value = ((value << 1) | get_bit()) & 0xffff

    low_part = value & 0x3f
    return high_part | low_part
```

The `position_len` value is not the number of extra bits. It is adjusted by
subtracting 2.

### Encode Procedure

The encoder builds inverse tables by taking the first 8-bit prefix value that
maps to each high value.

First prefix value and length by high-value range:

| High value range | First prefix value formula | `position_len` | Extra suffix bits |
| --- | --- | ---: | ---: |
| `0` | `0` | 3 | 1 |
| `1..3` | `32 + (high - 1) * 16` | 4 | 2 |
| `4..11` | `80 + (high - 4) * 8` | 5 | 3 |
| `12..23` | `144 + (high - 12) * 4` | 6 | 4 |
| `24..47` | `192 + (high - 24) * 2` | 7 | 5 |
| `48..63` | `240 + (high - 48)` | 8 | 6 |

Encoding a position:

```text
encode_position(position):
    high = (position >> 6) & 0x3f
    low = position & 0x3f
    base_prefix = first prefix for high
    bit_len = position_len for high
    extra = bit_len - 2

    prefix = base_prefix + (low >> extra)
    write_bits(prefix, 8)                  // not byte-aligned unless already at a byte boundary
    write_bits(low, extra)                // only low `extra` bits are consumed
```

The source writes `low` as an `extra`-bit field; bits above that width are
ignored by the bit writer. Equivalently, write `low & ((1 << extra) - 1)`.

## Decoder Algorithm

A full decoder is small once the Huffman and position helpers are implemented.

```text
decode_ggc(file_bytes):
    require len(file_bytes) >= 6

    output_size = (file_bytes[0] << 8) | file_bytes[1]
    payload = file_bytes[6..]

    if output_size != 0 and payload is empty:
        reject as truncated

    bit_reader = MSB-first reader over bytes `(payload_byte XOR 0xff)`
    text_buf = array[N + F - 1] filled with 0x20
    init_position_decode_tables()
    start_huff()

    r = N - F
    output = []

    while len(output) < output_size:
        c = decode_char()

        if c < 256:
            text_buf[r] = c
            output.append(c XOR 0xff)
            r = (r + 1) & 0x0fff
        else:
            pos = decode_position()
            source = (r - pos - 1) & 0x0fff
            length = c - 253

            for k in 0..length - 1:
                if len(output) == output_size:
                    break
                b = text_buf[(source + k) & 0x0fff]
                text_buf[r] = b
                output.append(b XOR 0xff)
                r = (r + 1) & 0x0fff

        if bit_reader tried to consume unavailable bits:
            reject as truncated

    return output
```

The loop may stop in the middle of a decoded match if the header byte count is
reached. This matches the repository implementation. Extra bits or bytes after
the requested output size are ignored.

## Encoder Match Finder

To emit a valid GGC stream, any LZSS parser using the same symbol meanings and
position coding can work. To emit byte-identical Game Guru streams, use the
same binary-tree parser.

Tree arrays:

| Array | Size | Meaning |
| --- | ---: | --- |
| `lson` | `N + 1` | Left child for ring nodes. |
| `rson` | `N + 256 + 1` | Right child for ring nodes plus 256 first-byte roots. |
| `dad` | `N + 1` | Parent. `N` is the null/sentinel node. |

Initialization:

```text
for i in N + 1..N + 256:
    rson[i] = N
for i in 0..N - 1:
    dad[i] = N
```

Each first-byte value has a root at `N + 1 + first_byte`.

The encoder also uses a `text_buf` of `N + F - 1` bytes. When a byte is written
to position `s < F - 1`, the same byte is mirrored to `text_buf[s + N]`. This
allows string comparisons to read linearly across the ring-buffer wrap.

## Encoder Initialization And Main Loop

```text
encode_ggc(src, file_type):
    require len(src) <= 65535

    write 6-byte header later
    text_buf = array[N + F - 1] filled with 0x20
    init_position_encode_tables()
    start_huff()
    init_tree()

    input_pos = 0
    s = 0
    r = N - F
    length = 0

    while length < F and input_pos < len(src):
        text_buf[r + length] = src[input_pos] XOR 0xff
        input_pos += 1
        length += 1

    if length == 0:
        flush_bits()
        build header plus empty payload
        return

    for i in 1..F:
        insert_node(r - i)
    insert_node(r)

    do:
        if match_len > length:
            match_len = length

        if match_len <= THRESHOLD:
            match_len = 1
            encode_char(text_buf[r])
        else:
            encode_char(match_len + 253)
            encode_position(match_position)

        last_match_len = match_len
        i = 0

        while i < last_match_len and input_pos < len(src):
            c = src[input_pos] XOR 0xff
            input_pos += 1
            delete_node(s)
            text_buf[s] = c
            if s < F - 1:
                text_buf[s + N] = c
            s = (s + 1) & 0x0fff
            r = (r + 1) & 0x0fff
            insert_node(r)
            i += 1

        while i < last_match_len:
            delete_node(s)
            s = (s + 1) & 0x0fff
            r = (r + 1) & 0x0fff
            length -= 1
            if length != 0:
                insert_node(r)
            i += 1
    while length > 0

    flush_bits()
    prepend header
```

The encoder does not write an explicit end marker. It relies on the header size
for decoding termination.

## Insert Node Details

`insert_node(r)` searches for the longest match of the string beginning at ring
position `r`, then inserts `r` into the binary tree. It updates two encoder
state fields: `match_len` and `match_position`.

```text
insert_node(r):
    cmp = 1
    key = r
    p = N + 1 + text_buf[key]
    rson[r] = N
    lson[r] = N
    match_len = 0

    while true:
        if cmp >= 0:
            if rson[p] != N:
                p = rson[p]
            else:
                rson[p] = r
                dad[r] = p
                return
        else:
            if lson[p] != N:
                p = lson[p]
            else:
                lson[p] = r
                dad[r] = p
                return

        for i in 1..F - 1:
            cmp = text_buf[key + i] - text_buf[p + i]
            if cmp != 0:
                break

        if i > THRESHOLD:
            distance = ((r - p) & 0x0fff) - 1
            if i > match_len or (i == match_len and distance < match_position):
                match_position = distance
                match_len = i
                if match_len >= F:
                    break

    replace node p with node r in the tree
```

Important compatibility points:

| Detail | Required behavior |
| --- | --- |
| Comparison starts at `i = 1` | The root is selected by first byte, so comparisons skip byte 0. |
| Match threshold for tracking | Only `i > 2` updates `match_len`. |
| Equal-length tie-break | Prefer smaller encoded `distance`. |
| Full-length match | If `match_len >= 60`, stop search and replace the old node. |
| Replacement | New node `r` takes the old node's parent and children; old node parent becomes `N`. |

## Delete Node Details

`delete_node(p)` removes ring node `p` from the binary tree if it is currently
linked. Sentinel `N` means null. If both children exist, it uses the rightmost
node of the left subtree as the replacement, matching the source implementation.

The exact delete algorithm matters for byte-identical compression because it
changes future tree walk order and therefore match tie outcomes.

## Header Construction

After the payload bytes have been generated, the compressor allocates the final
file and prepends the 6-byte header.

```text
result[0] = (src_size >> 8) & 0xff
result[1] = src_size & 0xff
for i in 0..3:
    result[2 + i] = file_type[i] XOR 0xff
copy payload bytes to result[6..]
```

`src_size` must be `<= 0xffff`. The payload may be empty only when `src_size` is
zero.

## Validity And Error Handling

Recommended decoder behavior:

| Condition | Recommended result |
| --- | --- |
| File shorter than 6 bytes | Reject as bad size. |
| Header size is nonzero and payload is empty | Reject as truncated. |
| Bit reader needs unavailable bits before producing header byte count | Reject as truncated. |
| Header size is zero | Return an empty output; payload, if present, is not needed. |
| Payload has extra bytes after requested output is produced | Ignore for Game Guru compatibility, or warn in strict tools. |
| Header file type decodes to nonprintable bytes | Still valid for decompression; the field is metadata. |

Recommended encoder behavior:

| Condition | Recommended result |
| --- | --- |
| Input size greater than 65535 bytes | Reject. The container cannot store the size. |
| File type omitted | Use four spaces unless your application has better metadata. |
| File type supplied | Require exactly 4 bytes if using a CLI or text API. |

The `ggc_decompress` implementation reports `GGC_ERR_BADSIZE` for files shorter
than the header, `GGC_ERR_TRUNCATED` when compressed bits run out, and ignores
the file-type field. It allocates one byte internally for zero-length outputs so
that allocation behavior remains well-defined, but returns output size zero.

## Implementation Checklist

Use unsigned byte values everywhere. The internal inverted literal domain uses
all values `0..255`.

Initialize the LZSS ring to `0x20`, not zero.

Initialize `r` to `N - F` for both compression and decompression.

Update the adaptive Huffman tree after every literal or match-length symbol,
but not after position bits. Positions are encoded by the static position code,
not by the adaptive tree.

Write and read payload bits MSB-first after applying payload-byte XOR.

Do not add an EOS symbol. The only termination condition is the 16-bit header
size.

Preserve `parent[R] = -1` after initial Huffman setup and reconstruction.

For byte-exact compression, preserve the binary-tree insert/delete order and
the equal-length smaller-distance tie-break.

## Source Cross-reference

| Behavior | Source |
| --- | --- |
| Container constants | Top of `src/ggc.c` |
| Header read/write | `ggc_decompress`, `ggc_compress` in `src/ggc.c` |
| Payload bit reader | `ggc_decoder_get_byte`, `ggc_decoder_get_bit`, `ggc_decoder_get_byte_bits` in `src/ggc.c` |
| Payload bit writer | `ggc_put_bit`, `ggc_put_bits`, `ggc_flush_bits` in `src/ggc.c` |
| Huffman setup/update | `ggc_start_huff`, `ggc_update`, `ggc_reconst` in `src/ggc.c` |
| Huffman symbol coding | `ggc_decode_char`, `ggc_encode_char` in `src/ggc.c` |
| Position tables | `ggc_init_decode_position_tables`, `ggc_init_encode_position_tables` in `src/ggc.c` |
| Position coding | `ggc_decode_position`, `ggc_encode_position` in `src/ggc.c` |
| Match finder | `ggc_insert_node`, `ggc_delete_node` in `src/ggc.c` |
| CLI file-type policy | `src/subcmd_ggc_compress.cpp` |
