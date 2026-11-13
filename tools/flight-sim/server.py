#!/usr/bin/env python3
"""Browser simulator for the Flights plugin, standalone version.

The device serves this same page at http://ikea-led.local/flights/ using its
own live data. This server is for running it without the device:

    python3 tools/flight-sim/server.py      ->  http://localhost:8787

Serves the simulator page and proxies the flight APIs (adsb.lol/adsb.fi send
no CORS headers, so a browser can't call them directly). Settings come from
include/home_location.h and glyphs from src/signs.cpp, so the simulator uses
the same config and font as the firmware. Standard library only.
"""

import json
import re
import sys
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8787
USER_AGENT = "ikea-led-flight-sim"

CONFIG_KEYS = {
    "HOME_LAT": "lat",
    "HOME_LON": "lon",
    "FLIGHT_RADIUS_NM": "radiusNm",
    "FLIGHT_OVERHEAD_NM": "overheadNm",
    "FLIGHT_OVERHEAD_MAX_ALT_FT": "overheadMaxAltFt",
}


def read_config():
    # Same fallbacks as include/plugins/FlightTrackerPlugin.h
    config = {"lat": 39.8561, "lon": -104.6737, "radiusNm": 15.0,
              "overheadNm": 4.0, "overheadMaxAltFt": 30000}
    path = ROOT / "include" / "home_location.h"
    if path.exists():
        for name, value in re.findall(r"#define\s+(\w+)\s+(-?[\d.]+)f?", path.read_text()):
            if name in CONFIG_KEYS:
                config[CONFIG_KEYS[name]] = float(value)
    return config


def parse_byte_rows(block):
    return [[int(b, 16) for b in re.findall(r"0x[0-9A-Fa-f]+", row)]
            for row in re.findall(r"\{\s*(0x[^{}]*)\}", block)]


def read_glyphs():
    src = (ROOT / "src" / "signs.cpp").read_text()
    system = src[src.index('{"system"'):src.index('{"boldnumber"')]
    small = src[src.index("smallNumbers = {"):]
    small = small[:small.index("};")]
    return {"system": {"sizeX": 5, "offset": 32, "data": parse_byte_rows(system)},
            "smallNumbers": parse_byte_rows(small)}


def fetch(url, timeout=10):
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.status, response.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except Exception as e:  # network errors, timeouts
        return 502, json.dumps({"error": str(e)}).encode()


def fetch_aircraft():
    config = read_config()
    # Rounded to ~1 km, matching the firmware: exact location stays local.
    lat, lon = round(config["lat"], 2), round(config["lon"], 2)
    radius = int(-(-(config["radiusNm"] * 1.42 + 2) // 1))
    sources = [
        ("adsb.lol", f"https://api.adsb.lol/v2/point/{lat:.2f}/{lon:.2f}/{radius}"),
        ("adsb.fi", f"https://opendata.adsb.fi/api/v2/lat/{lat:.2f}/lon/{lon:.2f}/dist/{radius}"),
    ]
    status = 502
    for name, url in sources:
        status, body = fetch(url)
        if status == 200:
            data = json.loads(body)
            return 200, {"source": name, "aircraft": data.get("ac") or data.get("aircraft") or []}
    return status, {"error": f"all sources failed (last status {status})"}


class Handler(BaseHTTPRequestHandler):
    def send_json(self, status, payload):
        body = json.dumps(payload).encode() if not isinstance(payload, bytes) else payload
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0]
        if path in ("/", "/index.html"):
            body = (HERE / "index.html").read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
        elif path == "/api/config":
            self.send_json(200, {"config": read_config(), "glyphs": read_glyphs()})
        elif path == "/api/aircraft":
            self.send_json(*fetch_aircraft())
        else:
            self.send_json(404, {"error": "not found"})

    def log_message(self, fmt, *args):
        if "/api/" in (args[0] if args else ""):
            sys.stderr.write("%s\n" % (fmt % args))


if __name__ == "__main__":
    print(f"Flight simulator on http://localhost:{PORT}")
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
