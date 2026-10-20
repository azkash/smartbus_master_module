#include "wifi_connect_guard.h"

static_assert(shouldStartStaConnect(false, "", false, "", "Thankyou"),
              "first connection request must start");
static_assert(!shouldStartStaConnect(true, "Thankyou", false, "", "Thankyou"),
              "duplicate request for the connected SSID must be ignored");
static_assert(!shouldStartStaConnect(false, "", true, "Thankyou", "Thankyou"),
              "duplicate request while the same SSID is connecting must be ignored");
static_assert(shouldStartStaConnect(true, "OldNetwork", false, "", "Thankyou"),
              "a request for a different SSID must start");
static_assert(shouldStartStaConnect(false, "", true, "OldNetwork", "Thankyou"),
              "a different request may replace an in-flight connection");

