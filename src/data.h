#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "ble_bridge.h"
#include "xfer.h"

struct TamaState {
  uint8_t  sessionsTotal;
  uint8_t  sessionsRunning;
  uint8_t  sessionsWaiting;
  bool     recentlyCompleted;
  uint32_t tokensToday;
  uint32_t lastUpdated;
  char     msg[24];
  bool     connected;
  char     lines[8][92];
  uint8_t  nLines;
  uint16_t lineGen;          // bumps when lines change — lets UI reset scroll
  char     promptId[40];     // pending permission request ID; empty = no prompt
  char     promptTool[20];
  char     promptHint[44];
  // Latest AskUserQuestion (parsed from turn events) — shown read-only when an
  // "approve: AskUserQuestion" prompt is active. The device can't submit a
  // choice (protocol has no answer command); the user answers on the desktop.
  char     qHeader[24];
  char     qText[200];
  char     qOpts[4][44];
  uint8_t  qNOpts;
  bool     qMulti;
};

// ---------------------------------------------------------------------------
// Three modes, checked in priority order:
//   demo   → auto-cycle fake scenarios every 8s, ignore live data
//   live   → JSON arrived in the last 10s over USB or BT
//   asleep → no data, all zeros, "No Claude connected"
// ---------------------------------------------------------------------------

static uint32_t _lastLiveMs = 0;
static uint32_t _lastBtByteMs = 0;   // hasClient() lies; track actual BT traffic
static bool     _demoMode   = false;
static uint8_t  _demoIdx    = 0;
static uint32_t _demoNext   = 0;

struct _Fake { const char* n; uint8_t t,r,w; bool c; uint32_t tok; };
static const _Fake _FAKES[] = {
  {"asleep",0,0,0,false,0}, {"one idle",1,0,0,false,12000},
  {"busy",4,3,0,false,89000}, {"attention",2,1,1,false,45000},
  {"completed",1,0,0,true,142000},
};

inline void dataSetDemo(bool on) {
  _demoMode = on;
  if (on) { _demoIdx = 0; _demoNext = millis(); }
}
inline bool dataDemo() { return _demoMode; }

inline bool dataConnected() {
  return _lastLiveMs != 0 && (millis() - _lastLiveMs) <= 30000;
}

inline bool dataBtActive() {
  // Desktop's idle keepalive is ~10s; give it 1.5x headroom.
  return _lastBtByteMs != 0 && (millis() - _lastBtByteMs) <= 15000;
}

inline const char* dataScenarioName() {
  if (_demoMode) return _FAKES[_demoIdx].n;
  if (dataConnected()) return dataBtActive() ? "bt" : "usb";
  return "none";
}

// Set true once the bridge sends a time sync — until then the RTC may
// hold whatever was on the coin cell (or 2000-01-01 if it lost power).
static bool _rtcValid = false;
inline bool dataRtcValid() { return _rtcValid; }

// The desktop bridge emits a "(no message)" / "(no messages)" placeholder
// when there's nothing to show. Treat any string containing it (case-
// insensitive) as empty so the display stays blank instead of printing it.
static bool _isNoMsg(const char* s) {
  if (!s) return false;
  for (const char* p = s; *p; ++p) {
    const char* a = p;
    const char* b = "(no message";
    while (*b) {
      char c = *a;
      if (c >= 'A' && c <= 'Z') c += 32;
      if (c != *b) break;
      ++a; ++b;
    }
    if (!*b) return true;
  }
  return false;
}

static void _sanitize(char* dst, const char* src, size_t cap);   // defined below

// Pull questions[0] (text, header, option labels) from an AskUserQuestion
// tool_use block in a turn event. Stored for read-only display.
static void _parseQuestion(JsonObject blk, TamaState* out) {
  JsonObject q0 = blk["input"]["questions"][0];
  if (q0.isNull()) return;
  const char* qt = q0["question"];
  const char* qh = q0["header"];
  _sanitize(out->qText,   qt ? qt : "", sizeof(out->qText));
  _sanitize(out->qHeader, qh ? qh : "", sizeof(out->qHeader));
  out->qMulti = q0["multiSelect"] | false;
  uint8_t n = 0;
  for (JsonObject op : q0["options"].as<JsonArray>()) {
    if (n >= 4) break;
    const char* lb = op["label"];
    _sanitize(out->qOpts[n], lb ? lb : "", sizeof(out->qOpts[n]));
    n++;
  }
  out->qNOpts = n;
}

// FreeMono9pt7b (and the GLCD font) only cover ASCII 0x20-0x7E. Desktop text
// often contains UTF-8 typographic punctuation (em dash, curly quotes, ellipsis)
// that would otherwise render as "tofu" boxes. Map the common ones to ASCII and
// drop anything else non-printable. Copies up to cap-1 chars + null.
static void _sanitize(char* dst, const char* src, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; src[i] && o < cap - 1; ) {
    unsigned char c = (unsigned char)src[i];
    if (c < 0x80) {                                  // plain ASCII
      dst[o++] = (c >= 0x20 && c < 0x7F) ? (char)c : ' ';
      i++;
      continue;
    }
    uint32_t cp = 0; int len;                        // decode UTF-8 sequence
    if      ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
    else { i++; continue; }                          // stray byte → skip
    for (int k = 1; k < len; k++) {
      unsigned char cc = (unsigned char)src[i + k];
      if ((cc & 0xC0) != 0x80) { len = k; break; }   // truncated sequence
      cp = (cp << 6) | (cc & 0x3F);
    }
    i += len;
    const char* rep;
    switch (cp) {
      case 0x2012: case 0x2013: case 0x2014: case 0x2015: rep = "-";   break; // dashes
      case 0x2018: case 0x2019: case 0x201B: case 0x2032: rep = "'";   break; // single quotes
      case 0x201C: case 0x201D: case 0x201F: case 0x2033: rep = "\"";  break; // double quotes
      case 0x2026: rep = "..."; break;                                        // ellipsis
      case 0x2022: case 0x00B7: case 0x2219: rep = "*";  break;               // bullet / middot
      case 0x2192: rep = "->"; break;                                         // right arrow
      case 0x00A0: rep = " ";  break;                                         // nbsp
      default:     rep = "?";  break;                                         // unknown glyph
    }
    for (const char* r = rep; *r && o < cap - 1; r++) dst[o++] = *r;
  }
  dst[o] = 0;
}

static void _applyJson(const char* line, TamaState* out) {
  // Turn events mirror every session's content. We only care about an
  // AskUserQuestion tool call; skip the rest cheaply and — importantly —
  // never let a turn event clear the active permission prompt below.
  if (strstr(line, "\"evt\":\"turn\"")) {
    _lastLiveMs = millis();
    if (!strstr(line, "AskUserQuestion")) return;
    JsonDocument tdoc;
    if (deserializeJson(tdoc, line)) return;
    JsonArray content = tdoc["content"];
    if (content.isNull()) return;
    for (JsonObject blk : content) {
      const char* bt = blk["type"];
      const char* bn = blk["name"];
      if (bt && bn && strcmp(bt, "tool_use") == 0 && strcmp(bn, "AskUserQuestion") == 0) {
        _parseQuestion(blk, out);
        break;
      }
    }
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, line)) return;
  if (xferCommand(doc)) { _lastLiveMs = millis(); return; }

  // Bridge sends {"time":[epoch_sec, tz_offset_sec]}; set the ESP32 system
  // clock so gettimeofday()/localtime_r() work in the clock display.
  // No battery-backed RTC — time resets to epoch on power cycle.
  JsonArray t = doc["time"];
  if (!t.isNull() && t.size() == 2) {
    time_t local = (time_t)t[0].as<uint32_t>() + (int32_t)t[1];
    struct timeval tv = { .tv_sec = local, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
    extern uint32_t _clkLastRead;
    _clkLastRead = 0;   // force re-read in clockRefreshRtc()
    _rtcValid = true;
    _lastLiveMs = millis();
    return;
  }

  out->sessionsTotal     = doc["total"]     | out->sessionsTotal;
  out->sessionsRunning   = doc["running"]   | out->sessionsRunning;
  out->sessionsWaiting   = doc["waiting"]   | out->sessionsWaiting;
  out->recentlyCompleted = doc["completed"] | false;
  uint32_t bridgeTokens = doc["tokens"] | 0;
  if (doc["tokens"].is<uint32_t>()) statsOnBridgeTokens(bridgeTokens);
  out->tokensToday = doc["tokens_today"] | out->tokensToday;
  const char* m = doc["msg"];
  if (_isNoMsg(m)) m = "";
  if (m) _sanitize(out->msg, m, sizeof(out->msg));
  JsonArray la = doc["entries"];
  if (!la.isNull()) {
    uint8_t n = 0;
    for (JsonVariant v : la) {
      if (n >= 8) break;
      const char* s = v.as<const char*>();
      if (_isNoMsg(s)) continue;   // skip placeholder entries
      _sanitize(out->lines[n], s ? s : "", sizeof(out->lines[n]));
      n++;
    }
    if (n != out->nLines || (n > 0 && strcmp(out->lines[n-1], out->msg) != 0)) {
      out->lineGen++;
    }
    out->nLines = n;
  }
  JsonObject pr = doc["prompt"];
  if (!pr.isNull()) {
    const char* pid = pr["id"]; const char* pt = pr["tool"]; const char* ph = pr["hint"];
    strncpy(out->promptId,   pid ? pid : "", sizeof(out->promptId)-1);   out->promptId[sizeof(out->promptId)-1]=0;
    _sanitize(out->promptTool, pt ? pt : "", sizeof(out->promptTool));
    _sanitize(out->promptHint, ph ? ph : "", sizeof(out->promptHint));
  } else {
    out->promptId[0] = 0; out->promptTool[0] = 0; out->promptHint[0] = 0;
  }
  out->lastUpdated = millis();
  _lastLiveMs = millis();
}

template<size_t N>
struct _LineBuf {
  char buf[N];
  uint16_t len = 0;
  void feed(Stream& s, TamaState* out) {
    while (s.available()) {
      char c = s.read();
      if (c == '\n' || c == '\r') {
        if (len > 0) { buf[len]=0; if (buf[0]=='{') _applyJson(buf, out); len=0; }
      } else if (len < N-1) {
        buf[len++] = c;
      }
    }
  }
};

// 4608 bytes: turn events carry the full SDK content array (capped at 4KB by
// the desktop). The old 1024 buffer truncated them, so AskUserQuestion options
// never parsed. BLE is the real transport; USB mirrors the size for parity.
static _LineBuf<4608> _usbLine, _btLine;

inline void dataPoll(TamaState* out) {
  uint32_t now = millis();

  if (_demoMode) {
    if (now >= _demoNext) { _demoIdx = (_demoIdx + 1) % 5; _demoNext = now + 8000; }
    const _Fake& s = _FAKES[_demoIdx];
    out->sessionsTotal=s.t; out->sessionsRunning=s.r; out->sessionsWaiting=s.w;
    out->recentlyCompleted=s.c; out->tokensToday=s.tok; out->lastUpdated=now;
    out->connected = true;
    snprintf(out->msg, sizeof(out->msg), "demo: %s", s.n);
    return;
  }

  _usbLine.feed(Serial, out);
  // BLE ring buffer is drained manually since it's not a Stream.
  while (bleAvailable()) {
    int c = bleRead();
    if (c < 0) break;
    _lastBtByteMs = millis();
    if (c == '\n' || c == '\r') {
      if (_btLine.len > 0) {
        _btLine.buf[_btLine.len] = 0;
        if (_btLine.buf[0] == '{') _applyJson(_btLine.buf, out);
        _btLine.len = 0;
      }
    } else if (_btLine.len < sizeof(_btLine.buf) - 1) {
      _btLine.buf[_btLine.len++] = (char)c;
    }
  }

  out->connected = dataConnected();
  if (!out->connected) {
    out->sessionsTotal=0; out->sessionsRunning=0; out->sessionsWaiting=0;
    out->recentlyCompleted=false; out->lastUpdated=now;
    strncpy(out->msg, "No Claude connected", sizeof(out->msg)-1);
    out->msg[sizeof(out->msg)-1]=0;
  }
}
