#pragma once

constexpr bool wifiSsidEquals(const char* left, const char* right) {
  if (!left || !right) return false;
  while (*left || *right) {
    if (*left != *right) return false;
    ++left;
    ++right;
  }
  return true;
}

constexpr bool shouldStartStaConnect(bool staConnected,
                                            const char* currentSsid,
                                            bool connectInFlight,
                                            const char* pendingSsid,
                                            const char* requestedSsid) {
  if (!requestedSsid || requestedSsid[0] == '\0') return false;
  if (staConnected && wifiSsidEquals(currentSsid, requestedSsid)) return false;
  if (connectInFlight && wifiSsidEquals(pendingSsid, requestedSsid)) return false;
  return true;
}

