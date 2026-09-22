// ============================================================
// WW-II Phosphor Radar - XIAO ESP32-C3 + GC9A01 Round LCD
// ============================================================
// Authentic WWII PPI radar simulation with captive portal
// configuration -- no recompile needed to change settings.
//
// FIRST TIME SETUP:
//   1. Power on the device
//   2. Connect your phone/laptop to WiFi network "RadarSetup"
//   3. A config page will open automatically (captive portal)
//   4. Enter your WiFi credentials, latitude, longitude, radius
//   5. Hit Save -- device reboots and starts radar mode
//
// RECONFIGURE:
//   Hold external button on D7 (GPIO20) for 3 seconds at
//   power-on to reset settings and re-enter setup mode
//
// Wiring (XIAO ESP32-C3):
//   Display VCC → 3.3V
//   Display GND → GND
//   Display SCL → D8  (GPIO8)
//   Display SDA → D10 (GPIO10)
//   Display DC  → D4  (GPIO6)
//   Display CS  → D5  (GPIO7)
//   Display RST → D6  (GPIO21)
//   Reset button → D7 (GPIO20) to GND
//
// Libraries required:
//   - GFX Library for Arduino (moononournation)
//   - WiFiManager (tzapu)
//   - ArduinoJson (Benoit Blanchon)
// ============================================================

#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <math.h>

// ============================================================
// DISPLAY PINS (XIAO ESP32-C3)
// ============================================================
#define TFT_CS   7    // D5
#define TFT_DC   6    // D4
#define TFT_RST  21   // D6
#define TFT_MOSI 10   // D10
#define TFT_SCLK 8    // D8

// External reset button (momentary switch to GND)
#define BOOT_PIN 20   // D7

// ============================================================
// COLOURS (RGB565)
// ============================================================
#define COLOR_BG      0x0020  // near-black green background
#define COLOR_RING    0x02A0  // static ring green (never fades)
#define COLOR_CENTER  0xFFFF  // white center marker (never fades)
#define COLOR_SWEEP   0x47E6  // bright sweep arm green
#define BLACK         0x0000
#define RED           0xF800
#define WHITE         0xFFFF

// Phosphor brightness steps: full bright → dim → background
// 9 steps for smooth staircase decay over FADE_MS
const uint16_t PHOSPHOR[] = {
    0x07E0,   // step 0: full bright green (100%)
    0x06E0,   // step 1: ~87%
    0x05E0,   // step 2: ~75%
    0x04E0,   // step 3: ~62%
    0x03E0,   // step 4: ~50%
    0x02E0,   // step 5: ~37%
    0x01E0,   // step 6: ~25%
    0x00E0,   // step 7: very dim
    0x0020    // step 8: background (gone)
};
const int PHOSPHOR_STEPS = 9;

// ============================================================
// RADAR GEOMETRY
// ============================================================
#define SCREEN_W   240
#define SCREEN_H   240
#define CENTER_X   120
#define CENTER_Y   120
#define MAX_RADIUS 108  // outermost ring radius in pixels
#define RING_COUNT 3    // number of range rings
#define MAX_AC     30   // max aircraft to track
#define DOT_SIZE   3    // overlap marker dot radius

// ============================================================
// CONFIGURATION (loaded from flash at startup)
// ============================================================
float        HOME_LAT  = 38.80579;   // default fallback
float        HOME_LON  = -121.11277; // default fallback
int          RADIUS_NM = 10;         // default fallback

const unsigned long SWEEP_MS = 2000; // sweep arm duration (ms)
const unsigned long FADE_MS  = 8000; // total fade duration (ms)

// ============================================================
// AIRCRAFT DATA STRUCTURE
// ============================================================
struct Aircraft {
    float bearing_deg;  // bearing from home (0=North, clockwise)
    int   x, y;         // screen pixel coordinates
    int   alt_ft;       // altitude in feet
    char  callsign[10]; // callsign or tail number
};

Aircraft ac_list[MAX_AC];
int      ac_count = 0;

// Label bounding boxes for overlap detection
struct LabelBox {
    int x, y, w, h;
};
LabelBox drawn_boxes[MAX_AC];
int      box_count = 0;

// ============================================================
// GFX + PREFERENCES
// ============================================================
Arduino_DataBus *bus = new Arduino_ESP32SPI(
    TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, GFX_NOT_DEFINED
);
Arduino_GFX *gfx = new Arduino_GC9A01(bus, TFT_RST, 0, true);

Preferences prefs;

// ============================================================
// MATH HELPERS
// ============================================================

// Great-circle bearing from point 1 to point 2
// Returns 0-360 degrees (0=North, clockwise)
float calc_bearing(float lat1, float lon1, float lat2, float lon2) {
    float phi1 = radians(lat1), phi2 = radians(lat2);
    float dl   = radians(lon2 - lon1);
    float y    = sin(dl) * cos(phi2);
    float x    = cos(phi1)*sin(phi2) - sin(phi1)*cos(phi2)*cos(dl);
    float brng = fmod(degrees(atan2(y, x)) + 360.0, 360.0);
    if (brng >= 360.0f) brng = 0.0f;
    return brng;
}

// Haversine distance between two points in nautical miles
float calc_distance_nm(float lat1, float lon1, float lat2, float lon2) {
    const float R = 3440.065;
    float dphi = radians(lat2 - lat1);
    float dl   = radians(lon2 - lon1);
    float phi1 = radians(lat1), phi2 = radians(lat2);
    float a = sin(dphi/2)*sin(dphi/2) +
              cos(phi1)*cos(phi2)*sin(dl/2)*sin(dl/2);
    return R * 2 * atan2(sqrt(a), sqrt(1-a));
}

// Convert bearing + distance to screen x,y pixel coordinates
void polar_to_xy(float brng, float dist_nm, int &x, int &y) {
    float scale = min(dist_nm / (float)RADIUS_NM, 1.0f);
    float r     = scale * MAX_RADIUS;
    float a     = radians(brng);
    x = CENTER_X + (int)(r * sin(a));
    y = CENTER_Y - (int)(r * cos(a));
}

// Sort aircraft by bearing (insertion sort)
void sort_by_bearing() {
    for (int i = 1; i < ac_count; i++) {
        Aircraft key = ac_list[i];
        int j = i - 1;
        while (j >= 0 && ac_list[j].bearing_deg > key.bearing_deg) {
            ac_list[j+1] = ac_list[j];
            j--;
        }
        ac_list[j+1] = key;
    }
}

// ============================================================
// OVERLAP DETECTION
// Check if a proposed label rectangle overlaps any already
// drawn label. Returns true if overlap detected.
// ============================================================
bool overlaps_existing(int lx, int ly, int lw, int lh) {
    for (int i = 0; i < box_count; i++) {
        if (lx < drawn_boxes[i].x + drawn_boxes[i].w &&
            lx + lw > drawn_boxes[i].x &&
            ly < drawn_boxes[i].y + drawn_boxes[i].h &&
            ly + lh > drawn_boxes[i].y) {
            return true;
        }
    }
    return false;
}

// ============================================================
// DRAWING FUNCTIONS
// ============================================================

// Draw center crosshair marker (your location)
void draw_center_marker() {
    gfx->fillCircle(CENTER_X, CENTER_Y, 3, COLOR_CENTER);
    gfx->drawLine(CENTER_X-6, CENTER_Y, CENTER_X+6, CENTER_Y, COLOR_CENTER);
    gfx->drawLine(CENTER_X, CENTER_Y-6, CENTER_X, CENTER_Y+6, COLOR_CENTER);
}

// Draw permanent static background -- rings never fade,
// simulating etched glass overlay on real WWII radar scopes
void draw_static_scene() {
    gfx->fillScreen(COLOR_BG);
    for (int i = 1; i <= RING_COUNT; i++) {
        int r = MAX_RADIUS * i / RING_COUNT;
        gfx->drawCircle(CENTER_X, CENTER_Y, r, COLOR_RING);
    }
    gfx->drawCircle(CENTER_X, CENTER_Y, MAX_RADIUS, COLOR_RING);
    draw_center_marker();
}

// Draw one aircraft label at a given phosphor brightness.
// If label would overlap an existing one, draw a small dot
// instead so the aircraft position is still visible.
void draw_one_aircraft(int idx, uint16_t colour) {
    Aircraft &ac = ac_list[idx];

    int  alt_k = (int)(ac.alt_ft / 100);
    char label[16];
    snprintf(label, sizeof(label), "%s %d", ac.callsign, alt_k);

    int label_len = strlen(label);
    int label_w   = label_len * 12;  // 12px per char at textSize 2
    int label_h   = 16;              // 16px tall at textSize 2

    // Position label to right of dot by default,
    // flip left if it would overflow right edge
    int lx = ac.x + 6;
    if (lx + label_w > SCREEN_W - 2) lx = ac.x - label_w - 6;
    if (lx < 2) lx = 2;

    int ly = ac.y - 4;
    if (ly < label_h)                 ly = ac.y + 6;
    if (ly + label_h > SCREEN_H - 2) ly = SCREEN_H - label_h - 2;

    if (overlaps_existing(lx, ly, label_w, label_h)) {
        // Overlap detected -- draw small dot instead of label
        gfx->fillCircle(ac.x, ac.y, DOT_SIZE, colour);
        Serial.printf("  [overlap] %s -- dot only\n", ac.callsign);
    } else {
        // No overlap -- record bounding box and draw full label
        drawn_boxes[box_count++] = {lx, ly, label_w, label_h};
        gfx->fillRect(lx, ly, label_w + 4, label_h, COLOR_BG);
        gfx->setTextWrap(false);
        gfx->setTextSize(2);
        int cx = lx;
        for (int i = 0; i < label_len; i++) {
            gfx->drawChar(cx, ly, (unsigned char)label[i], colour, COLOR_BG);
            cx += 12;
        }
    }
}

// Draw all aircraft at a given brightness level
void draw_all_aircraft(uint16_t colour) {
    box_count = 0;  // reset overlap tracking for this redraw
    for (int i = 0; i < ac_count; i++) {
        draw_one_aircraft(i, colour);
    }
}

// Draw sweep arm line from center to edge at given angle
void draw_sweep_arm(float angle_deg, uint16_t colour) {
    float a  = radians(angle_deg);
    int   x2 = CENTER_X + (int)(MAX_RADIUS * sin(a));
    int   y2 = CENTER_Y - (int)(MAX_RADIUS * cos(a));
    gfx->drawLine(CENTER_X, CENTER_Y, x2, y2, colour);
}

// Draw status line at top of screen
void draw_status(int count, bool wifi_ok) {
    gfx->fillRect(0, 0, SCREEN_W, 14, COLOR_BG);
    gfx->setTextSize(1);
    gfx->setTextColor(PHOSPHOR[0], COLOR_BG);
    gfx->setCursor(10, 3);
    if (!wifi_ok) {
        gfx->print("No WiFi");
    } else {
        gfx->print(count);
        gfx->print(" ac  ");
        gfx->print(RADIUS_NM);
        gfx->print("nm");
    }
}

// Display a centered message -- used during WiFi setup
void show_message(const char* line1, const char* line2 = nullptr,
                  uint16_t colour = WHITE) {
    gfx->fillScreen(BLACK);
    for (int i = 1; i <= RING_COUNT; i++) {
        int r = MAX_RADIUS * i / RING_COUNT;
        gfx->drawCircle(CENTER_X, CENTER_Y, r, 0x0060);
    }
    gfx->setTextWrap(true);
    gfx->setTextSize(1);
    gfx->setTextColor(colour, BLACK);
    if (line2 == nullptr) {
        gfx->setCursor(10, 115);
        gfx->print(line1);
    } else {
        gfx->setCursor(10, 105);
        gfx->print(line1);
        gfx->setCursor(10, 120);
        gfx->print(line2);
    }
}

// ============================================================
// RADAR SWEEP ANIMATION
// Arm rotates 0->360 over SWEEP_MS revealing aircraft as it
// passes their bearing. Uses surgical erase (previous arm line
// only) to prevent flicker on already-revealed labels.
// ============================================================
void radar_sweep() {
    sort_by_bearing();

    int next_ac = 0;
    box_count = 0;  // reset overlap tracking for new sweep
    unsigned long sweep_start = millis();
    const int STEPS = 180;  // 2 degrees per step

    // Full clean wipe before sweep -- clears previous cycle's
    // faded aircraft. Double fillScreen ensures complete clear.
    gfx->fillScreen(COLOR_BG);
    gfx->fillScreen(COLOR_BG);
    for (int i = 1; i <= RING_COUNT; i++) {
        int r = MAX_RADIUS * i / RING_COUNT;
        gfx->drawCircle(CENTER_X, CENTER_Y, r, COLOR_RING);
    }
    gfx->drawCircle(CENTER_X, CENTER_Y, MAX_RADIUS, COLOR_RING);
    draw_center_marker();

    float prev_angle = -1;

    for (int step = 0; step <= STEPS; step++) {
        float angle = 360.0f * step / STEPS;

        // Surgical erase: repaint previous arm line with background,
        // then restore ring pixels the arm erased
        if (prev_angle >= 0) {
            draw_sweep_arm(prev_angle, COLOR_BG);
            for (int i = 1; i <= RING_COUNT; i++) {
                int r = MAX_RADIUS * i / RING_COUNT;
                float pa = radians(prev_angle);
                int rx = CENTER_X + (int)(r * sin(pa));
                int ry = CENTER_Y - (int)(r * cos(pa));
                gfx->drawPixel(rx, ry, COLOR_RING);
            }
            float pa = radians(prev_angle);
            int rx = CENTER_X + (int)(MAX_RADIUS * sin(pa));
            int ry = CENTER_Y - (int)(MAX_RADIUS * cos(pa));
            gfx->drawPixel(rx, ry, COLOR_RING);
        }

        // Reveal aircraft whose bearing the sweep arm has reached.
        // angle > 0.5 guard prevents reveals before arm has moved.
        while (next_ac < ac_count &&
               ac_list[next_ac].bearing_deg <= angle &&
               angle > 0.5f) {
            Serial.printf("Revealing %s at sweep angle=%.1f brng=%.1f\n",
                ac_list[next_ac].callsign, angle,
                ac_list[next_ac].bearing_deg);
            draw_one_aircraft(next_ac, PHOSPHOR[0]);
            next_ac++;
        }

        // Draw sweep arm at current angle
        draw_sweep_arm(angle, COLOR_SWEEP);
        draw_center_marker();
        prev_angle = angle;

        // Pace sweep to SWEEP_MS total duration
        unsigned long elapsed  = millis() - sweep_start;
        unsigned long expected = (unsigned long)(SWEEP_MS * step / STEPS);
        if (expected > elapsed) delay(expected - elapsed);
    }

    // Clean up: erase final arm, restore outer ring, reveal
    // any remaining aircraft (edge case: bearing near 360)
    draw_sweep_arm(360.0f, COLOR_BG);
    gfx->drawCircle(CENTER_X, CENTER_Y, MAX_RADIUS, COLOR_RING);
    draw_center_marker();
    while (next_ac < ac_count) {
        draw_one_aircraft(next_ac, PHOSPHOR[0]);
        next_ac++;
    }
}

// ============================================================
// PHOSPHOR FADE
// Steps through 9 brightness levels over FADE_MS total.
// Each step: wipe screen, redraw rings (permanent), redraw
// aircraft at next dimmer colour level.
// Rings and center marker never fade -- etched glass effect.
// ============================================================
void phosphor_fade() {
    unsigned long step_ms = FADE_MS / (PHOSPHOR_STEPS - 1);
    // 8000ms / 8 steps = 1000ms per step

    for (int step = 1; step < PHOSPHOR_STEPS; step++) {
        delay(step_ms);

        gfx->fillScreen(COLOR_BG);

        // Rings and center always redrawn solid -- never fade
        for (int i = 1; i <= RING_COUNT; i++) {
            int r = MAX_RADIUS * i / RING_COUNT;
            gfx->drawCircle(CENTER_X, CENTER_Y, r, COLOR_RING);
        }
        gfx->drawCircle(CENTER_X, CENTER_Y, MAX_RADIUS, COLOR_RING);
        draw_center_marker();

        // Redraw aircraft at current fade level
        // Skip last step -- background colour, nothing to draw
        if (step < PHOSPHOR_STEPS - 1) {
            draw_all_aircraft(PHOSPHOR[step]);
        }

        draw_status(ac_count, WiFi.status() == WL_CONNECTED);
    }
}

// ============================================================
// ADS-B DATA FETCH
// Tries adsb.lol first, falls back to adsb.fi on failure.
// Uses getString() to avoid IncompleteInput parse errors.
// ============================================================
int fetch_aircraft() {
    if (WiFi.status() != WL_CONNECTED) return -1;

    const char* api_urls[2] = {
        "https://api.adsb.lol/v2/point/%.4f/%.4f/%d",
        "https://opendata.adsb.fi/api/v2/point/%.4f/%.4f/%d"
    };

    String payload  = "";
    bool   got_data = false;

    for (int u = 0; u < 2 && !got_data; u++) {
        char url[128];
        snprintf(url, sizeof(url), api_urls[u],
                 HOME_LAT, HOME_LON, RADIUS_NM);
        Serial.printf("Trying: %s\n", url);

        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient http;
        http.begin(client, url);
        http.setTimeout(8000);
        int httpCode = http.GET();

        if (httpCode == 200) {
            payload  = http.getString();
            got_data = true;
            Serial.println("Fetch OK");
        } else {
            Serial.printf("HTTP %d, trying next...\n", httpCode);
        }
        http.end();
    }

    if (!got_data) return -1;

    // Filter to only fields we need (saves RAM)
    DynamicJsonDocument filter(200);
    JsonObject ac_filter = filter["ac"].createNestedObject();
    ac_filter["lat"]      = true;
    ac_filter["lon"]      = true;
    ac_filter["alt_baro"] = true;
    ac_filter["flight"]   = true;
    ac_filter["hex"]      = true;

    DynamicJsonDocument doc(24576);  // 24KB buffer
    DeserializationError err = deserializeJson(doc, payload,
        DeserializationOption::Filter(filter));

    if (err) {
        Serial.printf("JSON error: %s\n", err.c_str());
        return -1;
    }

    JsonArray aircraft = doc["ac"];
    ac_count = 0;

    for (JsonObject ac : aircraft) {
        if (ac_count >= MAX_AC) break;

        float ac_lat = ac["lat"] | -999.0f;
        float ac_lon = ac["lon"] | -999.0f;
        if (ac_lat == -999.0f || ac_lon == -999.0f) continue;

        float dist = calc_distance_nm(HOME_LAT, HOME_LON, ac_lat, ac_lon);
        if (dist > RADIUS_NM) continue;

        int alt_ft = 0;
        if (ac["alt_baro"].is<int>()) alt_ft = ac["alt_baro"].as<int>();

        const char* cs_raw = ac["flight"] | ac["hex"] | "?";
        memset(ac_list[ac_count].callsign, 0,
               sizeof(ac_list[0].callsign));
        strncpy(ac_list[ac_count].callsign, cs_raw,
                sizeof(ac_list[0].callsign) - 1);

        // Trim trailing whitespace from callsign
        for (int i = strlen(ac_list[ac_count].callsign)-1;
             i >= 0 && (ac_list[ac_count].callsign[i] == ' '  ||
                        ac_list[ac_count].callsign[i] == '\t' ||
                        ac_list[ac_count].callsign[i] == '\r' ||
                        ac_list[ac_count].callsign[i] == '\n'); i--)
            ac_list[ac_count].callsign[i] = '\0';

        ac_list[ac_count].alt_ft      = alt_ft;
        ac_list[ac_count].bearing_deg = calc_bearing(
            HOME_LAT, HOME_LON, ac_lat, ac_lon);
        polar_to_xy(ac_list[ac_count].bearing_deg, dist,
                    ac_list[ac_count].x, ac_list[ac_count].y);

        Serial.printf("  [%d] %s alt=%d brng=%.0f dist=%.1fnm\n",
            ac_count, ac_list[ac_count].callsign,
            alt_ft, ac_list[ac_count].bearing_deg, dist);

        ac_count++;
    }

    return 0;
}

// ============================================================
// SETTINGS: SAVE / LOAD / RESET (ESP32 flash via Preferences)
// ============================================================
void save_settings(float lat, float lon, int nm) {
    prefs.begin("radar", false);
    prefs.putFloat("lat", lat);
    prefs.putFloat("lon", lon);
    prefs.putInt("nm",  nm);
    prefs.putBool("configured", true);
    prefs.end();
    Serial.printf("Settings saved: lat=%.5f lon=%.5f nm=%d\n",
                  lat, lon, nm);
}

bool load_settings() {
    prefs.begin("radar", true);
    bool configured = prefs.getBool("configured", false);
    if (configured) {
        HOME_LAT  = prefs.getFloat("lat", HOME_LAT);
        HOME_LON  = prefs.getFloat("lon", HOME_LON);
        RADIUS_NM = prefs.getInt("nm",   RADIUS_NM);
    }
    prefs.end();
    Serial.printf("Settings loaded: lat=%.5f lon=%.5f nm=%d\n",
                  HOME_LAT, HOME_LON, RADIUS_NM);
    return configured;
}

void reset_settings() {
    prefs.begin("radar", false);
    prefs.clear();
    prefs.end();
    Serial.println("Settings cleared.");
}

// ============================================================
// WIFIMANAGER CAPTIVE PORTAL SETUP
// Starts config portal if not configured or reset requested.
// Custom parameters: latitude, longitude, radius.
// ============================================================
void run_wifi_setup() {
    show_message("Connect to WiFi:", "RadarSetup", PHOSPHOR[0]);

    char lat_str[20], lon_str[20], nm_str[8];
    snprintf(lat_str, sizeof(lat_str), "%.6f", HOME_LAT);
    snprintf(lon_str, sizeof(lon_str), "%.6f", HOME_LON);
    snprintf(nm_str,  sizeof(nm_str),  "%d",   RADIUS_NM);

    WiFiManagerParameter param_lat("lat", "Latitude",    lat_str, 20);
    WiFiManagerParameter param_lon("lon", "Longitude",   lon_str, 20);
    WiFiManagerParameter param_nm ("nm",  "Radius (nm)", nm_str,   4);

    WiFiManager wm;
    wm.addParameter(&param_lat);
    wm.addParameter(&param_lon);
    wm.addParameter(&param_nm);
    wm.setTitle("WW-II Radar Setup");
    wm.setConfigPortalTimeout(300);  // 5 minute timeout

    wm.setAPCallback([](WiFiManager *wm) {
        show_message("Open browser:", "192.168.4.1", PHOSPHOR[0]);
    });

    Serial.println("Starting config portal...");
    bool connected = wm.startConfigPortal("RadarSetup");

    if (connected) {
        float new_lat = atof(param_lat.getValue());
        float new_lon = atof(param_lon.getValue());
        int   new_nm  = atoi(param_nm.getValue());

        if (new_lat >= -90  && new_lat <= 90 &&
            new_lon >= -180 && new_lon <= 180 &&
            new_nm  >= 1    && new_nm  <= 100) {
            save_settings(new_lat, new_lon, new_nm);
            HOME_LAT  = new_lat;
            HOME_LON  = new_lon;
            RADIUS_NM = new_nm;
            show_message("Settings saved!", "Restarting...", PHOSPHOR[0]);
        } else {
            show_message("Invalid values!", "Using defaults", RED);
        }
    } else {
        show_message("Setup timeout", "Restarting...", RED);
    }
    delay(2000);
    ESP.restart();
}

// ============================================================
// SETUP
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\nWW-II Phosphor Radar starting...");

    if (!gfx->begin()) {
        Serial.println("Display init failed!");
        while (1) delay(100);
    }
    gfx->fillScreen(BLACK);

    // Check reset button -- hold for 3 seconds at boot to
    // clear settings and re-enter captive portal setup
    pinMode(BOOT_PIN, INPUT_PULLUP);
    show_message("Hold BOOT to", "reset settings", PHOSPHOR[2]);
    delay(3000);
    if (digitalRead(BOOT_PIN) == LOW) {
        Serial.println("Reset button held -- clearing settings!");
        reset_settings();
        show_message("Settings reset!", "Starting setup...", RED);
        delay(1500);
        run_wifi_setup();
        return;
    }

    // Load saved settings from flash
    bool configured = load_settings();

    if (!configured) {
        Serial.println("Not configured -- starting setup portal.");
        run_wifi_setup();
        return;
    }

    // Connect to saved WiFi using WiFiManager autoConnect
    show_message("Connecting to", "WiFi...", PHOSPHOR[0]);

    WiFiManager wm;
    wm.setConnectTimeout(20);
    wm.setConfigPortalTimeout(180);

    char lat_str[20], lon_str[20], nm_str[8];
    snprintf(lat_str, sizeof(lat_str), "%.6f", HOME_LAT);
    snprintf(lon_str, sizeof(lon_str), "%.6f", HOME_LON);
    snprintf(nm_str,  sizeof(nm_str),  "%d",   RADIUS_NM);

    WiFiManagerParameter param_lat("lat", "Latitude",    lat_str, 20);
    WiFiManagerParameter param_lon("lon", "Longitude",   lon_str, 20);
    WiFiManagerParameter param_nm ("nm",  "Radius (nm)", nm_str,   4);
    wm.addParameter(&param_lat);
    wm.addParameter(&param_lon);
    wm.addParameter(&param_nm);

    wm.setAPCallback([](WiFiManager *wm) {
        show_message("Connect to WiFi:", "RadarSetup", PHOSPHOR[0]);
    });

    bool connected = wm.autoConnect("RadarSetup");

    if (!connected) {
        show_message("WiFi failed!", "Restarting...", RED);
        delay(2000);
        ESP.restart();
        return;
    }

    // Save any updated parameters if portal was used
    float new_lat = atof(param_lat.getValue());
    float new_lon = atof(param_lon.getValue());
    int   new_nm  = atoi(param_nm.getValue());
    if (new_lat != 0 && new_lon != 0) {
        HOME_LAT  = new_lat;
        HOME_LON  = new_lon;
        RADIUS_NM = new_nm;
        save_settings(HOME_LAT, HOME_LON, RADIUS_NM);
    }

    Serial.printf("Connected! IP: %s\n",
        WiFi.localIP().toString().c_str());
    Serial.printf("Location: %.5f, %.5f  Radius: %dnm\n",
        HOME_LAT, HOME_LON, RADIUS_NM);

    show_message("WiFi Connected!", nullptr, PHOSPHOR[0]);
    delay(1000);

    draw_static_scene();
    draw_status(0, true);
}

// ============================================================
// MAIN LOOP
// No delay() needed -- cycle timing driven by:
//   fetch (~2-3s) + sweep (SWEEP_MS=2s) + fade (FADE_MS=8s)
// Total cycle: ~12-13 seconds
// ============================================================
void loop() {
    Serial.println("\n--- New cycle ---");

    int result = fetch_aircraft();
    if (result != 0) {
        Serial.println("Fetch failed, using last known data.");
    }

    radar_sweep();
    draw_status(ac_count, WiFi.status() == WL_CONNECTED);
    phosphor_fade();
}
