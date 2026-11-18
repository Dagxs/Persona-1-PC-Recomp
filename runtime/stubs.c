// Import tables for DLLs that are not implemented yet: calling into them stops with the name.
#include "win32.h"

const struct impl USER32_IMPL[] = { { 0 } };
const struct impl GDI32_IMPL[] = { { 0 } };
const struct impl ADVAPI32_IMPL[] = { { 0 } };
const struct impl WINMM_IMPL[] = { { 0 } };
const struct impl OLE32_IMPL[] = { { 0 } };
const struct impl DDRAW_IMPL[] = { { 0 } };
const struct impl QUARTZ_IMPL[] = { { 0 } };
