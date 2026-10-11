#pragma once

#include "config.h"

// Wi-Fi settings and the configuration portal served to the Palm.
// The portal answers on http://PORTAL_DOMAIN/ (and http://PPP_GATEWAY_IP/) over
// the PPP link only; it is not reachable from the Wi-Fi side.
void irportal_init();
