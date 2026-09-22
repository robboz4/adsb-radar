"""
ADS-B Aircraft Radar for the 1.28" round GC9A01A LCD on Raspberry Pi.

Draws a radar-scope view: your location at the center, range rings,
and aircraft positioned by real-time bearing/distance. Each aircraft
is shown as a single text label (an asterisk marker + callsign +
altitude) -- marker and text are baked into one Label object so they
always appear together, with no possibility of one updating before
the other during a slow refresh. Refreshes periodically from adsb.lol.

Design note: the background, range rings, and center marker are drawn
directly into ONE bitmap that is fully repainted (cleared + redrawn)
every cycle, and each refresh cycle first wipes to a blank scene before
drawing the new one. This avoids ghosting/trails that occur when many
small separate sprites or labels are added/removed individually --
Blinka's displayio dirty-region tracking can miss repainting the exact
old footprint of a removed sprite. A full wipe + full repaint guarantees
every pixel is touched and no stale frame is ever partially visible.

Run with: python3 adsb_radar_display.py
Stop with: Ctrl+C
"""

import time
import math
import board
import busio
import displayio
import digitalio
import terminalio
import fourwire
from adafruit_display_text import label
from adafruit_gc9a01a import GC9A01A

import adsb_radar_core as core

# ---------------- CONFIG ----------------
CENTER = (120, 120)
MAX_RADIUS_PX = 110       # outermost ring radius in pixels
RING_COUNT = 3            # number of range rings (e.g. 3 rings = thirds of RADIUS_NM)
REFRESH_SECONDS = 10      # how often to re-query the API (seconds)
MAX_AIRCRAFT_LABELS = 12  # avoid clutter if many aircraft are in range

COLOR_BG = 0x001A00        # near-black green tint (classic radar look)
COLOR_RING = 0x00AA00      # green range rings
COLOR_AIRCRAFT = 0xFFAA00  # amber - used for aircraft marker+callsign labels
COLOR_TEXT = 0x00FF00      # bright green labels
COLOR_CENTER = 0xFFFFFF    # white center marker

# Palette indices (single shared palette for the whole scene bitmap)
IDX_BG = 0
IDX_RING = 1
IDX_AIRCRAFT = 2
IDX_CENTER = 3

# ---------------- DISPLAY SETUP ----------------
displayio.release_displays()

spi = busio.SPI(clock=board.SCLK, MOSI=board.MOSI)
tft_cs = board.CE0
tft_dc = board.D25
tft_rst = board.D27

bl = digitalio.DigitalInOut(board.D18)
bl.direction = digitalio.Direction.OUTPUT
bl.value = True

display_bus = fourwire.FourWire(spi, command=tft_dc, chip_select=tft_cs,
                                 reset=tft_rst, baudrate=24000000)
display = GC9A01A(display_bus, width=240, height=240)
display.auto_refresh = False  # we control exactly when to redraw

# ---------------- SCENE: one bitmap for background + rings + dots ----------------
main_group = displayio.Group()
display.root_group = main_group

scene_bitmap = displayio.Bitmap(240, 240, 4)  # 4 palette entries
scene_palette = displayio.Palette(4)
scene_palette[IDX_BG] = COLOR_BG
scene_palette[IDX_RING] = COLOR_RING
scene_palette[IDX_AIRCRAFT] = COLOR_AIRCRAFT
scene_palette[IDX_CENTER] = COLOR_CENTER

scene_sprite = displayio.TileGrid(scene_bitmap, pixel_shader=scene_palette, x=0, y=0)
main_group.append(scene_sprite)

# Status text (top of screen) - shows last update / aircraft count.
# Kept as a separate Label since it sits over a fixed, simple background
# area; if it ever ghosts too, move it into the bitmap as plain pixels.
status_label = label.Label(terminalio.FONT, text="Starting...", color=COLOR_TEXT,
                            x=60, y=18, scale=1)
main_group.append(status_label)

# Aircraft text labels live in their own group, fully rebuilt each cycle.
label_group = displayio.Group()
main_group.append(label_group)


def fill_background(bitmap):
    """Reset every pixel in the scene bitmap back to background color."""
    bitmap.fill(IDX_BG)


def draw_ring(bitmap, cx, cy, radius, palette_index):
    """Plot a circle outline directly into the bitmap (no sprites)."""
    steps = max(72, int(radius * 4))
    for i in range(steps):
        angle = 2 * math.pi * i / steps
        x = int(cx + radius * math.cos(angle))
        y = int(cy + radius * math.sin(angle))
        if 0 <= x < bitmap.width and 0 <= y < bitmap.height:
            bitmap[x, y] = palette_index


def draw_center_marker(bitmap, cx, cy, palette_index, size=2):
    for dx in range(-size, size + 1):
        for dy in range(-size, size + 1):
            x, y = cx + dx, cy + dy
            if 0 <= x < bitmap.width and 0 <= y < bitmap.height:
                bitmap[x, y] = palette_index


def redraw_scene():
    """Fully repaint background, range rings, and center marker.

    Aircraft are NOT drawn here -- they're rendered entirely as text
    labels (marker + callsign + altitude in one Label) by rebuild_labels().
    """
    fill_background(scene_bitmap)

    for i in range(1, RING_COUNT + 1):
        r = int(MAX_RADIUS_PX * i / RING_COUNT)
        draw_ring(scene_bitmap, CENTER[0], CENTER[1], r, IDX_RING)

    draw_center_marker(scene_bitmap, CENTER[0], CENTER[1], IDX_CENTER)


def rebuild_labels(points):
    """Clear and rebuild the text label group for aircraft markers + callsign/alt.

    The marker (an asterisk) is prepended directly to each label's text,
    so the position-marker and the callsign/altitude text are a single
    Label object -- one atomic unit on screen. This avoids the marker and
    text becoming visible at different moments during a slow refresh.
    """
    while len(label_group) > 0:
        label_group.pop()

    for idx, p in enumerate(points[:MAX_AIRCRAFT_LABELS]):
        x, y = p["x"], p["y"]
        alt_k = round(p["alt_ft"] / 100)  # e.g. 350 = 35,000 ft
        text = f"*{p['callsign']} {alt_k}"
        txt_label = label.Label(
            terminalio.FONT, text=text, color=COLOR_AIRCRAFT,
            x=min(max(0, x - 5), 240 - len(text) * 6),
            y=min(max(8, y), 232), scale=1,
        )
        label_group.append(txt_label)


# Initial static draw (no aircraft labels yet)
redraw_scene()
display.refresh()

# ---------------- MAIN LOOP ----------------
# print("Starting radar loop. Press Ctrl+C to stop.")
while True:
    try:
        points = core.get_radar_points(
            lat=core.HOME_LAT, lon=core.HOME_LON, radius_nm=core.RADIUS_NM,
            center=CENTER, max_radius_px=MAX_RADIUS_PX,
        )

        # Step 1: wipe to a clean radar background (no aircraft labels).
        # This guarantees no stale pixels linger from the previous frame
        # before we start drawing the new one.
        redraw_scene()
        scene_bitmap.dirty()
        while len(label_group) > 0:
            label_group.pop()
        display.refresh()

        # Step 2: draw the new frame with current aircraft positions.
        # Marker + callsign + altitude are one Label per aircraft, so
        # they always appear together with no ordering artifacts.
        status_label.text = f"{len(points)} ac  {time.strftime('%H:%M:%S')}"
        redraw_scene()
        scene_bitmap.dirty()
        rebuild_labels(points)
        display.refresh()

        # print(f"Updated: {len(points)} aircraft in range.")
    except Exception as e:
        # Never let a transient network/API hiccup kill the display loop
        print(f"[main loop] error this cycle: {e}")
        status_label.text = "update error"
        display.refresh()

    time.sleep(REFRESH_SECONDS)
