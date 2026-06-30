#pragma once

#include "app/state.h"

namespace app {

// Display rendering
void drawStatusScreen(const String& title, const String& message);
void drawDashboard();

// Web setup and provisioning
void startWebServer();
bool provisionEdfAccount(const String& email, const String& password, const String& account, bool drawProgress);

}  // namespace app
