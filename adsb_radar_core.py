"""
Core logic for the ADS-B radar display: fetching aircraft data and
converting lat/lon positions into bearing/distance/x,y for the screen.

This module has no display dependencies, so it can be tested standalone
on any machine with internet access (e.g. `python3 adsb_radar_core.py`).
"""

import math
import urllib.request
import json

# ---- CONFIGURE YOUR LOCATION HERE ----
HOME_LAT =    # <-- replace with your latitude
HOME_LON =    # <-- replace with your longitude
RADIUS_NM = 10       # tracking radius in nautical miles

# 38.80579292402006, -121.11277380796557

# adsb.lol public API - no key required
# Pattern: https://api.adsb.lol/v2/point/{lat}/{lon}/{radius_nm}
ADSB_LOL_URL = "https://api.adsb.lol/v2/point/{lat}/{lon}/{radius}"

# Fallback network with an identical API shape, in case adsb.lol is
# unreachable or rate-limited.
ADSB_FI_URL = "https://opendata.adsb.fi/api/v2/point/{lat}/{lon}/{radius}"


def fetch_aircraft(lat=HOME_LAT, lon=HOME_LON, radius_nm=RADIUS_NM, timeout=8):
    """
    Fetch nearby aircraft as a list of dicts. Tries adsb.lol first,
    falls back to adsb.fi on failure. Returns [] on total failure
    (never raises) so the display loop can keep running.
    """
    for base_url in (ADSB_LOL_URL, ADSB_FI_URL):
        url = base_url.format(lat=lat, lon=lon, radius=radius_nm)
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "pi-radar/1.0"})
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                data = json.loads(resp.read().decode("utf-8"))
                aircraft = data.get("ac") or data.get("aircraft") or []
                return aircraft
        except Exception as e:
            print(f"[adsb] fetch failed from {url}: {e}")
            continue
    return []


def bearing_distance_nm(lat1, lon1, lat2, lon2):
    """
    Great-circle bearing (degrees, 0=North/clockwise) and distance
    (nautical miles) from point 1 to point 2.
    """
    R_NM = 3440.065  # Earth radius in nautical miles

    phi1, phi2 = math.radians(lat1), math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlambda = math.radians(lon2 - lon1)

    # Haversine distance
    a = (math.sin(dphi / 2) ** 2
         + math.cos(phi1) * math.cos(phi2) * math.sin(dlambda / 2) ** 2)
    c = 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a))
    distance = R_NM * c

    # Initial bearing
    y = math.sin(dlambda) * math.cos(phi2)
    x = (math.cos(phi1) * math.sin(phi2)
         - math.sin(phi1) * math.cos(phi2) * math.cos(dlambda))
    bearing = (math.degrees(math.atan2(y, x)) + 360) % 360

    return bearing, distance


def polar_to_xy(bearing_deg, distance_nm, max_range_nm, center, max_radius_px):
    """
    Convert bearing/distance into screen x,y pixel coordinates.
    bearing_deg: 0 = north/up, clockwise.
    center: (cx, cy) pixel coords of screen center.
    max_radius_px: pixel radius corresponding to max_range_nm.
    """
    cx, cy = center
    if max_range_nm <= 0:
        scale = 0
    else:
        scale = min(distance_nm / max_range_nm, 1.0)
    r_px = scale * max_radius_px

    # Convert compass bearing (0=N, clockwise) to standard math angle,
    # then to screen coords (y grows downward).
    angle_rad = math.radians(bearing_deg)
    x = cx + r_px * math.sin(angle_rad)
    y = cy - r_px * math.cos(angle_rad)
    return int(x), int(y)


def get_radar_points(lat=HOME_LAT, lon=HOME_LON, radius_nm=RADIUS_NM,
                      center=(120, 120), max_radius_px=110):
    """
    Fetch aircraft and return a list of dicts ready for drawing:
    [{x, y, callsign, alt_ft, distance_nm, bearing_deg}, ...]
    """
    aircraft = fetch_aircraft(lat, lon, radius_nm)
    points = []
    for ac in aircraft:
        ac_lat = ac.get("lat")
        ac_lon = ac.get("lon")
        if ac_lat is None or ac_lon is None:
            continue  # skip aircraft with no current position

        bearing, distance = bearing_distance_nm(lat, lon, ac_lat, ac_lon)
        if distance > radius_nm:
            continue  # API radius search is approximate; double check

        x, y = polar_to_xy(bearing, distance, radius_nm, center, max_radius_px)

        callsign = (ac.get("flight") or ac.get("r") or ac.get("hex") or "?").strip()
        alt = ac.get("alt_baro")
        if alt in (None, "ground"):
            alt_ft = 0
        else:
            alt_ft = alt

        points.append({
            "x": x,
            "y": y,
            "callsign": callsign,
            "alt_ft": alt_ft,
            "distance_nm": round(distance, 1),
            "bearing_deg": round(bearing),
        })
    return points


if __name__ == "__main__":
    # Quick standalone test: run this file directly (no display needed)
    # to confirm network access and data parsing work before wiring in
    # the screen code.
    print(f"Querying aircraft within {RADIUS_NM} nm of ({HOME_LAT}, {HOME_LON})...")
    pts = get_radar_points()
    print(f"Found {len(pts)} aircraft with valid positions:\n")
    for p in pts:
        print(f"  {p['callsign']:<10} alt={p['alt_ft']:>6} ft  "
              f"dist={p['distance_nm']:>5} nm  bearing={p['bearing_deg']:>3}°  "
              f"xy=({p['x']},{p['y']})")

