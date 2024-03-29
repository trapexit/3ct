#pragma once

#include <cstdint>

/*
 * Various constants used to define the compression parameters.
 * INDEX_BIT_COUNT tells how many bits we allocate to indices into the
 * text window. This directly determines the WINDOW_SIZE.  LENGTH_BIT_COUNT
 * tells how many bits we allocate for the length of an encoded phrase. This
 * determines the size of the look ahead buffer. END_OF_STREAM is a special
 * index used to flag the fact that the file has been completely encoded, and
 * there is no more data. MOD_WINDOW() is a macro used to perform arithmetic
 * on tree indices.
 */

#define INDEX_BIT_COUNT  12
#define LENGTH_BIT_COUNT 4
#define WINDOW_SIZE      (1 << INDEX_BIT_COUNT)
#define BREAK_EVEN       2
#define END_OF_STREAM    0
#define MOD_WINDOW(a)    ((a) & (WINDOW_SIZE - 1))


/*
 * Definitions shared between compress.c and addstring.c. If addstring.c
 * gets merged into compress.c, move these definitions into compress.c to
 * reduce their visibility. addstring.c is a separate source file only
 * because of a bug in the ARM 1.61 compiler.
 */

#define LOOK_AHEAD_SIZE ((1 << LENGTH_BIT_COUNT) + BREAK_EVEN)
#define TREE_ROOT       WINDOW_SIZE
#define UNUSED          0
