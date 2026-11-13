#ifdef ESP32

#include "plugins/FlightTrackerPlugin.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <algorithm>
#include <math.h>

// Data sources (free, no API key):
//   aircraft: https://api.adsb.lol  (fallback: https://opendata.adsb.fi)
//   routes:   https://api.adsbdb.com
// Both aircraft APIs use the readsb JSON format, so a local ADS-B receiver's
// /data/aircraft.json could be dropped in as a source as well.

namespace
{
constexpr float DEG = M_PI / 180.0f;
constexpr uint32_t FRAME_MS = 50;
constexpr uint32_t BASE_INTERVAL_MS = 10000;
constexpr uint32_t MAX_INTERVAL_MS = 120000;
constexpr uint32_t STALE_MS = 90000;
constexpr float MAX_EXTRAPOLATE_S = 60.0f;
constexpr uint32_t INFO_HOLD_MS = 8000;
constexpr uint32_t RESHOW_AFTER_MS = 10UL * 60 * 1000;
constexpr size_t ROUTE_CACHE_MAX = 64;
constexpr int ROUTE_LOOKUPS_PER_CYCLE = 2;
constexpr int HOME_X = COLS / 2;
constexpr int HOME_Y = ROWS / 2;

// Radar flight-path pulse
constexpr float PATH_BEHIND_PX = 4.0f;    // path drawn behind the plane
constexpr float PATH_AHEAD_PX = 3.0f;     // and ahead of it
constexpr float COMET_TAIL_PX = 2.5f;     // length of the fading comet
constexpr float AHEAD_DIMMING = 0.6f;
constexpr float PULSE_MS_AT_450KT = 1100; // faster planes pulse faster
constexpr float PULSE_MIN_MS = 700;
constexpr float PULSE_MAX_MS = 2600;

// Stable per-plane offset in [0, 1) so pulses don't fire in unison.
float phaseSeed(const char *callsign)
{
  uint32_t hash = 2166136261u; // FNV-1a
  for (const char *c = callsign; *c; c++)
  {
    hash = (hash ^ (uint8_t)*c) * 16777619u;
  }
  return (hash >> 8) / 16777216.0f;
}

float cosHomeLat()
{
  static const float c = cosf(HOME_LAT * DEG);
  return c;
}

// Flat-earth projection: plenty accurate over a ~20 nm radius.
void toLocal(double lat, double lon, float &east, float &north)
{
  north = (lat - HOME_LAT) * 60.0;
  east = (lon - HOME_LON) * 60.0 * cosHomeLat();
}

float haversineNm(double lat1, double lon1, double lat2, double lon2)
{
  double dLat = (lat2 - lat1) * DEG;
  double dLon = (lon2 - lon1) * DEG;
  double a = sin(dLat / 2) * sin(dLat / 2) +
             cos(lat1 * DEG) * cos(lat2 * DEG) * sin(dLon / 2) * sin(dLon / 2);
  return 3440.065 * 2 * atan2(sqrt(a), sqrt(1 - a));
}

// GET a URL and parse the JSON body through a filter. Returns the HTTP status,
// or a negative number on connection/parse failure.
int getJson(const char *url, bool http10, JsonDocument &doc, const JsonDocument &filter)
{
  WiFiClientSecure client;
  client.setInsecure(); // no CA bundle on device; data is public and read-only
  HTTPClient http;
  http.useHTTP10(http10);
  http.setConnectTimeout(8000);
  http.setTimeout(10000);
  http.setUserAgent("ikea-led-flight-tracker");

  if (!http.begin(client, url))
  {
    return -1;
  }

  int code = http.GET();
  if (code == HTTP_CODE_OK)
  {
    DeserializationError err;
    if (http.getSize() > 0)
    {
      // Known length: stream straight into the parser, never buffering the
      // ~20 KB body.
      err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    }
    else
    {
      // Unknown length (chunked/close-delimited): let HTTPClient assemble it.
      String body = http.getString();
      err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
    }
    if (err)
    {
      Serial.printf("[flights] JSON error: %s\n", err.c_str());
      code = -2;
    }
  }
  http.end();
  return code;
}

void copyTrimmed(char *dest, size_t size, const char *src)
{
  strncpy(dest, src ? src : "", size - 1);
  dest[size - 1] = '\0';
  for (int i = strlen(dest) - 1; i >= 0 && dest[i] == ' '; i--)
  {
    dest[i] = '\0';
  }
}
} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

FlightTrackerPlugin::FlightTrackerPlugin()
{
  // Created up front: the web routes can read data before setup() ever runs.
  mutex = xSemaphoreCreateMutex();
}

void FlightTrackerPlugin::setup()
{
  state = RADAR;
  lastFrame = 0;
  active = true;

  // The fetch task lives for the lifetime of the device and idles while the
  // plugin is inactive. Deleting a task mid-HTTPS-request would leak the TLS
  // session, so parking it is safer than tearing it down.
  if (fetchTask == nullptr)
  {
    xTaskCreatePinnedToCore(fetchTaskEntry, "flightFetch", 16384, this, 1, &fetchTask, 1);
  }

  memset(frame, 0, sizeof(frame));
  present();
}

void FlightTrackerPlugin::teardown()
{
  active = false;
}

const char *FlightTrackerPlugin::getName() const
{
  return "Flights";
}

// ---------------------------------------------------------------------------
// Fetching (runs on its own task so HTTPS never stalls the animation)
// ---------------------------------------------------------------------------

void FlightTrackerPlugin::fetchTaskEntry(void *param)
{
  static_cast<FlightTrackerPlugin *>(param)->fetchLoop();
}

void FlightTrackerPlugin::fetchLoop()
{
  for (;;)
  {
    if (!active || WiFi.status() != WL_CONNECTED)
    {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    uint32_t started = millis();
    if (fetchAircraft())
    {
      fetchIntervalMs = BASE_INTERVAL_MS;
      lookupRoutes();
    }
    else
    {
      // Back off so a dead API (or a rate limit) isn't hammered.
      fetchIntervalMs = std::min(fetchIntervalMs * 2, MAX_INTERVAL_MS);
    }

    while (active && millis() - started < fetchIntervalMs)
    {
      vTaskDelay(pdMS_TO_TICKS(250));
    }
  }
}

bool FlightTrackerPlugin::fetchAircraft()
{
  // Round the query point to 0.01 deg (~1 km) so the exact home location never
  // leaves the device. The query radius covers the display corners plus slack.
  double queryLat = round(HOME_LAT * 100) / 100.0;
  double queryLon = round(HOME_LON * 100) / 100.0;
  int radius = (int)ceilf(FLIGHT_RADIUS_NM * 1.42f + 2);

  JsonDocument filter;
  for (const char *key : {"ac", "aircraft"}) // adsb.lol vs adsb.fi
  {
    JsonObject f = filter[key][0].to<JsonObject>();
    for (const char *field :
         {"flight", "t", "lat", "lon", "alt_baro", "gs", "track", "baro_rate", "seen_pos"})
    {
      f[field] = true;
    }
  }

  char url[128];
  JsonDocument doc;

  snprintf(url, sizeof(url), "https://api.adsb.lol/v2/point/%.2f/%.2f/%d", queryLat, queryLon, radius);
  int code = getJson(url, false, doc, filter); // adsb.lol requires HTTP/1.1
  if (code != HTTP_CODE_OK)
  {
    Serial.printf("[flights] adsb.lol failed (%d), trying adsb.fi\n", code);
    doc.clear();
    snprintf(url,
             sizeof(url),
             "https://opendata.adsb.fi/api/v2/lat/%.2f/lon/%.2f/dist/%d",
             queryLat,
             queryLon,
             radius);
    code = getJson(url, true, doc, filter);
    if (code != HTTP_CODE_OK)
    {
      Serial.printf("[flights] adsb.fi failed (%d)\n", code);
      return false;
    }
  }

  JsonArray list = doc["ac"].is<JsonArray>() ? doc["ac"].as<JsonArray>()
                                             : doc["aircraft"].as<JsonArray>();
  uint32_t now = millis();
  std::vector<Aircraft> fresh;
  fresh.reserve(list.size());

  for (JsonObject a : list)
  {
    // Skip entries without a position, and planes on the ground
    // (alt_baro is the string "ground" for those).
    if (!a["lat"].is<float>() || !a["lon"].is<float>() || !a["alt_baro"].is<float>())
    {
      continue;
    }

    Aircraft ac{};
    copyTrimmed(ac.callsign, sizeof(ac.callsign), a["flight"] | "");
    copyTrimmed(ac.type, sizeof(ac.type), a["t"] | "");
    toLocal(a["lat"].as<double>(), a["lon"].as<double>(), ac.east, ac.north);
    ac.groundSpeed = a["gs"] | 0.0f;
    ac.track = a["track"] | 0.0f;
    ac.altitude = a["alt_baro"].as<int>();
    ac.verticalRate = a["baro_rate"] | 0;
    float positionAge = a["seen_pos"] | 0.0f;
    ac.positionAt = now - (uint32_t)(positionAge * 1000);
    fresh.push_back(ac);
  }

  size_t count = fresh.size();
  xSemaphoreTake(mutex, portMAX_DELAY);
  aircraft.swap(fresh);
  hasData = true;
  lastFetchAt = now;
  xSemaphoreGive(mutex);

  Serial.printf("[flights] %u aircraft\n", (unsigned)count);
  return true;
}

void FlightTrackerPlugin::lookupRoutes()
{
  struct Candidate
  {
    std::string callsign;
    float east, north, distance;
  };
  std::vector<Candidate> todo;

  // Look up routes well before a plane reaches the overhead zone, so the
  // answer is cached by the time we want to display it.
  xSemaphoreTake(mutex, portMAX_DELAY);
  for (const Aircraft &a : aircraft)
  {
    if (!a.callsign[0] || a.altitude > FLIGHT_OVERHEAD_MAX_ALT_FT)
    {
      continue;
    }
    // US tail numbers (N123AB) are private flights without published routes.
    if (a.callsign[0] == 'N' && isdigit((unsigned char)a.callsign[1]))
    {
      continue;
    }
    float distance = hypotf(a.east, a.north);
    if (distance > FLIGHT_OVERHEAD_NM * 2.5f || routeCache.count(a.callsign))
    {
      continue;
    }
    todo.push_back({a.callsign, a.east, a.north, distance});
  }
  xSemaphoreGive(mutex);

  std::sort(todo.begin(), todo.end(), [](const Candidate &x, const Candidate &y) {
    return x.distance < y.distance;
  });

  int lookups = 0;
  for (const Candidate &c : todo)
  {
    if (lookups++ >= ROUTE_LOOKUPS_PER_CYCLE || !active)
    {
      break;
    }
    Route route;
    if (fetchRoute(c.callsign.c_str(), c.east, c.north, route))
    {
      xSemaphoreTake(mutex, portMAX_DELAY);
      if (routeCache.size() >= ROUTE_CACHE_MAX)
      {
        routeCache.clear();
      }
      routeCache[c.callsign] = route;
      xSemaphoreGive(mutex);
    }
  }
}

// Returns true when the answer is definitive (found, or unknown callsign) and
// should be cached; false on transient errors so it's retried next cycle.
bool FlightTrackerPlugin::fetchRoute(const char *callsign, float east, float north, Route &route)
{
  route = Route{};

  char url[96];
  snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/callsign/%s", callsign);

  JsonDocument filter;
  for (const char *end : {"origin", "destination"})
  {
    JsonObject f = filter["response"]["flightroute"][end].to<JsonObject>();
    for (const char *field : {"iata_code", "icao_code", "latitude", "longitude"})
    {
      f[field] = true;
    }
  }

  JsonDocument doc;
  int code = getJson(url, true, doc, filter);
  if (code == HTTP_CODE_NOT_FOUND)
  {
    Serial.printf("[flights] %s: no route known\n", callsign);
    return true;
  }
  if (code != HTTP_CODE_OK)
  {
    return false;
  }

  JsonObject origin = doc["response"]["flightroute"]["origin"];
  JsonObject destination = doc["response"]["flightroute"]["destination"];
  if (origin.isNull() || destination.isNull())
  {
    return true;
  }

  route.known = true;
  const char *originCode = origin["iata_code"] | "";
  const char *destinationCode = destination["iata_code"] | "";
  copyTrimmed(route.origin, sizeof(route.origin), *originCode ? originCode : origin["icao_code"] | "");
  copyTrimmed(route.destination,
              sizeof(route.destination),
              *destinationCode ? destinationCode : destination["icao_code"] | "");

  // Route databases are keyed by callsign, and airlines reuse flight numbers,
  // so entries go stale. Sanity check: the plane must lie inside an ellipse
  // with the two airports as foci (i.e. roughly on the way between them).
  double planeLat = HOME_LAT + north / 60.0;
  double planeLon = HOME_LON + east / (60.0 * cosHomeLat());
  double oLat = origin["latitude"], oLon = origin["longitude"];
  double dLat = destination["latitude"], dLon = destination["longitude"];
  float direct = haversineNm(oLat, oLon, dLat, dLon);
  float viaPlane = haversineNm(oLat, oLon, planeLat, planeLon) +
                   haversineNm(planeLat, planeLon, dLat, dLon);
  route.plausible = viaPlane <= direct * 1.2f + 30.0f;

  Serial.printf("[flights] %s: %s>%s%s\n",
                callsign,
                route.origin,
                route.destination,
                route.plausible ? "" : " (implausible, hidden)");
  return true;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

std::vector<Aircraft> FlightTrackerPlugin::snapshot()
{
  xSemaphoreTake(mutex, portMAX_DELAY);
  std::vector<Aircraft> copy = aircraft;
  xSemaphoreGive(mutex);
  return copy;
}

// Dead reckoning: move the plane along its track since its last known
// position, so dots glide smoothly between 10 s API updates.
void FlightTrackerPlugin::project(const Aircraft &a, uint32_t now, float &east, float &north)
{
  float seconds = std::min((now - a.positionAt) / 1000.0f, MAX_EXTRAPOLATE_S);
  float travelled = a.groundSpeed * seconds / 3600.0f;
  east = a.east + travelled * sinf(a.track * DEG);
  north = a.north + travelled * cosf(a.track * DEG);
}

// Low planes (the ones you can actually see) are brightest.
uint8_t FlightTrackerPlugin::altitudeBrightness(int altitude)
{
  const int low = 10000, high = 40000;
  const uint8_t bright = MAX_BRIGHTNESS, dim = 40;
  if (altitude <= low)
  {
    return bright;
  }
  if (altitude >= high)
  {
    return dim;
  }
  return bright - (altitude - low) * (bright - dim) / (high - low);
}

void FlightTrackerPlugin::plot(int x, int y, uint8_t brightness)
{
  if (x < 0 || y < 0 || x >= COLS || y >= ROWS)
  {
    return;
  }
  uint8_t &pixel = frame[y * COLS + x];
  pixel = std::max(pixel, brightness); // overlapping planes keep the brighter one
}

void FlightTrackerPlugin::line(int x0, int y0, int x1, int y1, uint8_t brightness)
{
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;)
  {
    plot(x0, y0, brightness);
    if (x0 == x1 && y0 == y1)
    {
      break;
    }
    int e2 = 2 * err;
    if (e2 >= dy)
    {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx)
    {
      err += dx;
      y0 += sy;
    }
  }
}

void FlightTrackerPlugin::smallDigit(int x, int y, int digit, uint8_t brightness)
{
  std::vector<int> bits = Screen.readBytes(smallNumbers[digit]);
  for (size_t i = 0; i < bits.size(); i++)
  {
    if (bits[i])
    {
      plot(x + i % 4, y + i / 4, brightness);
    }
  }
}

void FlightTrackerPlugin::present()
{
  Screen.setRenderBuffer(frame, true);
}

const Aircraft *FlightTrackerPlugin::findOverhead(const std::vector<Aircraft> &list,
                                                  uint32_t now) const
{
  const Aircraft *best = nullptr;
  float bestDistance = FLIGHT_OVERHEAD_NM;
  for (const Aircraft &a : list)
  {
    if (!a.callsign[0] || a.altitude > FLIGHT_OVERHEAD_MAX_ALT_FT ||
        now - a.positionAt > STALE_MS)
    {
      continue;
    }
    float east, north;
    project(a, now, east, north);
    float distance = hypotf(east, north);
    if (distance <= bestDistance)
    {
      bestDistance = distance;
      best = &a;
    }
  }
  return best;
}

void FlightTrackerPlugin::loop()
{
  uint32_t now = millis();
  if (now - lastFrame < FRAME_MS)
  {
    return;
  }
  lastFrame = now;

  std::vector<Aircraft> list = snapshot();

  if (state == OVERHEAD_INFO)
  {
    auto it = std::find_if(list.begin(), list.end(), [&](const Aircraft &a) {
      return infoCallsign == a.callsign;
    });
    if (now - stateStartedAt < INFO_HOLD_MS && it != list.end())
    {
      drawInfo(*it);
      return;
    }
    state = RADAR;
  }

  const Aircraft *overhead = findOverhead(list, now);
  if (overhead &&
      (lastShownCallsign != overhead->callsign || now - lastShownAt > RESHOW_AFTER_MS))
  {
    showOverhead(*overhead);
    return;
  }

  drawRadar(list, now);
}

void FlightTrackerPlugin::drawRadar(const std::vector<Aircraft> &list, uint32_t now)
{
  memset(frame, 0, sizeof(frame));

  bool anyFresh = false;
  const float pixelsPerNm = (COLS / 2) / FLIGHT_RADIUS_NM;

  for (const Aircraft &a : list)
  {
    if (now - a.positionAt > STALE_MS)
    {
      continue;
    }
    anyFresh = true;

    float east, north;
    project(a, now, east, north);
    float px = HOME_X + east * pixelsPerNm;
    float py = HOME_Y - north * pixelsPerNm; // screen y grows downward

    uint8_t brightness = altitudeBrightness(a.altitude);
    float dirX = sinf(a.track * DEG), dirY = -cosf(a.track * DEG); // screen y grows downward

    // Faint fixed tail so heading is readable between pulses.
    plot((int)floorf(px - dirX), (int)floorf(py - dirY), std::max<int>(brightness / 6, 12));

    // "Ghost rider" pulse: a comet repeatedly sweeps along the flight path,
    // from behind the plane, through it, and on ahead of it.
    float periodMs = std::min(std::max(PULSE_MS_AT_450KT * 450.0f / std::max(a.groundSpeed, 60.0f),
                                       PULSE_MIN_MS),
                              PULSE_MAX_MS);
    // Integer modulo first: millis() / float loses precision after a few days.
    uint32_t period = (uint32_t)periodMs;
    float phase = fmodf((now % period) / periodMs + phaseSeed(a.callsign), 1.0f);
    float cometHead = -PATH_BEHIND_PX + phase * (PATH_BEHIND_PX + PATH_AHEAD_PX + COMET_TAIL_PX);
    for (float t = -PATH_BEHIND_PX; t <= PATH_AHEAD_PX; t += 0.5f)
    {
      float behindHead = cometHead - t;
      if (behindHead < 0 || behindHead > COMET_TAIL_PX)
      {
        continue;
      }
      float intensity = 1.0f - behindHead / COMET_TAIL_PX;
      if (t > 0)
      {
        intensity *= AHEAD_DIMMING; // ahead of the plane is only a prediction
      }
      plot((int)floorf(px + dirX * t), (int)floorf(py + dirY * t), brightness * intensity);
    }

    plot((int)floorf(px), (int)floorf(py), brightness);
  }

  if (!anyFresh)
  {
    // No data yet (or the API is down): show a rotating radar sweep.
    float angle = (now % 3000) / 3000.0f * 360.0f * DEG;
    for (int k = 0; k < 2; k++)
    {
      float a = angle - k * 20 * DEG;
      line(HOME_X, HOME_Y, HOME_X + lroundf(7 * sinf(a)), HOME_Y - lroundf(7 * cosf(a)), k ? 20 : 70);
    }
  }

  // Home marker: a slow, dim pulse at the center.
  uint8_t pulse = 25 + 25 * (0.5f + 0.5f * sinf(now / 600.0f));
  plot(HOME_X, HOME_Y, pulse);

  present();
}

void FlightTrackerPlugin::showOverhead(const Aircraft &a)
{
  std::string text = a.callsign;

  Route route{};
  bool haveRoute = false;
  xSemaphoreTake(mutex, portMAX_DELAY);
  auto it = routeCache.find(a.callsign);
  if (it != routeCache.end())
  {
    route = it->second;
    haveRoute = true;
  }
  xSemaphoreGive(mutex);

  if (haveRoute && route.known && route.plausible)
  {
    text += std::string(" ") + route.origin + ">" + route.destination;
  }
  if (a.type[0])
  {
    text += std::string(" ") + a.type;
  }

  Serial.printf("[flights] overhead: %s\n", text.c_str());
  lastShownCallsign = a.callsign;
  lastShownAt = millis();
  infoCallsign = a.callsign;

  Screen.scrollText(text, 50); // blocking, ~5 s

  if (!active)
  {
    return; // user switched plugins mid-scroll
  }
  state = OVERHEAD_INFO;
  stateStartedAt = millis();
}

// Info card after the scroll:
//   top:    arrow pointing where the plane is heading (north = up)
//   bottom: altitude in thousands of feet + climb/descend indicator
void FlightTrackerPlugin::drawInfo(const Aircraft &a)
{
  memset(frame, 0, sizeof(frame));

  const int cx = 7, cy = 4, r = 4;
  float sx = sinf(a.track * DEG), sy = -cosf(a.track * DEG);
  int tipX = cx + lroundf(sx * r), tipY = cy + lroundf(sy * r);
  line(cx - lroundf(sx * r), cy - lroundf(sy * r), tipX, tipY, MAX_BRIGHTNESS);
  for (int side : {-1, 1})
  {
    float headAngle = (a.track + 180 + side * 35) * DEG;
    line(tipX,
         tipY,
         tipX + lroundf(sinf(headAngle) * 3),
         tipY - lroundf(cosf(headAngle) * 3),
         MAX_BRIGHTNESS);
  }

  int thousands = std::min(std::max((a.altitude + 500) / 1000, 0), 99);
  if (thousands >= 10)
  {
    smallDigit(3, 10, thousands / 10, MAX_BRIGHTNESS);
    smallDigit(8, 10, thousands % 10, MAX_BRIGHTNESS);
  }
  else
  {
    smallDigit(5, 10, thousands, MAX_BRIGHTNESS);
  }

  if (a.verticalRate > 300) // climbing
  {
    plot(14, 10, 160);
    line(13, 11, 15, 11, 160);
    line(14, 12, 14, 14, 160);
  }
  else if (a.verticalRate < -300) // descending
  {
    line(14, 10, 14, 12, 160);
    line(13, 13, 15, 13, 160);
    plot(14, 14, 160);
  }
  else // level
  {
    line(13, 12, 15, 12, 160);
  }

  present();
}

// ---------------------------------------------------------------------------
// Browser simulator (http://<device>/flights/)
// ---------------------------------------------------------------------------

#ifdef ENABLE_SERVER
#include <ESPAsyncWebServer.h>

// tools/flight-sim/index.html, linked in via board_build.embed_txtfiles
extern const char flightSimHtmlStart[] asm("_binary_tools_flight_sim_index_html_start");
extern const char flightSimHtmlEnd[] asm("_binary_tools_flight_sim_index_html_end");

void FlightTrackerPlugin::registerRoutes(AsyncWebServer &server)
{
  // Order matters: "/flights" also matches everything below "/flights/",
  // so the API routes are registered first.
  server.on("/flights/api/config", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    JsonObject config = doc["config"].to<JsonObject>();
    // Rounded (~1 km): the device API is CORS-open, so the exact home
    // location stays on the device. Aircraft are sent relative to home.
    config["lat"] = round(HOME_LAT * 100) / 100.0;
    config["lon"] = round(HOME_LON * 100) / 100.0;
    config["radiusNm"] = FLIGHT_RADIUS_NM;
    config["overheadNm"] = FLIGHT_OVERHEAD_NM;
    config["overheadMaxAltFt"] = FLIGHT_OVERHEAD_MAX_ALT_FT;

    JsonObject system = doc["glyphs"]["system"].to<JsonObject>();
    system["sizeX"] = fonts[0].sizeX;
    system["offset"] = fonts[0].offset;
    JsonArray data = system["data"].to<JsonArray>();
    for (const auto &glyph : fonts[0].data)
    {
      JsonArray rows = data.add<JsonArray>();
      for (int row : glyph)
      {
        rows.add(row);
      }
    }
    JsonArray small = doc["glyphs"]["smallNumbers"].to<JsonArray>();
    for (const auto &glyph : smallNumbers)
    {
      JsonArray rows = small.add<JsonArray>();
      for (int row : glyph)
      {
        rows.add(row);
      }
    }

    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response);
  });

  server.on("/flights/api/aircraft", HTTP_GET, [this](AsyncWebServerRequest *request) {
    JsonDocument doc;
    uint32_t now = millis();

    xSemaphoreTake(mutex, portMAX_DELAY);
    doc["source"] = "device";
    doc["active"] = active.load();
    if (hasData)
    {
      doc["dataAgeMs"] = now - lastFetchAt;
    }
    else
    {
      doc["dataAgeMs"] = nullptr;
    }
    JsonArray list = doc["aircraft"].to<JsonArray>();
    for (const Aircraft &a : aircraft)
    {
      JsonObject o = list.add<JsonObject>();
      o["flight"] = a.callsign;
      o["t"] = a.type;
      o["east"] = a.east;
      o["north"] = a.north;
      o["alt_baro"] = a.altitude;
      o["gs"] = a.groundSpeed;
      o["track"] = a.track;
      o["baro_rate"] = a.verticalRate;
      o["seen_pos"] = (now - a.positionAt) / 1000.0f;
      auto route = routeCache.find(a.callsign);
      if (route != routeCache.end())
      {
        JsonObject r = o["route"].to<JsonObject>();
        r["known"] = route->second.known;
        r["plausible"] = route->second.plausible;
        r["origin"] = route->second.origin;
        r["destination"] = route->second.destination;
      }
    }
    xSemaphoreGive(mutex);

    AsyncResponseStream *response = request->beginResponseStream("application/json");
    response->addHeader("Cache-Control", "no-store");
    serializeJson(doc, *response);
    request->send(response);
  });

  server.on("/flights", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->url() != "/flights/")
    {
      // Trailing slash matters: the page fetches relative "api/..." URLs.
      request->redirect("/flights/");
      return;
    }
    request->send(200,
                  "text/html",
                  (const uint8_t *)flightSimHtmlStart,
                  flightSimHtmlEnd - flightSimHtmlStart - 1); // embed_txtfiles adds a NUL
  });
}
#endif

#endif
