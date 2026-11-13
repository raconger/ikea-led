#pragma once

#ifdef ESP32

#include "PluginManager.h"
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <map>
#include <string>
#include <vector>

// Personal settings live in a gitignored file; fall back to sane defaults.
#if __has_include("home_location.h")
#include "home_location.h"
#endif

#ifndef HOME_LAT
#define HOME_LAT 39.8561 // Denver International Airport
#define HOME_LON -104.6737
#endif
#ifndef FLIGHT_RADIUS_NM
#define FLIGHT_RADIUS_NM 15.0f
#endif
#ifndef FLIGHT_OVERHEAD_NM
#define FLIGHT_OVERHEAD_NM 4.0f
#endif
#ifndef FLIGHT_OVERHEAD_MAX_ALT_FT
#define FLIGHT_OVERHEAD_MAX_ALT_FT 30000
#endif

class AsyncWebServer;

struct Aircraft
{
  char callsign[9];   // e.g. "UAL493", trimmed
  char type[5];       // ICAO type designator, e.g. "B739"
  float east;         // nm east of home at `positionAt`
  float north;        // nm north of home at `positionAt`
  float groundSpeed;  // knots
  float track;        // degrees true
  int altitude;       // feet, barometric
  int verticalRate;   // feet per minute
  uint32_t positionAt; // millis() timestamp the position refers to
};

struct Route
{
  bool known;      // adsbdb has a route for this callsign
  bool plausible;  // the plane's position fits the origin -> destination line
  char origin[5];
  char destination[5];
};

class FlightTrackerPlugin : public Plugin
{
public:
  FlightTrackerPlugin();
  void setup() override;
  void loop() override;
  void teardown() override;
  const char *getName() const override;

  // Serves the browser simulator at /flights/ plus the JSON it reads.
  void registerRoutes(AsyncWebServer &server);

private:
  enum DisplayState
  {
    RADAR,
    OVERHEAD_INFO,
  };

  // --- shared between fetch task and render loop (guarded by mutex) ---
  SemaphoreHandle_t mutex = nullptr;
  std::vector<Aircraft> aircraft;
  std::map<std::string, Route> routeCache;
  bool hasData = false;
  uint32_t lastFetchAt = 0;

  // --- fetch task ---
  TaskHandle_t fetchTask = nullptr;
  std::atomic<bool> active{false};
  uint32_t fetchIntervalMs = 10000;

  static void fetchTaskEntry(void *param);
  void fetchLoop();
  bool fetchAircraft();
  void lookupRoutes();
  bool fetchRoute(const char *callsign, float east, float north, Route &route);

  // --- rendering (plugin task only) ---
  DisplayState state = RADAR;
  uint32_t lastFrame = 0;
  uint32_t stateStartedAt = 0;
  std::string lastShownCallsign;
  uint32_t lastShownAt = 0;
  std::string infoCallsign;

  // Frames are composed off-screen and copied in one go: the LED timer ISR
  // reads the live buffer continuously, so clear-then-draw would flicker.
  uint8_t frame[ROWS * COLS];

  std::vector<Aircraft> snapshot();
  static void project(const Aircraft &a, uint32_t now, float &east, float &north);
  static uint8_t altitudeBrightness(int altitude);
  void plot(int x, int y, uint8_t brightness);
  void line(int x0, int y0, int x1, int y1, uint8_t brightness);
  void smallDigit(int x, int y, int digit, uint8_t brightness);
  void present();
  const Aircraft *findOverhead(const std::vector<Aircraft> &list, uint32_t now) const;
  void drawRadar(const std::vector<Aircraft> &list, uint32_t now);
  void drawInfo(const Aircraft &a);
  void showOverhead(const Aircraft &a);
};

#endif
