#pragma once

#include "config.h"

// Wi-Fi settings and the configuration portal served to the Palm.
// The portal answers on http://PORTAL_DOMAIN/ (and http://PPP_GATEWAY_IP/) over
// the PPP link only; it is not reachable from the Wi-Fi side, except for the
// Google sign-in pages for a short while after the Palm asked for them.
void irportal_init();
