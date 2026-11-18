// Movies (IAMMultiMediaStream over MPEG-1 .STR files). Not implemented yet: creation fails the
// way it does on a PC without DirectShow, and the game skips the movie.
#include "com.h"

uint32_t movie_create(uint32_t iid, uint32_t out) {
    (void)iid;
    WR32(out, 0);
    return 0x80040154u;
}
