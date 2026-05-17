# 3DO SDK Compression Format

This document describes the raw compression stream used by the 3DO SDK
compression library and by the `3ct compress` / `3ct decompress` commands. The
format is an LZSS-style byte stream packed into big-endian 32-bit words. There
is no file header, no stored decompressed byte count, and no checksum.

The details here are derived from `src/compress.c`, `src/decompress.cpp`, and
`src/lzss.h`. The goal is to make the format implementable without reading the
source.

## Terminology

| Term | Meaning |
| --- | --- |
| Compressed word | A 32-bit container word in the compressed stream. Words are serialized big-endian. |
| Input word | A 32-bit word of uncompressed data supplied to the compressor. The SDK API is word-oriented. |
| Literal | A token carrying one uncompressed byte directly. |
| Phrase | A token carrying a window index and length field, causing bytes to be copied from the sliding window. |
| Window index | A 12-bit absolute index into the 4096-byte ring buffer, not a backward distance. |
| EOS | End-of-stream marker: phrase flag plus index `0`. |

## Format Summary

| Property | Value |
| --- | --- |
| Algorithm family | LZ77/LZSS with binary-tree match finder. |
| Wrapper/header | None. The file is only the packed token stream. |
| Compressed unit | 32-bit words. Files produced by the SDK-style compressor are multiples of 4 bytes. |
| Compressed word byte order | Big-endian. |
| Bit order inside each word | Most-significant bit first. |
| Sliding window size | 4096 bytes. |
| Window index bits | 12. |
| Match length field bits | 4. |
| Minimum encoded match length | 3 bytes. |
| Maximum encoded match length | 18 bytes. |
| Literal token size | 9 bits. |
| Phrase token size | 17 bits. |
| EOS token size | 13 bits. |
| Output alignment | The SDK decompressor emits only complete 32-bit output words. A token stream may leave 1, 2, or 3 decoded bytes pending at EOS; those pending bytes are not emitted. |

## Constants

These constants define the bitstream and must match for interoperable streams.

```c
INDEX_BIT_COUNT  = 12
LENGTH_BIT_COUNT = 4
WINDOW_SIZE      = 1 << INDEX_BIT_COUNT   // 4096
BREAK_EVEN       = 2
END_OF_STREAM    = 0
LOOK_AHEAD_SIZE  = (1 << LENGTH_BIT_COUNT) + BREAK_EVEN  // 18
TREE_ROOT        = WINDOW_SIZE            // 4096, compressor-only sentinel
UNUSED           = 0                      // compressor tree null value
MOD_WINDOW(x)    = x & (WINDOW_SIZE - 1)
```

`BREAK_EVEN` is named from the classic LZSS decision: a match must be longer
than 2 bytes to beat literal encoding. The stored length field is not the byte
count. The actual copied byte count is `stored_length + BREAK_EVEN + 1`, which
is `stored_length + 3`.

## File Layout

The compressed file is a sequence of 32-bit words.

```text
word[0] word[1] word[2] ... word[n - 1]
```

Each word is stored big-endian. The first bit consumed is bit 31 of `word[0]`,
then bit 30, continuing down to bit 0, then bit 31 of `word[1]`.

There is no magic number. A decoder must discover stream termination by reading
the EOS token.

## Bit Reader

A compatible decoder treats the compressed file as big-endian 32-bit words and
reads bits MSB-first.

```text
load_word():
    if no complete 32-bit compressed word remains:
        signal missing compressed data
    return next 4 bytes interpreted as a big-endian uint32

read_bits(n):
    result = 0
    while n > 0:
        if bit_buffer is empty:
            bit_buffer = load_word()
            bits_left = 32
        take = min(n, bits_left)
        result = (result << take) | top take bits from bit_buffer
        remove those bits from bit_buffer
        n -= take
    return result
```

The implementation in this repository loads an entire word when a request spans
the current word boundary. It is equivalent to the loop above for all token
field sizes used by the format.

## Bit Writer

A compatible encoder writes fields MSB-first into a 32-bit accumulator. When the
accumulator fills, it serializes the accumulator as a big-endian word. On stream
finish, if the accumulator is partially full, the encoder writes one final
big-endian word padded with zero bits in the low-order unused positions.

```text
bits_left = 32
bit_buffer = 0

write_bits(value, width):
    for i from width - 1 down to 0:
        bits_left -= 1
        bit_buffer |= ((value >> i) & 1) << bits_left
        if bits_left == 0:
            write_big_endian_u32(bit_buffer)
            bits_left = 32
            bit_buffer = 0

finish_bits():
    if bits_left != 32:
        write_big_endian_u32(bit_buffer)
```

The source implementation exposes a helper that writes a one-bit token flag and
then a token field. This is only an API convenience. On the wire all bits are a
single continuous stream.

## Token Grammar

Every token begins with a one-bit flag.

```text
token := literal | phrase | eos

literal := 1:1 byte:8
phrase  := 0:1 index:12 length_field:4    where index != 0
eos     := 0:1 index:12                   where index == 0
```

The flag meaning is fixed.

| Flag bit | Meaning |
| ---: | --- |
| `1` | Literal token. Read 8 more bits and emit that byte. |
| `0` | Phrase or EOS. Read a 12-bit index. If the index is `0`, stop. Otherwise read a 4-bit length field and copy bytes from the window. |

Phrase fields are interpreted as follows.

```text
index        = read_bits(12)              // 1..4095 for phrases
length_field = read_bits(4)               // 0..15
copy_count   = length_field + 3           // 3..18 bytes
```

The repository decompressor computes `matchLen = length_field + BREAK_EVEN` and
then copies while `i <= index + matchLen`. This inclusive loop produces
`length_field + 3` bytes.

## Sliding Window Semantics

The decoder maintains a 4096-byte ring buffer.

Initial decoder state:

```text
window[0..4095] = 0x00
write_pos       = 1
```

Literal decoding:

```text
c = read_bits(8)
emit_byte(c)
window[write_pos] = c
write_pos = (write_pos + 1) & 0x0fff
```

Phrase decoding:

```text
index = read_bits(12)
if index == 0:
    stop

length_field = read_bits(4)
copy_count = length_field + 3

for offset from 0 to copy_count - 1:
    c = window[(index + offset) & 0x0fff]
    emit_byte(c)
    window[write_pos] = c
    write_pos = (write_pos + 1) & 0x0fff
```

The phrase index is an absolute ring-buffer index. It is not encoded relative to
`write_pos`. This differs from many LZSS file formats that store a backward
distance.

Phrase copies are overlap-aware because bytes are read and written one at a
time. If the copy source range reaches bytes just written by the same phrase,
those bytes are used, producing the usual LZ77 repeated-run behavior.

Index `0` is reserved for EOS and is not a valid phrase source in SDK-compatible
streams. The compressor avoids node `0` for match insertion.

## Decompressed Output Word Handling

The original SDK API is word-oriented. The callback receives 32-bit words, not
individual bytes. `3ct` preserves this behavior.

Decoded bytes are accumulated in big-endian byte order into a 32-bit word.

```text
pending_word = 0
bytes_needed = 4

emit_byte(c):
    if bytes_needed == 0:
        output_big_endian_word(pending_word)
        pending_word = c
        bytes_needed = 3
    else:
        pending_word = (pending_word << 8) | c
        bytes_needed -= 1

finish_output():
    if EOS was seen and bytes_needed == 0:
        output_big_endian_word(pending_word)
```

The implementation writes output words in host memory order after swapping on
little-endian hosts, so the bytes in the output file match the original byte
order.

SDK-generated streams are word-oriented, not byte-count-oriented. The token
stream can leave 1, 2, or 3 decoded bytes pending at EOS. Those pending bytes are
discarded by the SDK callback model because only complete 32-bit words are
observable. For example, the SDK fixture in `src/subcmd_check.cpp` represents a
1024-byte file, but a raw byte-token walk sees two additional zero bytes after
the final complete word; the SDK decompressor does not emit them.

For a byte-oriented API, the raw stream alone is insufficient to distinguish
intentional trailing bytes from SDK padding unless an outer container supplies an
expected decompressed byte count. A compatibility decoder should expose the SDK
word output or require an external expected size.

## Complete Decoder Pseudocode

This pseudocode describes an SDK-compatible word-oriented decoder. The helper
`emit_decoded_byte` is the word accumulator described above.

```text
decode_3do(compressed_bytes):
    require len(compressed_bytes) % 4 == 0

    bits = MSB-first reader over big-endian u32 words
    window = array[4096] filled with 0x00
    write_pos = 1
    output_words = []
    pending_word = 0
    bytes_needed = 4

    emit_decoded_byte(c):
        if bytes_needed == 0:
            output_words.append(pending_word)
            pending_word = c
            bytes_needed = 3
        else:
            pending_word = (pending_word << 8) | c
            bytes_needed -= 1

    while true:
        flag = bits.read(1)

        if flag == 1:
            c = bits.read(8)
            emit_decoded_byte(c)
            window[write_pos] = c
            write_pos = (write_pos + 1) & 0x0fff
            continue

        index = bits.read(12)
        if index == 0:
            break

        length_field = bits.read(4)
        count = length_field + 3

        for offset in 0..count - 1:
            c = window[(index + offset) & 0x0fff]
            emit_decoded_byte(c)
            window[write_pos] = c
            write_pos = (write_pos + 1) & 0x0fff

    if bytes_needed == 0:
        output_words.append(pending_word)

    return output_words serialized high byte first
```

Strict decoders should report truncated input if a token field requires bits
past the last complete compressed word. They may ignore zero padding bits after
EOS in the final loaded compressed word. The `3ct` decompressor reports data
remaining if EOS is reached before all supplied compressed words have been
consumed.

## Encoder Token Selection

Any stream following the token grammar and window rules is decompressible. To be
byte-identical with the SDK and with `3ct`, use the same parser and match finder
described in this section.

The encoder maintains the same 4096-byte window and starts with this state.

```text
window[0..4095] = 0x00
current_pos     = 1
look_ahead      = 1
match_pos       = 0
match_len       = 0
```

It also maintains a binary search tree of strings in the window.

```text
tree[0..4096] has parent, left_child, right_child
all fields are initialized to UNUSED (0)
tree[TREE_ROOT].right_child = 1
tree[1].parent = TREE_ROOT
```

The tree root index is `4096`, outside the ring buffer. Node `0` is both the
tree null value and the stream EOS value, so it is not inserted as a matchable
string.

### Look-ahead Loading

The streaming API receives full 32-bit input words. The compressor processes the
bytes of those words in file order. The CLI zero-initializes the final word
before reading, so an input file whose byte length is not a multiple of 4 is
compressed as if it had zero padding through the next word boundary.

At the start of the first feed, bytes are loaded into `window[1]` through
`window[18]` until either 18 bytes are available or the current feed chunk ends.
The first emitted token is for `current_pos == 1`.

### Match Finder

The tree stores one node for each candidate string start position. Strings are
compared lexicographically for up to `LOOK_AHEAD_SIZE` bytes, wrapping through
the ring buffer.

Insertion and search are one operation:

```text
add_string(new_node):
    if new_node == END_OF_STREAM:
        return 0

    test_node = tree[TREE_ROOT].right_child
    best_len = 0
    best_pos = 0

    while true:
        i = 0
        while i < LOOK_AHEAD_SIZE:
            delta = window[(new_node + i) & 0x0fff] -
                    window[(test_node + i) & 0x0fff]
            if delta != 0:
                break
            i += 1

        if i >= best_len:
            best_len = i
            best_pos = test_node
            if best_len >= LOOK_AHEAD_SIZE:
                replace test_node in the tree with new_node
                return best_len

        if delta >= 0:
            follow or create right child
        else:
            follow or create left child
```

The `i >= best_len` comparison is significant. Equal-length matches update the
best position to the node encountered later in the tree walk. A maximum-length
18-byte duplicate replaces the older tree node with the new node.

Deletion is classic binary-tree deletion. Before the window overwrites a string
start, the node at that start is deleted from the tree.

### Token Emission Rule

Before emitting, clamp `match_len` to the number of bytes actually available in
the look-ahead buffer.

```text
if match_len > look_ahead:
    match_len = look_ahead

if match_len <= BREAK_EVEN:
    write flag 1
    write window[current_pos] as 8 bits
    replace_count = 1
else:
    write flag 0
    write match_pos as 12 bits
    write (match_len - 3) as 4 bits
    replace_count = match_len
```

After emitting a token, repeat `replace_count` times:

```text
delete_string((current_pos + LOOK_AHEAD_SIZE) & 0x0fff)

if more input bytes are available:
    window[(current_pos + LOOK_AHEAD_SIZE) & 0x0fff] = next input byte
else:
    save state, set SecondPass, and return from FeedCompressor

current_pos = (current_pos + 1) & 0x0fff

if look_ahead != 0:
    match_len = add_string(current_pos)
```

Final flushing is performed only by `DeleteCompressor`. If the last
`FeedCompressor` call ran out of input in the replacement loop, finalization
resumes at the `resume_replacement` label instead of redoing the delete step.
No new byte is loaded during finalization.

The SDK-compatible final flush loop continues while `look_ahead >= 0`. This is
intentional: after the last real look-ahead byte has been consumed, the encoder
emits one more normal token with `look_ahead == 0`. `WriteNextToken` clamps the
match length to zero, so that final normal token is encoded as a literal from
the current window position. Only after that zero-lookahead token does the
encoder write EOS. The SDK decompressor's word-output behavior may leave the
decoded byte from this final token pending and unobservable.

### End-of-stream Emission

After final flushing, including the zero-lookahead normal token, write:

```text
flag  = 0
index = 0  // 12 bits
```

No length field follows EOS. Then flush the 32-bit bit accumulator, padding the
last word with zeros if necessary.

## Byte-oriented Encoder Variant

This pseudocode describes a normalized single-shot encoder that emits all input
bytes and then writes EOS. It produces a valid stream for decoders that implement
the token grammar, but it is not byte-identical to the SDK compressor because it
does not emit the SDK final zero-lookahead token and does not model the streaming
`SecondPass` resume point. Use it only when byte-for-byte SDK compatibility is
not required. For SDK-compatible output, follow the source-state finalization
behavior after the pseudocode.

```text
encode_3do(input_bytes):
    input_bytes must represent whole 32-bit words

    window = array[4096] filled with 0x00
    tree = initialized as described above
    current_pos = 1
    look_ahead = 1
    match_pos = 0
    match_len = 0

    src = iterator(input_bytes)
    while look_ahead <= 18 and src has byte:
        window[look_ahead] = src.next()
        look_ahead += 1
    look_ahead -= 1

    while look_ahead > 0:
        if match_len > look_ahead:
            match_len = look_ahead

        if match_len <= 2:
            write_bits(1, 1)
            write_bits(window[current_pos], 8)
            replace_count = 1
        else:
            write_bits(0, 1)
            write_bits(match_pos, 12)
            write_bits(match_len - 3, 4)
            replace_count = match_len

        while replace_count > 0:
            replace_count -= 1
            delete_string((current_pos + 18) & 0x0fff)

            if src has byte:
                window[(current_pos + 18) & 0x0fff] = src.next()
            else:
                look_ahead -= 1

            current_pos = (current_pos + 1) & 0x0fff
            if look_ahead > 0:
                match_len, match_pos = add_string(current_pos)

    write_bits(0, 1)
    write_bits(0, 12)
    finish_bits()
```

The actual source uses a `SecondPass` flag to resume inside the replacement loop
when streaming chunks end. Its final flush is equivalent to:

```text
flush_compressor_state():
    if SecondPass:
        goto resume_replacement

    while look_ahead >= 0:
        emit token using the normal token rule

        while replace_count--:
            delete_string((current_pos + 18) & 0x0fff)
            look_ahead -= 1

        resume_replacement:
            // No byte is loaded during finalization.
            current_pos = (current_pos + 1) & 0x0fff
            if look_ahead != 0:
                match_len, match_pos = add_string(current_pos)

    write EOS and flush the bit accumulator
```

That control flow is part of byte-exact SDK compatibility. The `while
look_ahead >= 0` condition is what emits the final zero-lookahead literal before
EOS. A simpler encoder can produce streams that the SDK decompressor accepts,
but it will not necessarily match SDK output byte-for-byte.

## Validity And Error Handling

Recommended validation rules:

| Condition | Recommended result |
| --- | --- |
| Compressed byte length is not a multiple of 4 | Reject as truncated or malformed. |
| A token field crosses past the last complete compressed word | Reject as truncated. |
| EOS is never found | Reject as truncated. |
| EOS is found before unconsumed complete compressed words remain | Reject as trailing data, unless your container allows concatenation. |
| Phrase index is `0` | Treat as EOS; do not read a length. |
| Phrase index is `1..4095` | Valid. Copy from that absolute window position. |
| EOS occurs with a partial decompressed output word | Discard the pending bytes for SDK compatibility; require an external expected byte count for byte-oriented decoding. |

The `3ct` decompressor checks for missing compressed data, missing EOS, partial
compressed words at the CLI level, and extra compressed words after EOS. Padding
bits after EOS inside the final consumed word are ignored.

## Implementation Notes

Use unsigned arithmetic for bit buffers and ring indices. Signed right shifts or
sign-extending byte conversions will break compatibility.

Do not reinterpret phrase indexes as distances. The 12-bit phrase field is an
absolute index into the 4096-byte decoder window.

Do not add a header or decompressed-size field when producing SDK-compatible
streams. Size information, if needed, belongs to an outer container.

Preserve big-endian compressed word order on disk. On little-endian machines,
this means byte-swapping the 32-bit bit accumulator before writing with native
word I/O, or more simply writing explicit big-endian bytes.

For a byte-oriented public API, define how non-word-aligned inputs are handled.
The `3ct` CLI follows the SDK word API and pads the final partial input word
with zero bytes before compression.

## Source Cross-reference

| Behavior | Source |
| --- | --- |
| Constants | `src/lzss.h`, `src/compress.c` |
| Bit writer | `WriteBits`, `CleanupBitStream` in `src/compress.c` |
| Match finder | `AddString`, `DeleteString` in `src/compress.c` |
| Encoder state machine | `FeedCompressor`, `FlushCompressor`, `DeleteCompressor` in `src/compress.c` |
| Bit reader | `ReadBits` in `src/decompress.cpp` |
| Decoder state machine | `internalFeedDecompressor`, `DeleteDecompressor` in `src/decompress.cpp` |
| CLI word I/O behavior | `src/subcmd_compress.cpp`, `src/subcmd_decompress.cpp` |
