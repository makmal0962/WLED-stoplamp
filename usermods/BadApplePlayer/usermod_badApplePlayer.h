#pragma once
#include "wled.h"
#include "LittleFS.h"

// #define BADAPPLE_USE_SD   // uncomment to stream from SD instead of LittleFS
// #define BADAPPLE_RLE      // must match how badapple.bin was encoded

#define USERMOD_ID_BADAPPLE 242

#ifdef BADAPPLE_USE_SD
  #include <SD.h>
  #define BA_FS SD
#else
  #define BA_FS LittleFS
#endif

#define BA_MAGIC     "BA2B"
#define BA_PATH      "/badapple.bin"
#define BA_W         16
#define BA_H         8
#define BA_PPF       (BA_W * BA_H)         // pixels per frame
#define BA_BPF       ((BA_PPF * 2 + 7)/8)  // bytes per frame = 32

class BadAppleUsermod : public Usermod {
private:
    static const uint8_t BA_LEVELS[4];
    bool    _enabled    = false;
    uint8_t _fps        = 15;
    uint8_t _brightness = 255;
    bool    _loop       = true;

    File    _file;
    bool    _playing    = false;
    uint16_t _frameCount = 0;
    uint16_t _curFrame   = 0;
    uint32_t _lastRender = 0;

    // RLE streaming state
    uint8_t  _rleByte   = 0;
    int16_t  _rleLeft   = 0; // remaining repeats of _rleByte
    uint32_t _dataStart = 0; // file offset where frame data begins

    bool openFile() {
        _file = BA_FS.open(BA_PATH, "r");
        if (!_file) { DEBUG_PRINTLN("BadApple: file not found"); return false; }

        char magic[5] = {};
        _file.readBytes(magic, 4);
        if (strncmp(magic, BA_MAGIC, 4) != 0) {
            DEBUG_PRINTLN("BadApple: bad magic");
            _file.close(); return false;
        }

        uint16_t fc; uint8_t w, h, flags, res[3];
        _file.readBytes((char*)&fc, 2);
        _file.read(); _file.read(); // w, h — fixed BA_W/BA_H
        flags = _file.read();
        _file.readBytes((char*)res, 3);

        _frameCount = fc;
        _dataStart  = _file.position();
        _rleLeft    = 0;
        _curFrame   = 0;
        return true;
    }

    // Read one raw byte from file (handles RLE transparently if BADAPPLE_RLE)
    int readByte() {
#ifdef BADAPPLE_RLE
        if (_rleLeft > 0) { _rleLeft--; return _rleByte; }
        int cnt = _file.read();
        if (cnt < 0) return -1;
        _rleByte = _file.read();
        _rleLeft = cnt; // will be decremented after return
        return _rleByte;
#else
        return _file.read();
#endif
    }

    void renderFrame() {
        uint8_t buf[BA_BPF];
        for (int i = 0; i < BA_BPF; i++) {
            int b = readByte();
            if (b < 0) { stopPlayback(); return; }
            buf[i] = (uint8_t)b;
        }

        for (int px = 0; px < BA_PPF; px++) {
            uint8_t level  = (buf[(px * 2) / 8] >> (6 - ((px * 2) % 8))) & 0x03;
            uint8_t bright = (uint16_t)BA_LEVELS[level] * _brightness / 255;
            strip.setPixelColorXY(px % BA_W, px / BA_W, bright, bright, bright);
        }

        _curFrame++;
        if (_curFrame >= _frameCount) {
            if (_loop) {
                // seek back to data start, reset RLE state
                _file.seek(_dataStart);
                _rleLeft = 0;
                _curFrame = 0;
            } else {
                stopPlayback();
            }
        }
    }

    void stopPlayback() {
        _playing = false;
        if (_file) _file.close();
    }

    void startPlayback() {
        stopPlayback();
        if (!openFile()) return;
        _playing = true;
        _lastRender = 0;
    }

public:
    void setup() override {
#ifdef BADAPPLE_USE_SD
        if (!SD.begin()) DEBUG_PRINTLN("BadApple: SD init failed");
#else
        if (!LittleFS.begin()) DEBUG_PRINTLN("BadApple: LittleFS init failed");
#endif
        if (_enabled) startPlayback();
    }

    void loop() override {}

    void handleOverlayDraw() override {
        if (!_enabled || !_playing) return;
        uint32_t now = millis();
        if (now - _lastRender < (1000u / _fps)) return;
        _lastRender = now;
        renderFrame();
    }

    void addToConfig(JsonObject& root) override {
        JsonObject top = root.createNestedObject("BadApple");
        top["enabled"]    = _enabled;
        top["fps"]        = _fps;
        top["brightness"] = _brightness;
        top["loop"]       = _loop;
    }

    bool readFromConfig(JsonObject& root) override {
        JsonObject top = root["BadApple"];
        if (top.isNull()) return false;
        bool wasEnabled = _enabled;
        getJsonValue(top["enabled"],    _enabled);
        getJsonValue(top["fps"],        _fps);
        getJsonValue(top["brightness"], _brightness);
        getJsonValue(top["loop"],       _loop);
        if (_enabled && !wasEnabled) startPlayback();
        if (!_enabled && wasEnabled)  stopPlayback();
        return true;
    }

    void addToJsonInfo(JsonObject& root) override {
        JsonObject user = root["u"];
        if (user.isNull()) user = root.createNestedObject("u");
        user["BadApple"] = _playing
            ? String(_curFrame) + "/" + String(_frameCount)
            : "stopped";
    }

    uint16_t getId() override { return USERMOD_ID_BADAPPLE; } // define in const.h or use any free ID
};


static BadAppleUsermod badApple;
REGISTER_USERMOD(badApple);