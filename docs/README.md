# Compression Format Documentation

This directory contains implementation-oriented documentation for the two
compression formats supported by `3ct`.

| Document | Scope |
| --- | --- |
| [`3do-compression-format.md`](3do-compression-format.md) | Raw 3DO SDK `comp3do` / `decomp3do` LZSS bitstream. |
| [`game-guru-compression-format.md`](game-guru-compression-format.md) | Game Guru `.COMP` container and its byte-inverted adaptive-Huffman/LZSS payload. |

The documents are written as engineering specs for new compatible
implementations. They describe the on-disk byte layout, bit ordering, stream
state, encoder and decoder algorithms, edge cases, and byte-exact compatibility
requirements reflected by this repository's source.
