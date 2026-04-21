/*
usermod_autoRearLight.cpp
Created by MikaTsuki
Find me on Facebook: MikaTsuki

Created: 2026-03-29
Last Updated: 2026-04-21

Useful flags in your platformio_override.ini:
-D AUTOREARL_DISABLE_FILE_PATTERNS (useful if your board RAM is very tight)
-D AUTOREARL_DISABLE_CONFIG (useful if your board is bootlooping)

Known issues:
- Do not use "Transpose" in segment settings. it rotates the matrix width and hegiht value in 
  dimension call which this mod reads them constantly during overlay draw, causing patterns draw in 
  wrong place (truncated in bottom left matrix).
*/

#include "wled.h"
#include "Arduino.h"

#define USERMOD_ID_AUTOREARL 200

#ifdef AUTOREARL_DISABLE_FILE_PATTERNS
  #warning "AutoRearLight: File Pattern Loading is DISABLED. Using hardcoded patterns only."
#endif

#define ARRAY_W(arr) (sizeof(arr[0]) / sizeof(arr[0][0])) // column count of 2D array
#define ARRAY_H(arr) (sizeof(arr)    / sizeof(arr[0]))    // row count of 2D array

// ===== BUILT-IN PATTERNS (PROGMEM) =====
// Fallback when no LittleFS file is loaded. 0 = off, 1 = on. [row][col]

const uint8_t PROGMEM arrowLeft[9][10] = {
  {0,0,0,0,1,1,0,0,1,1},
  {0,0,0,1,1,0,0,1,1,0},
  {0,0,1,1,0,0,1,1,0,0},
  {0,1,1,0,0,1,1,0,0,0},
  {1,1,0,0,1,1,0,0,0,0},
  {0,1,1,0,0,1,1,0,0,0},
  {0,0,1,1,0,0,1,1,0,0},
  {0,0,0,1,1,0,0,1,1,0},
  {0,0,0,0,1,1,0,0,1,1},
};

const uint8_t PROGMEM arrowRight[9][10] = {
  {1,1,0,0,1,1,0,0,0,0},
  {0,1,1,0,0,1,1,0,0,0},
  {0,0,1,1,0,0,1,1,0,0},
  {0,0,0,1,1,0,0,1,1,0},
  {0,0,0,0,1,1,0,0,1,1},
  {0,0,0,1,1,0,0,1,1,0},
  {0,0,1,1,0,0,1,1,0,0},
  {0,1,1,0,0,1,1,0,0,0},
  {1,1,0,0,1,1,0,0,0,0},
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

  enum State {
    IDLE, // headlamp off — full brightness
    TAIL, // headlamp on  — capped at 50%
  };

  // Mutually exclusive — eliminates impossible bool combos.
  enum SignalState {
    SIG_NONE,
    SIG_LEFT,
    SIG_RIGHT,
    SIG_HAZARD,
  };

  // Owned by handleOverlayDraw(); loop() only sets intent flags.
  enum WipeState {
    WIPE_IDLE,
    WIPE_IN,
    WIPE_OUT,
  };
  
  uint16_t wledReadyDelay = 3000; // Additional delay to make sure WLED is ready

  // ===== HARDWARE PIN CONFIG =====
  // Overridable via WLED config UI.
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
  uint8_t presetIdle = 1;
  uint8_t presetTail = 2;

  // ===== TIMING CONFIG =====
  uint16_t debounceMs     = 50;  // ms
  uint16_t tailReturnMs   = 500; // delay after signals fall before clearing state
  uint16_t hazardDetectMs = 50;  // window to detect simultaneous left+right
  uint16_t brakeFlashMs   = 25;  // ms per flash toggle
  uint16_t wipeSpeedMs    = 10;  // ms per column step

  // ===== MATRIX DIMENSIONS =====
  // Fetched every frame in handleOverlayDraw() — safe against live resizing and boot ordering.
  uint16_t matrixWidth  = 0;
  uint16_t matrixHeight = 0;
  bool dimReady = false;

  // ===== DEBOUNCE STATE =====
  bool debBrake      = false;
  bool rawBrakeDB    = false;
  unsigned long debBrakeTime = 0;

  bool debHeadlamp   = false;
  bool rawHeadlampDB = false;
  unsigned long debHeadlampTime = 0;

  // Relay output is clean, but debounced for safety.
  bool debLeft    = false;
  bool rawLeftDB  = false;
  unsigned long debLeftTime = 0;

  bool debRight   = false;
  bool rawRightDB = false;
  unsigned long debRightTime = 0;

  // ===== SIGNAL TRACKING =====
  bool lastLeft  = false;
  bool lastRight = false;
  bool lastBrake = false;
  bool lastHead  = false;

  unsigned long signalFellTime  = 0; // falling edge timestamp (both signals)
  unsigned long signalStartTime = 0; // rising edge timestamp (hazard window)

  bool hazardWindowOpen = false; // waiting for hazardDetectMs to classify signal

  bool prevBlink = false; // previous blink state for edge detection
  bool enabled   = true;

  // ===== ACTIVE STATES =====
  State       currentState    = IDLE;
  SignalState signalState     = SIG_NONE;
  SignalState prevSignalState = SIG_NONE; // signal change detection
  WipeState   wipeState       = WIPE_IDLE;
  WipeState   prevWipeState   = WIPE_IDLE;

  // ===== WIPE INTENT FLAGS =====
  // Set by loop(), consumed by handleOverlayDraw() which knows patternW/turnLen.
  bool requestWipeIn  = false;
  bool requestWipeOut = false;

  // ===== OVERLAY STATE =====
  bool          brakeFlashState  = false;
  unsigned long lastBrakeFlash   = 0;
  uint16_t      wipeColumn       = 0; // 0 = blank sentinel, patternW = fully shown
  unsigned long lastWipeStep     = 0;
  bool          holdOnOff        = false; // keep overlay visible during blink-off phase
  uint8_t       overlayBrightness = 255;
  uint8_t       wipeOutMode      = 1; // 0 = reverse, 1 = forward push, 2 = hard blank
  unsigned long wipeStartTime    = 0;

  // ===== 1D CONFIG =====
  // Clamped to total strip length at draw time.
  uint16_t strip1DTurnLen   = 8; // px per side
  uint16_t strip1DHazardLen = 8; // px centered

  // ===== OVERLAY COLORS =====
  uint8_t bgR = 40, bgG = 0, bgB = 0;               // Background (dark red default)
  uint8_t turnR   = 255, turnG   = 165, turnB   = 0; // Turn signal color (amber)
  uint8_t hazardR = 255, hazardG = 165, hazardB = 0; // Hazard color (amber)

  // ===== PATTERN FILE =====
  // Loaded deferred via loop() — never during setup()/readFromConfig() (LittleFS not ready).
  char patternFile[32] = "/autoRearLight.txt";
  char currentFile[32] = "";
  bool needLoadPatterns = false; // set by readFromConfig(), consumed in loop()

  // ===== DYNAMIC PATTERN BUFFERS =====
  // null = load failed or disabled; always falls back to PROGMEM.
  uint8_t* patternLeft   = nullptr; uint16_t patternLeftW   = 10,  patternLeftH   = 9;
  uint8_t* patternRight  = nullptr; uint16_t patternRightW  = 10,  patternRightH  = 9;
  uint8_t* patternHazard = nullptr; uint16_t patternHazardW = 10, patternHazardH = 8;
  bool     patternsLoaded = false;

  // ===== HELPERS =====

  bool readPin(int8_t pin) {
    return digitalRead(pin) == HIGH;
  }

  // Returns 0 if pattern >= matrix dimension.
  int centerOffsetY(uint16_t patternH) {
    return (matrixHeight > patternH) ? (int)(matrixHeight - patternH) / 2 : 0;
  }

  int centerOffsetX(uint16_t patternW) {
    return (matrixWidth > patternW) ? (int)(matrixWidth - patternW) / 2 : 0;
  }

  // ===== BRIGHTNESS CONTROL =====
  // Caps at 50% in TAIL state.
  void applyBrightness(uint8_t &r, uint8_t &g, uint8_t &b) {
    uint8_t eff = overlayBrightness;
    if (currentState == TAIL && eff > 128) eff = 128; // above this is too bright at night
    if (eff == 255) return;
    if (strip.getBrightness() > eff) strip.setBrightness(eff, false); // cap preset brightness too with this
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
  // File format: # left/right/hazard -> section header, 8x8 -> skipped, 01100110 -> pixel row.
  // Max 32 rows × 64 cols per pattern. Call only from loop() — LittleFS must be ready.
  // On any malloc failure, all buffers freed and patternsLoaded stays false.
  void loadPatterns() {
    freePatterns();

    File f = WLED_FS.open(patternFile, "r");
    if (!f) return;

    uint8_t** target  = nullptr;
    uint16_t* targetW = nullptr;
    uint16_t* targetH = nullptr;

    uint16_t detectedW = 0;
    uint16_t rowCount  = 0;

    // 32 rows × 64 cols temp buffer; may fail on fragmented ESP8266 heap.
    uint8_t (*tmp)[64] = (uint8_t(*)[64]) malloc(32 * 64);
    if (!tmp) { f.close(); return; } // fall back to PROGMEM silently

    // Flush accumulated rows into target buffer; abort all on malloc failure.
    auto commitPattern = [&]() {
      if (!target || rowCount == 0 || detectedW == 0) return;
      *targetW = detectedW;
      *targetH = rowCount;
      size_t sz = detectedW * rowCount;
      *target = (uint8_t*)malloc(sz);
      if (!*target) {
        free(tmp); tmp = nullptr; // abort — never leave partial state
        freePatterns();
        return;
      }
      for (uint16_t r = 0; r < rowCount; r++)
        memcpy(*target + r * detectedW, tmp[r], detectedW);
    };

    while (f.available()) {
      if (!tmp) break; // malloc failed upstream

      String line = f.readStringUntil('\n');
      line.trim();
      if (line.length() == 0) continue;

      // Section header — commit previous and switch target
      if (line.startsWith("#")) {
        commitPattern();
        rowCount = 0; detectedW = 0;
        if      (line.indexOf("left")   >= 0) { target = &patternLeft;   targetW = &patternLeftW;   targetH = &patternLeftH;   }
        else if (line.indexOf("right")  >= 0) { target = &patternRight;  targetW = &patternRightW;  targetH = &patternRightH;  }
        else if (line.indexOf("hazard") >= 0) { target = &patternHazard; targetW = &patternHazardW; targetH = &patternHazardH; }
        else target = nullptr;
        continue;
      }

      // Skip dimension hint lines e.g. "8x8"
      if (line.indexOf('x') >= 0 && line.length() <= 6) continue;

      if (!target || rowCount >= 32) continue;
      uint16_t len = min((int)line.length(), 64);
      if (detectedW == 0) detectedW = len;
      for (uint16_t i = 0; i < detectedW; i++)
        tmp[rowCount][i] = (i < len && line[i] == '1') ? 1 : 0;
      rowCount++;
    }

    if (tmp) {
      commitPattern(); // flush last section
      free(tmp);
      patternsLoaded = (patternLeft != nullptr || patternRight != nullptr || patternHazard != nullptr);
    }
    f.close();
  }
#else
  void loadPatterns() {} // file loading disabled at compile time
#endif

  // ===== 2D DRAW HELPERS =====
  // Row-major: idx = y * patternW + x. Bounds-checked against matrix.

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

  // Full frame draw — used for hazard blink and wipe-in hold.
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
    signalFellTime = millis() - tailReturnMs; // prevent false trigger on boot

    pinMode(pinHeadlamp, INPUT);
    pinMode(pinBrake,    INPUT);
    pinMode(pinLeft,     INPUT);
    pinMode(pinRight,    INPUT);

    // strip and LittleFS not ready here — both deferred to loop()
  }

  void loop() override {
#ifndef AUTOREARL_DISABLE_FILE_PATTERNS
    if (needLoadPatterns) { // LittleFS ready by loop() — safe to load now
      loadPatterns();
      needLoadPatterns = false;
    }
#endif

    if (!enabled) return;
    unsigned long now = millis();

    // ===== INPUT DEBOUNCE =====

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
    // Rising edge opens window; classify after hazardDetectMs elapses.
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

    // ===== FALLING EDGE =====
    if ((lastLeft || lastRight) && !(left || right)) signalFellTime = now;

    if (!left && !right && (now - signalFellTime >= tailReturnMs)) {
      signalState      = SIG_NONE;
      hazardWindowOpen = false;
    }

    // ===== STATE MACHINE =====
    State newState = head ? TAIL : IDLE;
    if (newState != currentState) {
      currentState = newState;
      if (millis() > wledReadyDelay) {
        if (dimReady){
          applyPreset(head ? presetTail : presetIdle);
        }
        else {
          applyPreset (presetIdle); // always call idle. Do not use "Transpose" here
        }
      }
    }

    // ===== WIPE INTENT =====
    // Flags consumed by handleOverlayDraw() which knows patternW/turnLen.
    bool blinkNow  = (left || right);
    bool blinkRise = blinkNow && !prevBlink;
    bool blinkFall = !blinkNow && prevBlink;

    if (blinkRise)               requestWipeIn  = true;
    if (blinkFall && !holdOnOff) requestWipeOut = true;

    prevBlink = blinkNow;

    lastLeft  = left;
    lastRight = right;
    lastBrake = brake;
    lastHead  = head;
  }

  // ===== OVERLAY DRAW =====
  void handleOverlayDraw() override {
    // Always check strip length is not 0 to avoid crash
    if (strip.getLengthTotal() == 0 ) return;
    if (millis() < wledReadyDelay - 500) return; // minus 500, before applyPreset change
    // only check dimension after boot and config saved, eliminates "Transpose" option breaks the dimension
    if (!dimReady){
      // we need to find alternatives for these calls! these are affected by segment draw
      matrixWidth  = strip.getMainSegment().virtualWidth();
      matrixHeight = strip.getMainSegment().virtualHeight();
      dimReady = true;
    }

    if (!enabled || matrixWidth <= 1) return;

    bool is1D         = (matrixHeight <= 1);
    unsigned long now = millis();
    bool brake        = lastBrake;
    bool signalActive = (lastLeft || lastRight); // raw blink — drives hazard hard-blink
    bool anySignal    = (signalState != SIG_NONE);

    // ===== BACKGROUND FILL =====
    uint8_t br = bgR, bgg = bgG, bb = bgB;
    applyBrightness(br, bgg, bb);
    if (brake || anySignal) {
      for (uint16_t i = 0; i < strip.getLengthTotal(); i++)
        strip.setPixelColor(i, RGBW32(br, bgg, bb, 0));
    }

    // ===== BRAKE FLASH =====
    // Flash-off phase goes full black, overriding background.
    // Reset on release so next press always starts bright.
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
      brakeFlashState = false; // next press starts on bright phase
    }

    // ===== 1D PATH =====
    if (is1D) {
      uint16_t total   = strip.getLengthTotal();
      uint16_t turnLen = min((uint16_t)strip1DTurnLen,   total);
      uint16_t hazLen  = min((uint16_t)strip1DHazardLen, total);

      if (brake && brakeFlashState) return; // don't overdraw during brake flash-on

      if (anySignal) {
        uint8_t r = (signalState == SIG_HAZARD) ? hazardR : turnR;
        uint8_t g = (signalState == SIG_HAZARD) ? hazardG : turnG;
        uint8_t b = (signalState == SIG_HAZARD) ? hazardB : turnB;
        applyBrightness(r, g, b);

        if (signalState == SIG_HAZARD) {
          // Hard blink driven by raw signal, centered hazLen pixels.
          if (signalActive) {
            uint16_t start = (total - hazLen) / 2;
            for (uint16_t i = start; i < start + hazLen; i++)
              strip.setPixelColor(i, RGBW32(r, g, b, 0));
          }
        } else {
          // Turn wipe — wipeColumn in pixel units (turnLen = virtual patternW).
          // Invariant: 0 = blank sentinel, turnLen in WIPE_IDLE = fully shown.

          // Signal change mid-animation -> restart from blank
          if (signalState != prevSignalState) {
            wipeColumn     = 0;
            wipeState      = WIPE_IN;
            requestWipeIn  = false;
            requestWipeOut = false;
          }

          // Consume wipe intent
          if (requestWipeIn) {
            wipeColumn    = 0;
            wipeState     = WIPE_IN;
            wipeStartTime = now;
            requestWipeIn = false;
          }
          if (requestWipeOut) {
            wipeState      = WIPE_OUT;
            wipeColumn     = (wipeOutMode == 0) ? turnLen : 0;
            requestWipeOut = false;
          }

          // Advance wipe
          if (now - lastWipeStep >= wipeSpeedMs) {
            lastWipeStep = now;
            switch (wipeState) {
              case WIPE_IN:
                if (wipeColumn < turnLen) wipeColumn++;
                else wipeState = WIPE_IDLE; // wipeColumn == turnLen: fully shown
                break;
              case WIPE_OUT:
                if (wipeOutMode == 0) {
                  if (wipeColumn > 0) wipeColumn--;
                  else wipeState = WIPE_IDLE; // wipeColumn already 0: blank sentinel
                } else if (wipeOutMode == 1) {
                  if (wipeColumn < turnLen) wipeColumn++;
                  else { wipeColumn = 0; wipeState = WIPE_IDLE; } // normalize to blank sentinel
                } else { // mode 2: hard blank
                  wipeColumn = 0; wipeState = WIPE_IDLE;          // normalize to blank sentinel
                }
                break;
              case WIPE_IDLE: break;
            }
          }
          if (wipeColumn > turnLen) wipeColumn = turnLen;

          // wipeColumn == 0 in WIPE_IDLE -> blank: drawStart == drawEnd, nothing drawn.
          uint16_t drawStart = 0;
          uint16_t drawEnd   = wipeColumn;
          if (wipeState == WIPE_OUT && wipeOutMode == 1) {
            drawStart = wipeColumn;
            drawEnd   = turnLen;
          }

          for (uint16_t i = drawStart; i < drawEnd; i++) { // outer edge inward
            int x = (signalState == SIG_LEFT) ? (int)(total - 1 - i) : (int)i;
            strip.setPixelColor(x, RGBW32(r, g, b, 0));
          }
        }
      }

      prevSignalState = signalState;
      return;
    } // end 1D

    // ===== 2D PATH =====
    // Allow wipe-out to finish even after signalState returns to SIG_NONE.
    if (!anySignal && wipeState == WIPE_IDLE) {
      prevSignalState = signalState;
      return;
    }

    uint8_t r = (signalState == SIG_HAZARD) ? hazardR : turnR;
    uint8_t g = (signalState == SIG_HAZARD) ? hazardG : turnG;
    uint8_t b = (signalState == SIG_HAZARD) ? hazardB : turnB;
    applyBrightness(r, g, b);

    const uint8_t* pattern;
    uint16_t patternW, patternH;
    int offsetX, offsetY;
    bool isProgmem = false;

    // ===== PATTERN SELECTION =====
    // null pointer = malloc failed silently; falls back to PROGMEM.
    switch (signalState) {
      case SIG_HAZARD:
        if (patternsLoaded && patternHazard) {
          pattern = patternHazard; patternW = patternHazardW; patternH = patternHazardH;
          isProgmem = false;
        } else {
          pattern = &arrowHazard[0][0]; patternW = ARRAY_W(arrowHazard); patternH = ARRAY_H(arrowHazard);
          isProgmem = true;
        }
        offsetX = centerOffsetX(patternW);
        offsetY = min((int)centerOffsetY(patternH), max(0, (int)matrixHeight - (int)patternH));
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
        offsetY = min((int)centerOffsetY(patternH), max(0, (int)matrixHeight - (int)patternH));
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
        offsetY = min((int)centerOffsetY(patternH), max(0, (int)matrixHeight - (int)patternH));
        break;

      default:
        // SIG_NONE with wipe still running — no geometry, nothing to draw.
        prevSignalState = signalState;
        return;
    }

    // ===== WIPE REQUEST HANDLING =====
    if (signalState != prevSignalState) { // signal change — restart from blank
      wipeColumn     = 0;
      wipeState      = WIPE_IN;
      requestWipeIn  = false;
      requestWipeOut = false;
    }

    if (requestWipeIn) {
      wipeColumn    = 0;
      wipeState     = WIPE_IN;
      wipeStartTime = now;
      requestWipeIn = false;
    }
    if (requestWipeOut && !holdOnOff) {
      wipeState      = WIPE_OUT;
      wipeColumn     = (wipeOutMode == 0) ? patternW : 0;
      requestWipeOut = false;
    } else {
      requestWipeOut = false; // discard if holdOnOff or already consumed
    }

    // ===== HAZARD: FULL FRAME, NO WIPE =====
    if (signalState == SIG_HAZARD) {
      if (signalActive) drawFull(pattern, patternW, patternH, offsetX, offsetY, r, g, b, isProgmem);
      prevSignalState = signalState;
      return;
    }

    // ===== WIPE ANIMATION STEP =====
    // 0 in WIPE_IDLE = blank sentinel; patternW in WIPE_IDLE = fully shown.
    if (now - lastWipeStep >= wipeSpeedMs) {
      lastWipeStep = now;
      switch (wipeState) {
        case WIPE_IN:
          if (wipeColumn < patternW) wipeColumn++;
          else wipeState = WIPE_IDLE; // wipeColumn == patternW: fully shown
          break;
        case WIPE_OUT:
          if (wipeOutMode == 0) {
            if (wipeColumn > 0) wipeColumn--;
            else wipeState = WIPE_IDLE; // wipeColumn already 0: blank sentinel
          } else if (wipeOutMode == 1) {
            if (wipeColumn < patternW) wipeColumn++;
            else { wipeColumn = 0; wipeState = WIPE_IDLE; } // normalize to blank sentinel
          } else { // mode 2: hard blank
            wipeColumn = 0; wipeState = WIPE_IDLE;          // normalize to blank sentinel
          }
          break;
        case WIPE_IDLE: break;
      }
    }
    if (wipeColumn > patternW) wipeColumn = patternW;

    // ===== DRAW RANGE CALCULATION =====
    // mode 0/WIPE_IN: [0, wipeColumn) — mode 1 WIPE_OUT: [wipeColumn, patternW)
    uint16_t drawStart = 0;
    uint16_t drawEnd   = wipeColumn;
    if (wipeState == WIPE_OUT && wipeOutMode == 1) {
      drawStart = wipeColumn;
      drawEnd   = patternW;
    }

    // ===== DRAW =====
    if (wipeState == WIPE_IDLE) {
      // 0 = blank sentinel; patternW = hold full until next wipe-out
      if (wipeColumn == patternW && anySignal) {
        drawFull(pattern, patternW, patternH, offsetX, offsetY, r, g, b, isProgmem);
      }
    } else {
      for (uint16_t i = drawStart; i < drawEnd; i++) {
        int col = (signalState == SIG_LEFT) ? ((int)patternW - 1) - (int)i : (int)i;
        drawColumn(pattern, patternW, patternH, col, offsetX, offsetY, r, g, b, isProgmem);
      }
    }

    prevSignalState = signalState;
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

    // Compare with currentFile to detect changes; defer load to loop().
    {
      const char* tmp_pf = top["Pattern File"] | "/autoRearLight.txt";
      if (strcmp(tmp_pf, currentFile) != 0) {
        strncpy(patternFile, tmp_pf, sizeof(patternFile) - 1);
        patternFile[sizeof(patternFile) - 1] = '\0';
        strncpy(currentFile, patternFile, sizeof(currentFile) - 1);
        currentFile[sizeof(currentFile) - 1] = '\0';
        needLoadPatterns = true; // consumed in loop()
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
      dimReady = false;
    }

    return configComplete;
  }
#else
  void addToConfig(JsonObject& root) override {}
  bool readFromConfig(JsonObject& root) override {}   
  #warning "AutoRearLight: Usermod config is DISABLED. Using hardcoded settings."
#endif // AUTOREARL_DISABLE_CONFIG

  // ===== INFO PANEL =====
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
    
    JsonArray dbg = user.createNestedArray("SegDim");
    dbg.add(strip.getMainSegment().virtualWidth());
    dbg.add(strip.getMainSegment().virtualHeight());
  }
};

static AutoRearLightUsermod autoRearLight;
REGISTER_USERMOD(autoRearLight);