#pragma once

#include "app/state.h"

namespace app {

bool obtainRefreshToken(const String& email, const String& password, String* refreshToken);
bool discoverAccount(const String& accountNumber);
bool refreshData(bool* displayChanged = nullptr);
bool currentPageHasData();

}  // namespace app
