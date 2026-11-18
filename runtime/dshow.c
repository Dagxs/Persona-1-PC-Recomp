// DirectShow (movies). Not implemented yet: the filter graph reports "class not registered",
// so movie playback fails the way it would on a PC without DirectShow.
#include "com.h"

uint32_t dshow_create(uint32_t iid, uint32_t out) {
    (void)iid;
    fprintf(stderr, "DirectShow filter graph requested: movies are not implemented yet\n");
    WR32(out, 0);
    return 0x80040154u;
}
