/* Empty host-test assets in a separate TU: presentation tests never render
 * text/icons, and production's opaque asset symbols stay opaque to the compiler. */
#include "types.h"
const uint8_t asset_fonts[8]={0},asset_icons[8]={0},asset_globe[16]={0};
