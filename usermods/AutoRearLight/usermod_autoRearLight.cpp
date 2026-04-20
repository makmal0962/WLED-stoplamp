/*
usermod_autoRearLight.cpp
Created by MikaTsuki
Find me on Facebook: MikaTsuki

Created: 2026-03-29
Last Updated: 2026-04-20

Useful flags in your platformio_override.ini:
-D AUTOREARL_DISABLE_FILE_PATTERNS
-D AUTOREARL_DISABLE_CONFIG

Compile with -D AUTOREARL_DISABLE_FILE_PATTERNS to skip all LittleFS pattern loading.
Use this flag when debugging bootloops or on RAM-constrained builds (e.g. ESP8266).
When defined, the usermod always uses the built-in PROGMEM arrow patterns.

Use -D AUTOREARL_DISABLE_CONFIG flag to disable usermod config
*/

#include "wled.h"
#include "Arduino.h"

#define USERMOD_ID_AUTOREARL 200

#ifdef AUTOREARL_DISABLE_FILE_PATTERNS
  #warning "AutoRearLight: File Pattern Loading is DISABLED. Using hardcoded patterns only."
#endif

#ifdef ARDUINO_ARCH_ESP8266
  #warning "AutoRearLight: You are using ESP8266! Don't DM me if your board (or YOU) crashes in the middle of the road!"
#endif

// Macro to get the column count (width) of a 2D array
#define ARRAY_W(arr) (sizeof(arr[0]) / sizeof(arr[0][0]))
// Macro to get the row count (height) of a 2D array
#define ARRAY_H(arr) (sizeof(arr)    / sizeof(arr[0]))

// ===== BUILT-IN PIXEL ART PATTERNS (PROGMEM) =====
// Fallback patterns used when no file is loaded from LittleFS.
// Each cell: 0 = off, 1 = on. Rows = Y, Columns = X.

const uint8_t PROGMEM arrowLeft[8][9] = {
  {0,0,0,1,1,0,0,1,1},
  {0,0,1,1,0,0,1,1,0},
  {0,1,1,0,0,1,1,0,0},
  {1,1,0,0,1,1,0,0,0},
  {1,1,0,0,1,1,0,0,0},
  {0,1,1,0,0,1,1,0,0},
  {0,0,1,1,0,0,1,1,0},
  {0,0,0,1,1,0,0,1,1},
};

const uint8_t PROGMEM arrowRight[8][9] = {
  {1,1,0,0,1,1,0,0,0},
  {0,0,1,0,0,1,1,0,0},
  {0,0,0,1,0,0,1,1,0},
  {0,0,0,0,1,0,0,1,1},
  {0,0,0,1,1,0,0,1,1},
  {0,0,1,1,0,0,1,1,0},
  {0,1,1,0,0,1,1,0,0},
  {1,1,0,0,1,1,0,0,0},
};

const uint8_t PROGMEM arrowHazard[8][10] = {
  {1,1,0,0,1,1,0,0,1,1},
  {1,1,0,0,1,1,0,0,1,1},
  {1,1,0,0,1,1,0,0,1,1},
  {1,1,0,0,1,1,0,0,1,1},
  {1,1,0,0,1,1,0,0,1,1},
  {0,0,0,0,0,0,0,0,0,0},
  {1,1,0,0,1,1,0,0,1,1},
  {1,1,0,0,1,1,0,0,1,1},
};


class AutoRearLightUsermod : public Usermod {
  private:

  // ===== STATE ENUMS =====

  // Main operating state, driven by the headlamp input.
  // Designed for motorcycle use. Extend here for future modes.
  enum State {
    IDLE, // Headlamp off — full brightness, idle preset
    TAIL, // Headlamp on  — brightness capped at 50%, tail preset
  };

  // Active turn signal classification.
  // Mutually exclusive — eliminates impossible bool combinations.
  enum SignalState {
    SIG_NONE,   // No signal active
    SIG_LEFT,   // Left turn signal committed
    SIG_RIGHT,  // Right turn signal committed
    SIG_HAZARD, // Both signals committed as hazard
  };

  // Wipe animation state.
  // Owned entirely by handleOverlayDraw(); loop() only sets intent flags.
  enum WipeState {
    WIPE_IDLE, // No animation running
    WIPE_IN,   // Pattern is revealing column by column
    WIPE_OUT,  // Pattern is hiding column by column
  };

  // ===== HARDWARE PIN CONFIG =====
  // Default pins per platform. All overridable via WLED config UI.
  #ifdef ARDUINO_ARCH_ESP32
    int8_t pinHeadlamp = 18;
    int8_t pinBrake    = 19;
    int8_t pinLeft     = 21;
    int8_t pinRight    = 22;
  #elif defined(ARDUINO_ARCH_ESP8266)
    int8_t pinHeadlamp = 14;
    int8_t pinBrake    = 5;
    int8_t pinLeft     = 12;
    int8_t pinRight    = 13;
  #endif

  // ===== PRESET IDs =====
  // WLED preset to apply on each state transition.
  uint8_t presetIdle = 1;
  uint8_t presetTail = 2;

  // ===== TIMING CONFIG =====
  uint16_t debounceMs     = 50;  // Input debounce window (ms)
  uint16_t tailReturnMs   = 500; // Delay before clearing signal state after signals go low (ms)
  uint16_t hazardDetectMs = 50;  // Window to detect simultaneous left+right as hazard (ms)
  uint16_t brakeFlashMs   = 25;  // Brake flash toggle interval (ms)
  uint16_t wipeSpeedMs    = 10;  // Wipe animation step interval (ms per column)

  // ===== MATRIX DIMENSIONS =====
  // Fetched dynamically every frame in handleOverlayDraw() to support
  // live segment resizing and prevent premature access during boot.
  uint16_t matrixWidth  = 0;
  uint16_t matrixHeight = 0;

  // ===== DEBOUNCE STATE =====
  // Each input pin has its own raw/debounced state and timestamp.
  bool debBrake      = false;
  bool rawBrakeDB    = false;
  unsigned long debBrakeTime = 0;

  bool debHeadlamp   = false;
  bool rawHeadlampDB = false;
  unsigned long debHeadlampTime = 0;

  // Turn signals are already clean from the relay, but debounce is kept for safety.
  bool debLeft    = false;
  bool rawLeftDB  = false;
  unsigned long debLeftTime = 0;

  bool debRight   = false;
  bool rawRightDB = false;
  unsigned long debRightTime = 0;

  // ===== SIGNAL TRACKING =====
  // Last committed (debounced) pin values, used by handleOverlayDraw().
  bool lastLeft  = false;
  bool lastRight = false;
  bool lastBrake = false;
  bool lastHead  = false;

  unsigned long signalFellTime  = 0; // Timestamp of the last falling edge on both turn signals
  unsigned long signalStartTime = 0; // Timestamp of rising edge, used for hazard detection window
  unsigned long lastBothTime    = 0; // Last time both left+right were HIGH simultaneously

  // True while waiting for hazardDetectMs to elapse after a rising edge.
  // Cleared once the signal type is committed to signalState.
  bool hazardWindowOpen = false;

  bool prevBlink = false; // Previous frame's blink state, for edge detection
  bool enabled   = true;

  // ===== ACTIVE STATES =====
  State       currentState    = IDLE;
  SignalState signalState     = SIG_NONE;
  SignalState prevSignalState = SIG_NONE; // For detecting signal change mid-animation
  WipeState   wipeState       = WIPE_IDLE;
  WipeState   prevWipeState   = WIPE_IDLE; // For detecting wipe-out transition in draw

  // ===== WIPE INTENT FLAGS =====
  // Set by loop(), consumed by handleOverlayDraw() which has access to patternW/1D size.
  bool requestWipeIn  = false;
  bool requestWipeOut = false;

  // ===== OVERLAY STATE =====
  bool          brakeFlashState  = false;
  unsigned long lastBrakeFlash   = 0;
  uint16_t      wipeColumn       = 0;     // Current wipe progress (columns for 2D, pixels for 1D)
  unsigned long lastWipeStep     = 0;
  bool          holdOnOff        = false; // If true, keep turn overlay visible during blink-off phase
  uint8_t       overlayBrightness = 255;
  uint8_t       wipeOutMode      = 1;    // 0 = reverse (shrink), 1 = forward (push out), 2 = hard blank
  unsigned long wipeStartTime    = 0;

  // ===== 1D CONFIG =====
  // Number of pixels used for turn signal and hazard display on a 1D strip.
  // Clamped to total strip length at draw time.
  uint16_t strip1DTurnLen   = 8; // Pixels lit per side for turn signal
  uint16_t strip1DHazardLen = 8; // Pixels lit for hazard (centered)

  // ===== OVERLAY COLORS =====
  uint8_t bgR = 40, bgG = 0, bgB = 0;               // Background (dark red default)
  uint8_t turnR   = 255, turnG   = 165, turnB   = 0; // Turn signal color (amber)
  uint8_t hazardR = 255, hazardG = 165, hazardB = 0; // Hazard color (amber)

  // ===== PATTERN FILE =====
  // Path to a LittleFS text file containing custom pixel art patterns.
  // Reloaded deferred via needLoadPatterns flag — never loaded during setup() or readFromConfig()
  // to avoid filesystem-not-ready crashes on ESP8266.
  // Compile with -D AUTOREARL_DISABLE_FILE_PATTERNS to disable all file loading entirely.
  char patternFile[32] = "/autoRearLight.txt";
  char currentFile[32] = "";
  bool needLoadPatterns = false; // Set by readFromConfig(); consumed safely in loop()

  // ===== DYNAMIC PATTERN BUFFERS =====
  // Loaded from LittleFS, or null if file loading is disabled/failed.
  // All pointers are null-checked before use — a null pointer always falls back to PROGMEM.
  // This prevents crashes from partial malloc failures on low-RAM devices.
  uint8_t* patternLeft   = nullptr; uint16_t patternLeftW   = 8,  patternLeftH   = 8;
  uint8_t* patternRight  = nullptr; uint16_t patternRightW  = 8,  patternRightH  = 8;
  uint8_t* patternHazard = nullptr; uint16_t patternHazardW = 16, patternHazardH = 8;
  bool     patternsLoaded = false;

  // ===== HELPERS =====

  bool readPin(int8_t pin) {
    return digitalRead(pin) == HIGH;
  }

  // Returns the Y offset to vertically center a pattern on the matrix.
  // Returns 0 if the pattern is taller than or equal to the matrix.
  int centerOffsetY(uint16_t patternH) {
    return (matrixHeight > patternH) ? (int)(matrixHeight - patternH) / 2 : 0;
  }

  // Returns the X offset to horizontally center a pattern on the matrix.
  // Returns 0 if the pattern is wider than or equal to the matrix.
  int centerOffsetX(uint16_t patternW) {
    return (matrixWidth > patternW) ? (int)(matrixWidth - patternW) / 2 : 0;
  }

  // ===== BRIGHTNESS CONTROL =====
  // Scales RGB values by overlayBrightness.
  // When in TAIL state, brightness is additionally capped at 128 (50%).
  void applyBrightness(uint8_t &r, uint8_t &g, uint8_t &b) {
    uint8_t eff = overlayBrightness;
    if (currentState == TAIL && eff > 128) eff = 128;
    if (eff == 255) return;
    r = (r * eff) / 255;
    g = (g * eff) / 255;
    b = (b * eff) / 255;
  }

  // ===== PATTERN MEMORY MANAGEMENT =====

  void freePatterns() {
    if (patternLeft)   { free(patternLeft);   patternLeft   = nullptr; }
    if (patternRight)  { free(patternRight);  patternRight  = nullptr; }
    if (patternHazard) { free(patternHazard); patternHazard = nullptr; }
    patternsLoaded = false;
  }

#ifndef AUTOREARL_DISABLE_FILE_PATTERNS
  // Parses a simple text file from LittleFS into pixel art pattern buffers.
  // File format:
  //   # left / # right / # hazard  → section header
  //   8x8                          → optional dimension hint (skipped)
  //   01100110                     → pixel row: '1' = on, anything else = off
  // Max 32 rows and 64 columns per pattern.
  //
  // Safe to call only after WLED has fully initialized (i.e. from loop(), not setup()).
  // If any malloc fails, all buffers are freed and patternsLoaded stays false.
  void loadPatterns() {
    freePatterns();

    File f = WLED_FS.open(patternFile, "r");
    if (!f) return;

    uint8_t** target  = nullptr;
    uint16_t* targetW = nullptr;
    uint16_t* targetH = nullptr;

    uint16_t detectedW = 0;
    uint16_t rowCount  = 0;

    // Temporary row buffer: max 32 rows × 64 columns = 2048 bytes
    // On ESP8266, this may fail if heap is fragmented — handled gracefully below.
    uint8_t (*tmp)[64] = (uint8_t(*)[64]) malloc(32 * 64);
    if (!tmp) { f.close(); return; } // malloc failed — silently fall back to PROGMEM

    // Flush accumulated rows into the target pattern buffer.
    // On malloc failure, frees all buffers so we never have a partial/corrupt state.
    auto commitPattern = [&]() {
      if (!target || rowCount == 0 || detectedW == 0) return;
      *targetW = detectedW;
      *targetH = rowCount;
      size_t sz = detectedW * rowCount;
      *target = (uint8_t*)malloc(sz);
      if (!*target) {
        // malloc failed for this pattern — free everything and abort
        free(tmp); tmp = nullptr;
        freePatterns();
        return;
      }
      for (uint16_t r = 0; r < rowCount; r++)
        memcpy(*target + r * detectedW, tmp[r], detectedW);
    };

    while (f.available()) {
      if (!tmp) break; // Aborted due to malloc failure

      String line = f.readStringUntil('\n');
      line.trim();
      if (line.length() == 0) continue;

      // Section header: commit previous pattern and select new target buffer
      if (line.startsWith("#")) {
        commitPattern();
        rowCount = 0; detectedW = 0;
        if      (line.indexOf("left")   >= 0) { target = &patternLeft;   targetW = &patternLeftW;   targetH = &patternLeftH;   }
        else if (line.indexOf("right")  >= 0) { target = &patternRight;  targetW = &patternRightW;  targetH = &patternRightH;  }
        else if (line.indexOf("hazard") >= 0) { target = &patternHazard; targetW = &patternHazardW; targetH = &patternHazardH; }
        else target = nullptr;
        continue;
      }

      // Skip optional dimension hint lines (e.g. "8x8", "16x8")
      if (line.indexOf('x') >= 0 && line.length() <= 6) continue;

      if (!target || rowCount >= 32) continue;
      uint16_t len = min((int)line.length(), 64);
      if (detectedW == 0) detectedW = len;
      for (uint16_t i = 0; i < detectedW; i++)
        tmp[rowCount][i] = (i < len && line[i] == '1') ? 1 : 0;
      rowCount++;
    }

    if (tmp) {
      commitPattern(); // Flush the last section
      free(tmp);
      patternsLoaded = (patternLeft != nullptr || patternRight != nullptr || patternHazard != nullptr);
    }
    f.close();
  }
#else
  // File pattern loading is disabled at compile time.
  // Always uses built-in PROGMEM patterns.
  void loadPatterns() {}
#endif

  // ===== 2D DRAW HELPERS =====
  // Both functions use row-major indexing: idx = y * patternW + x.
  // Bounds-checked against the matrix before writing to the strip.

  // Draws a single column of a pattern at a given pixel coordinate.
  void drawColumn(const uint8_t* pattern,
                  uint16_t patternW, uint16_t patternH,
                  int col, int offsetX, int offsetY,
                  uint8_t r, uint8_t g, uint8_t b,
                  bool isProgmem) {
    if (col < 0 || col >= (int)patternW) return;
    for (uint16_t y = 0; y < patternH; y++) {
      uint16_t idx = y * patternW + (uint16_t)col;
      uint8_t v = isProgmem ? pgm_read_byte(pattern + idx) : pattern[idx];
      if (v) {
        int xFinal = offsetX + col;
        int yFinal = offsetY + (int)y;
        if (xFinal >= 0 && xFinal < (int)matrixWidth && yFinal >= 0 && yFinal < (int)matrixHeight)
          strip.setPixelColorXY(xFinal, yFinal, RGBW32(r, g, b, 0));
      }
    }
  }

  // Draws all columns of a pattern (full frame, used for hazard blink).
  void drawFull(const uint8_t* pattern,
                uint16_t patternW, uint16_t patternH,
                int offsetX, int offsetY,
                uint8_t r, uint8_t g, uint8_t b,
                bool isProgmem) {
    for (uint16_t y = 0; y < patternH; y++) {
      for (uint16_t x = 0; x < patternW; x++) {
        uint16_t idx = y * patternW + x;
        uint8_t v = isProgmem ? pgm_read_byte(pattern + idx) : pattern[idx];
        if (v) {
          int xFinal = offsetX + (int)x;
          int yFinal = offsetY + (int)y;
          if (xFinal >= 0 && xFinal < (int)matrixWidth && yFinal >= 0 && yFinal < (int)matrixHeight)
            strip.setPixelColorXY(xFinal, yFinal, RGBW32(r, g, b, 0));
        }
      }
    }
  }

  public:
  AutoRearLightUsermod() {}

  void setup() override {
    signalFellTime = millis() - tailReturnMs; // Prevent false trigger on first loop

    pinMode(pinHeadlamp, INPUT);
    pinMode(pinBrake,    INPUT);
    pinMode(pinLeft,     INPUT);
    pinMode(pinRight,    INPUT);

    // Matrix dimensions are fetched dynamically in handleOverlayDraw().
    // Do NOT call strip.getMainSegment() here — WLED strip may not be ready yet.

    // Do NOT call loadPatterns() here — LittleFS is not mounted yet during setup().
    // Pattern loading is deferred to loop() via needLoadPatterns flag.
    // Initial load is triggered by readFromConfig() setting needLoadPatterns = true.
  }

  void loop() override {
#ifndef AUTOREARL_DISABLE_FILE_PATTERNS
    // Deferred pattern load: safe to call here because WLED and LittleFS are fully
    // initialized by the time loop() runs. Never load during setup() or readFromConfig().
    if (needLoadPatterns) {
      loadPatterns();
      needLoadPatterns = false;
    }
#endif

    if (!enabled) return;
    unsigned long now = millis();

    // ===== INPUT DEBOUNCE =====
    // Each pin is debounced independently using the same debounceMs window.

    bool rawHeadlampRead = readPin(pinHeadlamp);
    if (rawHeadlampRead != rawHeadlampDB) { rawHeadlampDB = rawHeadlampRead; debHeadlampTime = now; }
    if ((now - debHeadlampTime) >= debounceMs) debHeadlamp = rawHeadlampDB;
    bool head = debHeadlamp;

    bool rawBrakeRead = readPin(pinBrake);
    if (rawBrakeRead != rawBrakeDB) { rawBrakeDB = rawBrakeRead; debBrakeTime = now; }
    if ((now - debBrakeTime) >= debounceMs) debBrake = rawBrakeDB;
    bool brake = debBrake;

    bool rawLeftRead = readPin(pinLeft);
    if (rawLeftRead != rawLeftDB) { rawLeftDB = rawLeftRead; debLeftTime = now; }
    if ((now - debLeftTime) >= debounceMs) debLeft = rawLeftDB;
    bool left = debLeft;

    bool rawRightRead = readPin(pinRight);
    if (rawRightRead != rawRightDB) { rawRightDB = rawRightRead; debRightTime = now; }
    if ((now - debRightTime) >= debounceMs) debRight = rawRightDB;
    bool right = debRight;

    // ===== HAZARD DETECTION =====
    // On rising edge of either signal, open the detection window.
    // After hazardDetectMs elapses, classify and commit to signalState.
    if ((left || right) && !lastLeft && !lastRight) {
      signalStartTime  = now;
      hazardWindowOpen = true;
    }
    if (hazardWindowOpen && (now - signalStartTime >= hazardDetectMs)) {
      hazardWindowOpen = false;
      if      (left && right) signalState = SIG_HAZARD;
      else if (left)          signalState = SIG_LEFT;
      else if (right)         signalState = SIG_RIGHT;
    }

    // Hazard-off detection: track last time both signals were HIGH together.
    // Clear hazard state only after both go LOW for longer than the detect window.
    // This handles relay contacts that don't drop at exactly the same time.
    if (left && right) lastBothTime = now;
    if (signalState == SIG_HAZARD && !left && !right && (now - lastBothTime > hazardDetectMs)) {
      signalState      = SIG_NONE;
      hazardWindowOpen = false;
    }

    // ===== FALLING EDGE + RETURN TO IDLE =====
    // Record when both signals drop. After tailReturnMs, clear all signal state.
    if ((lastLeft || lastRight) && !(left || right)) signalFellTime = now;
    
    if (!left && !right && (now - signalFellTime >= tailReturnMs)) {
      signalState      = SIG_NONE;
      hazardWindowOpen = false;
    }

    // ===== STATE MACHINE =====
    // IDLE: headlamp off. TAIL: headlamp on.
    // Preset is applied once on each transition.
    // Guard: skip applyPreset during the first 3 seconds of boot to let WLED
    // finish initializing before we switch presets.
    State newState = head ? TAIL : IDLE;
    if (newState != currentState) {
      currentState = newState;
      if (millis() > 3000) {
        applyPreset(head ? presetTail : presetIdle);
      }
    }

    // ===== WIPE INTENT =====
    // loop() only sets intent flags. handleOverlayDraw() consumes them
    // with access to the actual pattern size (patternW / 1D pixel count).
    bool blinkNow  = (left || right);
    bool blinkRise = blinkNow && !prevBlink;
    bool blinkFall = !blinkNow && prevBlink;

    if (blinkRise)                requestWipeIn  = true;
    if (blinkFall && !holdOnOff) requestWipeOut = true;

    prevBlink = blinkNow;

    lastLeft  = left;
    lastRight = right;
    lastBrake = brake;
    lastHead  = head;
  }

  // ===== OVERLAY DRAW =====
  // Called every frame by WLED after the base effect is rendered.
  // Supports both 1D (linear strip) and 2D (matrix) configurations.
  void handleOverlayDraw() override {
    if (strip.getLengthTotal() == 0 ) return;
    // Fetch matrix dimensions dynamically every frame.
    // This guarantees safety across live segment resizing and avoids
    // calling strip methods before WLED is fully initialized.
    matrixWidth  = strip.getMainSegment().virtualWidth();
    matrixHeight = strip.getMainSegment().virtualHeight();

    if (!enabled || matrixWidth <= 1) return;

    // Global brightness cap: clamp to 50% (128/255) while in TAIL state.
    // This prevents headlights from washing out the overlay.
    if (currentState == TAIL && bri > 128) bri = 128;

    bool is1D         = (matrixHeight <= 1);
    unsigned long now = millis();
    bool brake        = lastBrake;
    bool signalActive = (lastLeft || lastRight); // Raw blink state for hazard hard-blink
    bool anySignal    = (signalState != SIG_NONE);

    // ===== BACKGROUND FILL =====
    // Paint the entire strip with the background color when any overlay is active.
    // This ensures the base WLED effect is fully covered by the overlay.
    uint8_t br = bgR, bgg = bgG, bb = bgB;
    applyBrightness(br, bgg, bb);
    if (brake || anySignal) {
      for (uint16_t i = 0; i < strip.getLengthTotal(); i++)
        strip.setPixelColor(i, RGBW32(br, bgg, bb, 0));
    }

    // ===== BRAKE FLASH =====
    // Full-strip red flash at brakeFlashMs interval.
    // During flash-off phase, strip goes fully black (overrides background).
    // brakeFlashState is reset when brake is released so the next press
    // always starts on the bright phase.
    if (brake) {
      if (now - lastBrakeFlash >= brakeFlashMs) {
        brakeFlashState = !brakeFlashState;
        lastBrakeFlash  = now;
      }
      if (brakeFlashState) {
        uint8_t r = 255, g = 0, b = 0;
        applyBrightness(r, g, b);
        for (uint16_t i = 0; i < strip.getLengthTotal(); i++)
          strip.setPixelColor(i, RGBW32(r, g, b, 0));
      } else {
        for (uint16_t i = 0; i < strip.getLengthTotal(); i++)
          strip.setPixelColor(i, RGBW32(0, 0, 0, 0));
      }
    } else {
      brakeFlashState = false; // Reset so next brake press starts bright
    }

    // ===== 1D PATH =====
    // Linear strip behavior — no matrix coordinate system.
    // Turn wipe uses strip1DTurnLen pixels from the outer edge inward.
    // Hazard uses strip1DHazardLen pixels centered on the strip.
    // Both use the same wipeState/wipeColumn system as 2D.
    if (is1D) {
      uint16_t total    = strip.getLengthTotal();
      uint16_t turnLen  = min((uint16_t)strip1DTurnLen,   total);
      uint16_t hazLen   = min((uint16_t)strip1DHazardLen, total);

      if (brake && brakeFlashState) return; // Skip turn overlay during brake flash-on phase

      if (anySignal) {
        uint8_t r = (signalState == SIG_HAZARD) ? hazardR : turnR;
        uint8_t g = (signalState == SIG_HAZARD) ? hazardG : turnG;
        uint8_t b = (signalState == SIG_HAZARD) ? hazardB : turnB;
        applyBrightness(r, g, b);

        if (signalState == SIG_HAZARD) {
          // Hazard: hard blink driven by raw signal, centered strip1DHazardLen pixels
          if (signalActive) {
            uint16_t start = (total - hazLen) / 2;
            for (uint16_t i = start; i < start + hazLen; i++)
              strip.setPixelColor(i, RGBW32(r, g, b, 0));
          }
        } else {
          // Turn: wipe animation over strip1DTurnLen pixels from the outer edge inward.
          // wipeColumn is in pixel units here (virtual patternW = turnLen).

          // Signal change → reset wipe
          if (signalState != prevSignalState) {
            wipeColumn      = 0;
            wipeState       = WIPE_IN;
            requestWipeIn   = false;
            requestWipeOut  = false;
          }

          // Consume wipe intent from loop()
          if (requestWipeIn) {
            wipeColumn    = 0;
            wipeState     = WIPE_IN;
            wipeStartTime = now; // start 1 frame delay
            requestWipeIn = false;
          }
          if (requestWipeOut) {
            wipeState      = WIPE_OUT;
            wipeColumn     = (wipeOutMode == 0) ? turnLen : 0;
            requestWipeOut = false;
          }

          // Advance wipe animation
          if (wipeState == WIPE_IN && now == wipeStartTime) {
          } // delay 1 frame before wipe in
          else if (now - lastWipeStep >= wipeSpeedMs) {
            lastWipeStep = now;
            switch (wipeState) {
              case WIPE_IN:
                if (wipeColumn < turnLen) wipeColumn++;
                else wipeState = WIPE_IDLE;
                break;
              case WIPE_OUT:
                if (wipeOutMode == 0) {
                  if (wipeColumn > 0) wipeColumn--;
                  else wipeState = WIPE_IDLE;
                } else if (wipeOutMode == 1) {
                  if (wipeColumn < turnLen) wipeColumn++;
                  else wipeState = WIPE_IDLE;
                } else { // mode 2: hard blank
                  wipeColumn = turnLen;
                  wipeState  = WIPE_IDLE;
                }
                break;
              case WIPE_IDLE: break;
            }
          }
          if (wipeColumn > turnLen) wipeColumn = turnLen;

          // Draw range (same logic as 2D)
          uint16_t drawStart = 0;
          uint16_t drawEnd   = wipeColumn;
          if (wipeState == WIPE_OUT) {
            if (wipeOutMode == 1) { drawStart = wipeColumn; drawEnd = turnLen; }
            else if (wipeOutMode == 2) { drawStart = drawEnd = 0; }
          }

          // Draw pixels from outer edge inward
          for (uint16_t i = drawStart; i < drawEnd; i++) {
            int x = (signalState == SIG_LEFT) ? (int)(total - 1 - i) : (int)i;
            strip.setPixelColor(x, RGBW32(r, g, b, 0));
          }
        }
      }

      prevSignalState = signalState;
      return;
    } // end 1D path

    // ===== 2D PATH =====
    if (!anySignal) {
      prevSignalState = signalState;
      return;
    }

    uint8_t r = (signalState == SIG_HAZARD) ? hazardR : turnR;
    uint8_t g = (signalState == SIG_HAZARD) ? hazardG : turnG;
    uint8_t b = (signalState == SIG_HAZARD) ? hazardB : turnB;
    applyBrightness(r, g, b);

    const uint8_t* pattern;
    uint16_t patternW, patternH;
    int offsetX;
    bool isProgmem = false;

    // ===== PATTERN SELECTION =====
    // Use file-loaded pattern if available AND pointer is non-null.
    // Null check is critical: malloc may have failed silently on low-RAM devices.
    // A null pattern pointer always falls back to PROGMEM.
    int offsetY;
    switch (signalState) {
      case SIG_HAZARD:
        if (patternsLoaded && patternHazard) {
          pattern = patternHazard; patternW = patternHazardW; patternH = patternHazardH;
          isProgmem = false;
        } else {
          pattern = &arrowHazard[0][0]; patternW = ARRAY_W(arrowHazard); patternH = ARRAY_H(arrowHazard);
          isProgmem = true;
        }
        // offsetX = centerOffsetX(patternW); // Dead center horizontally, safe against underflow
        // Exclusive offset for hazard
        // must be dead center!
        offsetX = (matrixWidth - patternW) / 2;
        offsetY = (matrixHeight - patternH) / 2;

        break;

      case SIG_LEFT:
        if (patternsLoaded && patternLeft) {
          pattern = patternLeft; patternW = patternLeftW; patternH = patternLeftH;
          isProgmem = false;
        } else {
          pattern = &arrowLeft[0][0]; patternW = ARRAY_W(arrowLeft); patternH = ARRAY_H(arrowLeft);
          isProgmem = true;
        }
        offsetX = 0; // Left-aligned
        offsetY = centerOffsetY(patternH);

        break;

      case SIG_RIGHT:
        if (patternsLoaded && patternRight) {
          pattern = patternRight; patternW = patternRightW; patternH = patternRightH;
          isProgmem = false;
        } else {
          pattern = &arrowRight[0][0]; patternW = ARRAY_W(arrowRight); patternH = ARRAY_H(arrowRight);
          isProgmem = true;
        }
        offsetX = (int)matrixWidth - (int)patternW; // Right-aligned
        offsetY = centerOffsetY(patternH);

        break;

      default:
        prevSignalState = signalState;
        return; // SIG_NONE — should not reach here
    }


    // ===== WIPE REQUEST HANDLING =====
    // All wipe init happens here — patternW is now known.

    // Signal change mid-animation → reset wipe to avoid glitch
    if (signalState != prevSignalState) {
      wipeColumn     = 0;
      wipeState      = WIPE_IN;
      requestWipeIn  = false;
      requestWipeOut = false;
    }

    // Consume wipe intent from loop()
    if (requestWipeIn) {
      wipeColumn    = 0;
      wipeState     = WIPE_IN;
      requestWipeIn = false;
    }
    if (requestWipeOut) {
      wipeState      = WIPE_OUT;
      wipeColumn     = (wipeOutMode == 0) ? patternW : 0;
      requestWipeOut = false;
    }

    prevSignalState = signalState;

    // ===== HAZARD: FULL FRAME, NO WIPE =====
    // Hazard blink is driven by the raw signal (hard blink), not the wipe system.
    if (signalState == SIG_HAZARD) {
      if (signalActive) drawFull(pattern, patternW, patternH, offsetX, offsetY, r, g, b, isProgmem);
      return;
    }

    // ===== WIPE ANIMATION =====
    // wipeColumn: 0 = fully hidden, patternW = fully visible.
    //
    // WIPE_IN:  wipeColumn increments until patternW
    // WIPE_OUT modes:
    //   0 = reverse (shrink inward): wipeColumn decrements to 0
    //   1 = forward (push out):      wipeColumn increments to patternW, draw window shifts
    //   2 = hard blank:              immediately completes, draws nothing
    if (wipeState == WIPE_IN && now == wipeStartTime) {
    } // delay 1 frame before wipe in
    else if (now - lastWipeStep >= wipeSpeedMs) {
      lastWipeStep = now;
      switch (wipeState) {
        case WIPE_IN:
          if (wipeColumn < patternW) wipeColumn++;
          else wipeState = WIPE_IDLE;
          break;
        case WIPE_OUT:
          if (wipeOutMode == 0) {
            if (wipeColumn > 0) wipeColumn--;
            else wipeState = WIPE_IDLE;
          } else if (wipeOutMode == 1) {
            if (wipeColumn < patternW) wipeColumn++;
            else wipeState = WIPE_IDLE;
          } else { // mode 2: hard blank
            wipeColumn = patternW;
            wipeState  = WIPE_IDLE;
          }
          break;
        case WIPE_IDLE: break;
      }
    }
    if (wipeColumn > patternW) wipeColumn = patternW;

    // ===== DRAW RANGE CALCULATION =====
    // drawStart..drawEnd defines which columns of the pattern to draw this frame.
    uint16_t drawStart = 0;
    uint16_t drawEnd   = wipeColumn;

    if (wipeState == WIPE_OUT) {
      if (wipeOutMode == 1) {
        // Forward push: remaining visible window shifts right as wipeColumn advances
        drawStart = wipeColumn;
        drawEnd   = patternW;
      } else if (wipeOutMode == 2) {
        drawStart = drawEnd = 0; // Draw nothing
      }
      // mode 0: drawEnd = wipeColumn (shrinking range), drawStart stays 0
    }

    // ===== DRAW WIPE =====
    // Left turn: wipe outward from the inner edge (columns drawn right→left).
    // Right turn: wipe outward from the inner edge (columns drawn left→right).
    for (uint16_t i = drawStart; i < drawEnd; i++) {
      int col = (signalState == SIG_LEFT) ? ((int)patternW - 1) - (int)i : (int)i;
      drawColumn(pattern, patternW, patternH, col, offsetX, offsetY, r, g, b, isProgmem);
    }
  }

  void connected() override {}

  uint16_t getId() override { return USERMOD_ID_AUTOREARL; }

  // ===== CONFIG SERIALIZATION =====
#ifndef AUTOREARL_DISABLE_CONFIG
  void addToConfig(JsonObject& root) override {
    JsonObject top = root.createNestedObject("AutoRearLight");
    top["Enable Usermod"] = enabled;

    JsonObject overlay = top.createNestedObject("Overlay Settings");
    overlay["Overlay Brightness"]                                           = overlayBrightness;
    overlay["Keep Turns Pattern When Signals Falling"] = holdOnOff;
    overlay["Exit Style, 0: Rev, 1: Fwd, 2: OFF"]                         = wipeOutMode;
    overlay["Bg Red"]   = bgR;
    overlay["Bg Green"] = bgG;
    overlay["Bg Blue"]  = bgB;
    overlay["Turn Red"]         = turnR;
    overlay["Turn Green"]       = turnG;
    overlay["Turn Blue"]        = turnB;
    overlay["Hazard Red"]       = hazardR;
    overlay["Hazard Green"]     = hazardG;
    overlay["Hazard Blue"]      = hazardB;

    top["Pattern File"] = patternFile;

    JsonObject strip1D = top.createNestedObject("1D Strip Settings");
    strip1D["Turn Signal Length (px)"] = strip1DTurnLen;
    strip1D["Hazard Length (px)"]      = strip1DHazardLen;

    JsonObject pins = top.createNestedObject("Hardware Input Pins");
    pins["Headlamp"]   = pinHeadlamp;
    pins["Brake"]      = pinBrake;
    pins["Left Turn"]  = pinLeft;
    pins["Right Turn"] = pinRight;

    JsonObject presets = top.createNestedObject("Preset IDs");
    presets["Idle"] = presetIdle;
    presets["Tail"] = presetTail;

    JsonObject timing = top.createNestedObject("Timing");
    timing["Pins Debounce (ms)"]       = debounceMs;
    timing["Signal Return Delay (ms)"] = tailReturnMs;
    timing["Hazard Detection (ms)"]    = hazardDetectMs;
    timing["Brake Flash Speed (ms)"]   = brakeFlashMs;
    timing["Wipe Speed (ms)"]          = wipeSpeedMs;
  }

  bool readFromConfig(JsonObject& root) override {
    JsonObject top = root["AutoRearLight"];
    bool configComplete = !top.isNull();

    configComplete &= getJsonValue(top["Enable Usermod"], enabled, true);

    JsonObject overlay = top["Overlay Settings"];
    configComplete &= getJsonValue(overlay["Overlay Brightness"],                                           overlayBrightness, 255);
    configComplete &= getJsonValue(overlay["Keep Turns Pattern When Signals Falling"], holdOnOff,         false);
    configComplete &= getJsonValue(overlay["Exit Style, 0: Rev, 1: Fwd, 2: OFF"],                         wipeOutMode,       1);
    configComplete &= getJsonValue(overlay["Bg Red"],   bgR,    40);
    configComplete &= getJsonValue(overlay["Bg Green"], bgG,    0);
    configComplete &= getJsonValue(overlay["Bg Blue"],  bgB,    0);
    configComplete &= getJsonValue(overlay["Turn Red"],         turnR,   255);
    configComplete &= getJsonValue(overlay["Turn Green"],       turnG,   165);
    configComplete &= getJsonValue(overlay["Turn Blue"],        turnB,   0);
    configComplete &= getJsonValue(overlay["Hazard Red"],       hazardR, 255);
    configComplete &= getJsonValue(overlay["Hazard Green"],     hazardG, 165);
    configComplete &= getJsonValue(overlay["Hazard Blue"],      hazardB, 0);

    // Pattern file: compare with currentFile to detect changes.
    // Do NOT call loadPatterns() here — LittleFS may not be ready.
    // Set needLoadPatterns = true instead; loop() will handle it safely.
    {
      const char* tmp_pf = top["Pattern File"] | "/autoRearLight.txt";
      if (strcmp(tmp_pf, currentFile) != 0) {
        strncpy(patternFile, tmp_pf, sizeof(patternFile) - 1);
        patternFile[sizeof(patternFile) - 1] = '\0';
        strncpy(currentFile, patternFile, sizeof(currentFile) - 1);
        currentFile[sizeof(currentFile) - 1] = '\0';
        needLoadPatterns = true; // Deferred — consumed in loop()
      }
    }

    JsonObject strip1D = top["1D Strip Settings"];
    configComplete &= getJsonValue(strip1D["Turn Signal Length (px)"], strip1DTurnLen,   8);
    configComplete &= getJsonValue(strip1D["Hazard Length (px)"],      strip1DHazardLen, 8);

    JsonObject pins = top["Hardware Input Pins"];
    #ifdef ARDUINO_ARCH_ESP32
    configComplete &= getJsonValue(pins["Headlamp"],   pinHeadlamp, 18);
    configComplete &= getJsonValue(pins["Brake"],      pinBrake,    19);
    configComplete &= getJsonValue(pins["Left Turn"],  pinLeft,     21);
    configComplete &= getJsonValue(pins["Right Turn"], pinRight,    22);
    #elif defined(ARDUINO_ARCH_ESP8266)
    configComplete &= getJsonValue(pins["Headlamp"],   pinHeadlamp, 14);
    configComplete &= getJsonValue(pins["Brake"],      pinBrake,    5);
    configComplete &= getJsonValue(pins["Left Turn"],  pinLeft,     12);
    configComplete &= getJsonValue(pins["Right Turn"], pinRight,    13);
    #endif

    JsonObject presets = top["Preset IDs"];
    configComplete &= getJsonValue(presets["Idle"], presetIdle, 1);
    configComplete &= getJsonValue(presets["Tail"], presetTail, 2);

    JsonObject timing = top["Timing"];
    configComplete &= getJsonValue(timing["Pins Debounce (ms)"],       debounceMs,     50);
    configComplete &= getJsonValue(timing["Signal Return Delay (ms)"], tailReturnMs,   500);
    configComplete &= getJsonValue(timing["Hazard Detection (ms)"],    hazardDetectMs, 50);
    configComplete &= getJsonValue(timing["Brake Flash Speed (ms)"],   brakeFlashMs,   25);
    configComplete &= getJsonValue(timing["Wipe Speed (ms)"],          wipeSpeedMs,    10);

    if (configComplete) {
      pinMode(pinHeadlamp, INPUT);
      pinMode(pinBrake,    INPUT);
      pinMode(pinLeft,     INPUT);
      pinMode(pinRight,    INPUT);
    }

    return configComplete;
  }
#else
  void addToConfig(JsonObject& root) override {}
  bool readFromConfig(JsonObject& root) override {}   
  #warning "AutoRearLight: Usermod config is DISABLED. Using hardcoded settings."
#endif // AUTOREARL_DISABLE_CONFIG

  // ===== INFO PANEL =====
  // Displayed in the WLED info tab for live debugging.
  void addToJsonInfo(JsonObject& root) override {
    JsonObject user = root["u"];
    if (user.isNull()) user = root.createNestedObject("u");

    const char* stateNames[]  = { "Idle", "Tail" };
    const char* signalNames[] = { "None", "Left", "Right", "Hazard" };
    const char* wipeNames[]   = { "Idle", "In", "Out" };

    JsonArray stateArr = user.createNestedArray("State");
    stateArr.add(stateNames[currentState]);
    stateArr.add(lastHead  ? "HEAD"  : "-");
    stateArr.add(lastBrake ? "BRAKE" : "-");
    stateArr.add(signalNames[signalState]);

    JsonArray wipeArr = user.createNestedArray("Wipe");
    wipeArr.add(wipeColumn);
    wipeArr.add(wipeNames[wipeState]);

    JsonArray matArr = user.createNestedArray("Matrix");
    matArr.add(matrixWidth);
    matArr.add(matrixHeight);

    JsonArray patArr = user.createNestedArray("Patterns");
#ifdef AUTOREARL_DISABLE_FILE_PATTERNS
    patArr.add("disabled (AUTOREARL_DISABLE_FILE_PATTERNS)");
#else
    patArr.add(patternsLoaded ? patternFile : "defaults");
#endif // AUTOREARL_DISABLE_FILE_PATTERNS
  }
};

static AutoRearLightUsermod autoRearLight;
REGISTER_USERMOD(autoRearLight);