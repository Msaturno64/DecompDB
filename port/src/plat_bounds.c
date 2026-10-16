/*
 * Diagnostics: a build whose game code is compiled with BT3_GAME_CFLAGS=-fsanitize=array-bounds (port/tools/
 * undefined.py; research/build-install.ps1 -Bounds of the multi-fighter work) calls this for every index past a
 * fixed-size array, and goes on. Each place is reported once, on stderr, as "bt3: bounds: <file>:<line> index <n>".
 * Some places are the game's own: the decompilation declares an array of one where the original runs on into
 * the fields behind it (bobj_b.h unk98). A list of a 1v1 run tells those from the new ones.
 * A build without the flag never calls it. Not game state (PORT_HOST).
 */
#include <stdint.h>
#include <stdio.h>
#include "port_host.h"

struct UbsanSourceLocation { const char *file; uint32_t line, column; }; /* clang's, first in the data record */

PORT_HOST static const void *sSeen[1024];
PORT_HOST static int sSeenCount;

void __ubsan_handle_out_of_bounds(void *data, void *index) {
    const struct UbsanSourceLocation *loc = data;
    int i;

    for (i = 0; i < sSeenCount; i++) {
        if (sSeen[i] == data) {
            return;
        }
    }
    if (sSeenCount < (int)(sizeof(sSeen) / sizeof(sSeen[0]))) {
        sSeen[sSeenCount++] = data;
    }
    fprintf(stderr, "bt3: bounds: %s:%u index %lld\n", loc->file, loc->line, (long long)(intptr_t)index);
    fflush(stderr);
}
