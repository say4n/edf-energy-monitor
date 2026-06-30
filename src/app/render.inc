float bucketTotal(const Bucket& bucket, bool cost) {
  return cost ? bucket.electricityCost + bucket.gasCost : bucket.electricityKwh + bucket.gasKwh;
}

void drawStackedChart(int x, int y, int w, int h, const String& title, Bucket* buckets, int count, bool cost) {
  LOGI("drawStackedChart title=%s x=%d y=%d w=%d h=%d buckets=%d metric=%s",
       title.c_str(), x, y, w, h, count, cost ? "cost" : "energy");
  canvas.setTextColor(BLACK);
  canvas.setFont(&fonts::FreeSansBold9pt7b);
  canvas.setTextDatum(top_left);

  int chartY = y + 22;
  int chartH = h - 44;
  canvas.fillRect(x, chartY, w, chartH, COLOR_PANEL);
  canvas.drawRect(x, chartY, w, chartH, BLACK);
  for (int i = 1; i < 4; ++i) {
    int gy = chartY + (chartH * i) / 4;
    canvas.drawFastHLine(x + 1, gy, w - 2, COLOR_GRID);
  }

  float maxTotal = 0;
  float periodTotal = 0;
  for (int i = 0; i < count; ++i) {
    float total = bucketTotal(buckets[i], cost);
    maxTotal = max(maxTotal, total);
    periodTotal += total;
  }
  String titleText = title + " (" + (cost ? (String(periodTotal, 2) + " GBP") : (String(periodTotal, 1) + " kWh")) + ")";
  drawClippedString(titleText, x, y, w);
  if (maxTotal <= 0.001f) maxTotal = 1;

  int gap = count > 12 ? 2 : 5;
  int barW = max(2, (w - 10 - (count - 1) * gap) / count);
  int startX = x + 5;
  int baseline = chartY + chartH - 1;

  for (int i = 0; i < count; ++i) {
    float elec = cost ? buckets[i].electricityCost : buckets[i].electricityKwh;
    float gas = cost ? buckets[i].gasCost : buckets[i].gasKwh;
    int elecH = static_cast<int>((elec / maxTotal) * (chartH - 4));
    int gasH = static_cast<int>((gas / maxTotal) * (chartH - 4));
    int bx = startX + i * (barW + gap);
    if (gasH > 0) canvas.fillRect(bx, baseline - gasH, barW, gasH, COLOR_GAS);
    if (elecH > 0) canvas.fillRect(bx, baseline - gasH - elecH, barW, elecH, COLOR_ELECTRICITY);

    float total = elec + gas;
    if (total > 0.01f && barW >= 20) {
      canvas.setFont(&fonts::Font0);
      canvas.setTextColor(BLACK);
      canvas.setTextDatum(bottom_center);
      String label = cost ? String(total, 2) : String(total, 1);
      canvas.drawString(label, bx + barW / 2, baseline - gasH - elecH - 2);
    }
  }

  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(COLOR_CHART_LABEL);
  canvas.setTextDatum(top_left);
  String maxLabel = cost ? ("GBP " + String(maxTotal, 2)) : (String(maxTotal, 1) + " kWh");
  drawClippedString(maxLabel, x, chartY + chartH + 3, w / 2);

  canvas.setTextDatum(top_right);
  if (count == 7) {
    canvas.drawString("Last 7 days", x + w, chartY + chartH + 3);
  } else if (count == YEAR_MONTHS) {
    canvas.drawString("Last 12 months", x + w, chartY + chartH + 3);
  } else {
    canvas.drawString("Last " + String(count) + " days", x + w, chartY + chartH + 3);
  }
}

void drawStatusScreen(const String& title, const String& message) {
  LOGI("drawStatusScreen title=%s message=%s wifi=%d error=%s",
       title.c_str(), message.c_str(), WiFi.isConnected(), rtcLastError);
  M5.Display.setEpdMode(epd_mode_t::epd_fast);
  int W = M5.Display.width();
  int H = M5.Display.height();
  canvas.fillSprite(WHITE);
  canvas.fillRect(0, 0, W, 52, COLOR_HEADER);
  canvas.setTextColor(WHITE);
  canvas.setFont(&fonts::FreeSansBold12pt7b);
  canvas.setTextDatum(top_left);
  drawClippedString(title, 18, 14, W - 36);
  canvas.setTextColor(BLACK);
  canvas.setFont(&fonts::FreeSans9pt7b);
  drawClippedString(message, 18, 70, W - 36);
  if (WiFi.isConnected()) {
    String setupUrl = "http://" + WiFi.localIP().toString() + "/";
    canvas.setTextColor(COLOR_HEADER);
    drawClippedString(setupUrl, 18, 102, W - 36);
    if (title == "Setup required") {
      canvas.setFont(&fonts::Font2);
      canvas.setTextColor(COLOR_MUTED);
      canvas.drawString("Scan to configure", 18, 132);
      drawQrCode(setupUrl, 64, 158, min(270, W - 128));
      canvas.setTextColor(BLACK);
    }
  } else if (rtcWifiDetail[0] != '\0') {
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(BLACK);
    drawClippedString(rtcWifiDetail, 18, 102, W - 36);
  }
  if (rtcLastError[0] != '\0') {
    int errorY = H - 82;
    canvas.fillRect(0, errorY - 8, W, 50, WHITE);
    canvas.drawFastHLine(18, errorY - 10, W - 36, COLOR_GRID);
    canvas.setTextColor(RED);
    canvas.setFont(&fonts::FreeSans9pt7b);
    drawClippedString(String("Error: ") + rtcLastError, 18, errorY, W - 36);
  }
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(COLOR_MUTED);
  canvas.drawString(String("LED enabled: ") + (M5.Led.isEnabled() ? "yes" : "no"), 18, H - 26);
  canvas.pushSprite(0, 0);
}

void drawDashboard() {
  LOGI("drawDashboard page=%s has_data=%d last_refresh=%ld wifi=%d",
       pageNameC(static_cast<Page>(rtcCurrentPage)), rtcHasData,
       static_cast<long>(rtcLastRefresh), WiFi.isConnected());
  M5.Display.setEpdMode(epd_mode_t::epd_fast);
  canvas.fillSprite(WHITE);
  int W = M5.Display.width();
  int H = M5.Display.height();
  Page page = static_cast<Page>(rtcCurrentPage);

  canvas.fillRect(0, 0, W, 52, COLOR_HEADER);
  canvas.setTextColor(WHITE);
  canvas.setFont(&fonts::FreeSansBold12pt7b);
  canvas.setTextDatum(middle_left);
  drawClippedString("EDF " + pageName(page), 14, 26, W - 180);

  canvas.setFont(&fonts::Font2);
  canvas.setTextDatum(top_right);
  String status = dashboardStatusLabel();
  canvas.drawString(ellipsizeText(status, 138), W - 14, 8);
  String refreshed = dashboardRefreshLabel(rtcLastRefresh);
  canvas.drawString(ellipsizeText(refreshed, 138), W - 14, 28);

  Bucket* buckets = rtcWeek;
  int count = 7;
  if (page == PAGE_MONTH) {
    buckets = rtcMonth;
    count = max(1, rtcMonthDays);
  } else if (page == PAGE_YEAR) {
    buckets = rtcYear;
    count = YEAR_MONTHS;
  }

  int chartX = 14;
  int chartW = W - 28;
  int footerY = H - 32;
  int legendY = footerY - 18;
  int chartTop = 64;
  int chartGap = 18;
  int chartBottom = legendY - 8;
  int chartH = max(120, (chartBottom - chartTop - chartGap) / 2);
  drawStackedChart(chartX, chartTop, chartW, chartH, "Cost", buckets, count, true);
  drawStackedChart(chartX, chartTop + chartH + chartGap, chartW, chartH, "Energy", buckets, count, false);

  canvas.drawFastHLine(0, footerY, W, BLACK);
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(BLACK);
  canvas.setTextDatum(middle_left);
  drawClippedString(batteryLabel(), 12, footerY + 15, 82);
  canvas.fillRect(12, legendY + 2, 10, 10, COLOR_ELECTRICITY);
  canvas.drawString("Electric", 28, legendY + 7);
  canvas.fillRect(96, legendY + 2, 10, 10, COLOR_GAS);
  canvas.drawString("Gas", 112, legendY + 7);
  canvas.setTextDatum(middle_right);
  canvas.drawString("G10 Up  G9 Down  G1 Refresh", W - 10, footerY + 15);

  if (rtcLastError[0] != '\0') {
    canvas.setTextColor(RED);
    canvas.setTextDatum(top_left);
    drawClippedString(rtcLastError, 14, 54, W - 28);
  }

  canvas.pushSprite(0, 0);
}
