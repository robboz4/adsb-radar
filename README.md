# ADS-B Radar Display

A live aircraft radar scope for a small round LCD, built from ADS-B data. Runs
on either a Raspberry Pi Zero W or an XIAO ESP32-C3, both driving a 1.28"
GC9A01A round display (240×240, SPI).

## Features

- Live traffic from [adsb.lol](https://adsb.lol), with [adsb.fi](https://adsb.fi)
  as a fallback
- Bearing/distance radar scope centred on a fixed home location, with range
  rings and aircraft markers
- Sweep animation — a rotating arm reveals aircraft as it passes their bearing
- Phosphor-fade rendering — aircraft and labels fade out over time rather
  than disappearing instantly, mimicking an old CRT radar scope
- Two independent implementations sharing the same look and data source, so
  you can run this on whichever board you have on hand

## Hardware

| Platform             | Files                                          |
|----------------------|-------------------------------------------------|
| Raspberry Pi Zero W  | `adsb_radar_core.py`, `adsb_radar_display.py`   |
| XIAO ESP32-C3        | `WW11_RADAR-Final.ino`                          |

Both drive a 1.28" round GC9A01A SPI LCD (240×240).

### Raspberry Pi Zero W

- CircuitPython/displayio via Blinka, using `adafruit-circuitpython-gc9a01a`
  and `fourwire.FourWire`
- `adsb_radar_core.py` handles the adsb.lol/adsb.fi data and bearing/distance
  math; `adsb_radar_display.py` draws the scope
- Refresh is roughly 20 seconds/frame — a limitation of displayio's software
  rendering on the Zero, not the data source
- Run with the `radar` shell alias, or `python3 adsb_radar_display.py`

### XIAO ESP32-C3

- `WW11_RADAR-Final.ino`, built with Arduino_GFX, WiFiClientSecure,
  HTTPClient, and ArduinoJson
- Pin mapping (SDA/SCL labels instead of the Pi's CLK/DIN):

  | Signal | Pin          |
  |--------|--------------|
  | SDA    | D10 / GPIO10 |
  | SCL    | D8 / GPIO8   |
  | DC     | D4 / GPIO6   |
  | CS     | D5 / GPIO7   |
  | RST    | D6 / GPIO21  |

- **Built-in setup portal** — no need to hardcode WiFi credentials or a
  location in the sketch. If no WiFi is configured, the display shows the
  access point details instead of the radar scope. Connect to that access
  point at `192.168.4.1` from a phone, tablet, or computer and enter your
  WiFi network and your latitude/longitude from there.
- **Reset button** — a button on the back panel clears the saved WiFi and
  location settings. Hold it down for 3 seconds while powering the board on,
  then reconnect to `192.168.4.1` to reconfigure.

## Case

![3D-printed case](images/IMG_8464.JPG)

- `RadarCase-Shell2.stl` and `RadarCase-RearCover.stl` are a 3D-printable
  enclosure sized for the ESP32-C3 build: sloped display face, corner mounting
  bosses for M3 heat-set inserts, and a 32 mm through-hole with internal
  guide posts that the display slides into from the top
- The rear cover includes a cutout for the reset button used to clear WiFi/
  location settings
- A matching Raspberry Pi enclosure hasn't been designed yet

## Configuration

- **ESP32-C3:** no code editing needed — WiFi and location are set through
  the `192.168.4.1` setup portal described above.
- **Raspberry Pi:** set your location directly in `adsb_radar_core.py`:

  ```python
  HOME_LAT = 38.80579292402006
  HOME_LON = -121.11277380796557
  ```

## Roadmap

- Traffic filtering by commercial / private / military category
- A 3D-printable case for the Raspberry Pi build
