#include "api.hpp"
#include "ui.h"
#include "logic/moka.hpp"
#include "logic/pressureSensor.hpp"
#include "logic/ota.hpp"
#include "logic/brewHistory.hpp"
#include "logic/webserver.hpp"

const char *const API_GET_PATHS[] = {
  "/temperature", "/brewInfo", "/brewHistory", "/brewSlots", "/brewSlot",
  "/settingsInfo", "/set", nullptr
};
const char *const API_POST_PATHS[] = {
  "/brewSave", "/brewDelete", "/brewImport", nullptr
};

// ---- Query string arguments ----

static int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Decodes %XX escapes and '+' (space), as browsers encode query strings.
static String urlDecode(const String &text) {
  String out;
  out.reserve(text.length());
  for (size_t k = 0; k < text.length(); k++) {
    char c = text[k];
    if (c == '+') out += ' ';
    else if (c == '%' && k + 2 < text.length() && hexValue(text[k + 1]) >= 0 && hexValue(text[k + 2]) >= 0) {
      out += (char)(hexValue(text[k + 1]) * 16 + hexValue(text[k + 2]));
      k += 2;
    }
    else out += c;
  }
  return out;
}

bool ApiQueryArgs::find(const char *name, String *value) const {
  int start = 0;
  while (start <= (int)query.length()) {
    int end = query.indexOf('&', start);
    if (end < 0) end = query.length();
    int equals = query.indexOf('=', start);
    bool hasValue = equals >= 0 && equals < end;
    String key = urlDecode(query.substring(start, hasValue ? equals : end));
    if (key == name) {
      if (value) *value = hasValue ? urlDecode(query.substring(equals + 1, end)) : String();
      return true;
    }
    start = end + 1;
  }
  return false;
}

bool ApiQueryArgs::has(const char *name) const {
  return find(name, nullptr);
}

String ApiQueryArgs::get(const char *name) const {
  String value;
  find(name, &value);
  return value;
}

// ---- JSON helpers ----

// Appends value as a JSON string literal (quotes, backslashes and control
// characters escaped).
static void appendJsonString(String &json, const String &value) {
  json += '"';
  for (size_t k = 0; k < value.length(); k++) {
    char c = value[k];
    if (c == '"' || c == '\\') { json += '\\'; json += c; }
    else if ((unsigned char)c < 0x20) { char esc[8]; snprintf(esc, sizeof(esc), "\\u%04x", c); json += esc; }
    else json += c;
  }
  json += '"';
}

// Appends "name":value to a JSON object body; NaN becomes null.
// decimals < 0 uses %g with 10 significant digits (for very small values).
static void appendJson(String &json, const char *name, double value, int decimals = 6) {
  char str[64];
  if (isnan(value)) snprintf(str, sizeof(str), "\"%s\":null", name);
  else if (decimals < 0) snprintf(str, sizeof(str), "\"%s\":%.10g", name, value);
  else snprintf(str, sizeof(str), "\"%s\":%.*f", name, decimals, value);
  if (json.length() > 1) json += ",";
  json += str;
}

// {"temp":<C>,"pressure":<bar>}
static String retrieveData() {
  String json = "{";
  appendJson(json, "temp", getCurrentTemp());
  appendJson(json, "pressure", brewInternalPressure());
  json += "}";
  return json;
}

// {"temp":<C>,"pressure":<bar>,"time":<brew seconds>,"status":<moka status>}
static String retrieveExtractionData() {
  String json = "{";
  appendJson(json, "temp", getCurrentTemp());
  appendJson(json, "pressure", brewInternalPressure());
  appendJson(json, "time", getBrewTime());
  appendJson(json, "status", getMokaStatus());
  json += "}";
  return json;
}

// All settings values as JSON, keyed by the settings form input ids.
static String retrieveSettings() {
  String json = "{";
  double current[6];
  getPID(current);
  appendJson(json, "PVal", current[0]);
  appendJson(json, "IVal", current[1]);
  appendJson(json, "DVal", current[2]);
  getResistorValues(current);
  appendJson(json, "RVal", current[0]);
  appendJson(json, "SHA", current[1], -1);
  appendJson(json, "SHB", current[2], -1);
  appendJson(json, "SHC", current[3], -1);
  appendJson(json, "BloomTime", getBloomTargetTime());
  appendJson(json, "BoilTemp", getBoilingTemp(getCurrentPressure()/100000), 2);
  appendJson(json, "BloomOffset", getBloomOffset());
  appendJson(json, "ManualCutoff", getManualCutoff(), 0);
  appendJson(json, "PreheatOffset", getPreheatOffset());
  // Celsius; the page shows it in Fahrenheit when the screen uses it.
  appendJson(json, "OvershootMargin", getOvershootMargin());
  appendJson(json, "Fahrenheit", ui_use_fahrenheit() ? 1 : 0, 0);
  appendJson(json, "Decimals", ui_show_decimals() ? 1 : 0, 0);
  char hex[32];
  snprintf(hex, sizeof(hex), ",\"ButtonColor\":\"#%06lx\"", (unsigned long)ui_get_button_color());
  json += hex;
  json += ",\"DeviceName\":";
  appendJsonString(json, getDeviceName());
  json += ",\"OtaUrl\":";
  appendJsonString(json, otaGetUrl());
  json += ",\"OtaFile\":";
  appendJsonString(json, otaFirmwareFile());
  json += "}";
  return json;
}

// ---- /set ----

// Reads a "#rrggbb" / "rrggbb" query argument. Returns false if missing or invalid.
static bool readHexColorArg(const ApiArgs &args, const char *name, uint32_t &out) {
  if (!args.has(name)) return false;
  String value = args.get(name);
  if (value.startsWith("#")) value = value.substring(1);
  if (value.length() != 6) return false;
  for (size_t k = 0; k < value.length(); k++) {
    if (!isxdigit((unsigned char)value[k])) return false;
  }
  out = strtoul(value.c_str(), nullptr, 16);
  return true;
}

// Reads a numeric query argument. Returns false if missing, empty or below minVal.
static bool readArg(const ApiArgs &args, const char *name, double minVal, double &out) {
  if (!args.has(name)) return false;
  String value = args.get(name);
  if (value.length() == 0) return false;
  double parsed = value.toDouble();
  if (parsed < minVal) return false;
  out = parsed;
  return true;
}

static void applySettings(const ApiArgs &args) {
  double value;

  // PID
  double pid[3];
  getPID(pid);
  bool pidChanged = false;
  pidChanged |= readArg(args, "PVal", 0.0, pid[0]);
  pidChanged |= readArg(args, "IVal", 0.0, pid[1]);
  pidChanged |= readArg(args, "DVal", 0.0, pid[2]);
  if (pidChanged) setPID(pid[0], pid[1], pid[2]);

  // Resistors
  double resistors[6];
  getResistorValues(resistors);
  bool resistorsChanged = false;
  resistorsChanged |= readArg(args, "RVal", 0.0, resistors[0]);
  resistorsChanged |= readArg(args, "SHA", -1.0, resistors[1]);
  resistorsChanged |= readArg(args, "SHB", -1.0, resistors[2]);
  resistorsChanged |= readArg(args, "SHC", -1.0, resistors[3]);
  if (resistorsChanged) setResistorValues(resistors[0], resistors[1], resistors[2], resistors[3]);

  // Bloom / boil
  if (readArg(args, "BloomTime", 0.0, value)) setBloomTime(value);
  if (readArg(args, "BoilTemp", 0.0, value)) overrideBoiling(value);
  if (readArg(args, "BloomOffset", -20.0, value)) setBloomOffset(value);
  // Clamped to -10..0 by setPreheatOffset().
  if (readArg(args, "PreheatOffset", -1e9, value)) setPreheatOffset(value);
  // Celsius (converted by the page); clamped to 0.5..10 by setOvershootMargin().
  if (readArg(args, "OvershootMargin", 0.0, value)) setOvershootMargin(value);
  // Any integer, negative or positive.
  if (readArg(args, "ManualCutoff", -1e9, value)) setManualCutoff((int)lround(value));

  // UI
  uint32_t buttonColor;
  if (readHexColorArg(args, "ButtonColor", buttonColor)) ui_set_button_color(buttonColor);
  if (readArg(args, "Fahrenheit", 0.0, value)) ui_set_use_fahrenheit(value != 0);
  if (readArg(args, "Decimals", 0.0, value)) ui_set_show_decimals(value != 0);

  // Network (an invalid name is ignored; the page shows the stored one)
  if (args.has("DeviceName")) setDeviceName(args.get("DeviceName").c_str());

  // OTA
  if (args.has("OtaUrl")) otaSetUrl(args.get("OtaUrl"));
}

// ---- Dispatch ----

static void respond(ApiResponse &out, int status, const char *type, const String &body) {
  out.status = status;
  out.contentType = type;
  out.body = body;
}

static bool listed(const char *const *paths, const String &path) {
  for (int k = 0; paths[k]; k++) {
    if (path == paths[k]) return true;
  }
  return false;
}

bool apiHandle(const String &method, const String &path, const ApiArgs &args,
               const String &body, ApiResponse &out) {
  bool isGet = listed(API_GET_PATHS, path);
  bool isPost = listed(API_POST_PATHS, path);
  if (!isGet && !isPost) return false;
  if ((isGet && method != "GET") || (isPost && method != "POST")) {
    respond(out, 405, "text/plain", "Method not allowed");
    return true;
  }

  if (path == "/temperature") respond(out, 200, "application/json", retrieveData());
  else if (path == "/brewInfo") respond(out, 200, "application/json", retrieveExtractionData());
  else if (path == "/settingsInfo") respond(out, 200, "application/json", retrieveSettings());
  else if (path == "/brewSlots") respond(out, 200, "application/json", brewSlotsJson());
  else if (path == "/set") {
    applySettings(args);
    respond(out, 200, "text/plain", "OK");
  }
  // GET /brewHistory?session=S&from=N: the recorded brew samples (all of them
  // for a new session, otherwise only those from index N).
  else if (path == "/brewHistory") {
    long session = args.has("session") ? args.get("session").toInt() : -1;
    long from = args.has("from") ? args.get("from").toInt() : 0;
    respond(out, 200, "application/json", brewHistoryJson(session, from));
  }
  // GET /brewSlot?slot=A|B|C: the samples of the brew saved in that slot.
  else if (path == "/brewSlot") {
    String slot = args.get("slot");
    String json = slot.length() == 1 ? brewSlotJson(slot[0]) : String();
    if (json.length() == 0) respond(out, 404, "text/plain", "Empty slot");
    else respond(out, 200, "application/json", json);
  }
  // POST /brewSave?slot=A|B|C: saves the finished brew into that slot.
  else if (path == "/brewSave") {
    String slot = args.get("slot");
    BrewSlotResult result = slot.length() == 1 ? brewHistorySaveSlot(slot[0]) : BREW_SLOT_INVALID;
    switch (result) {
      case BREW_SLOT_SAVED:   respond(out, 200, "text/plain", "Saved to slot " + slot); break;
      case BREW_SLOT_INVALID: respond(out, 400, "text/plain", "Unknown slot"); break;
      case BREW_SLOT_NOT_OFF: respond(out, 409, "text/plain", "Switch the moka OFF first"); break;
      case BREW_SLOT_EMPTY:   respond(out, 404, "text/plain", "No finished brew to save"); break;
      default:                respond(out, 500, "text/plain", "Could not write the slot"); break;
    }
  }
  // POST /brewImport?slot=A|B|C&mode=2|3&power=P, body: the samples, one per
  // line (see brewHistoryImportSlot); the settings page converts an exported
  // .json file into this.
  else if (path == "/brewImport") {
    String slot = args.get("slot");
    BrewSlotResult result = slot.length() == 1
      ? brewHistoryImportSlot(slot[0], args.get("mode").toInt(), args.get("power").toInt(), body.c_str())
      : BREW_SLOT_INVALID;
    switch (result) {
      case BREW_SLOT_SAVED:    respond(out, 200, "text/plain", "Imported into slot " + slot); break;
      case BREW_SLOT_INVALID:  respond(out, 400, "text/plain", "Unknown slot"); break;
      case BREW_SLOT_NOT_OFF:  respond(out, 409, "text/plain", "Switch the moka OFF first"); break;
      case BREW_SLOT_BAD_DATA: respond(out, 400, "text/plain", "Not a valid MokaBot brew file"); break;
      default:                 respond(out, 500, "text/plain", "Could not write the slot"); break;
    }
  }
  // POST /brewDelete?slot=A|B|C: deletes the brew saved in that slot.
  else if (path == "/brewDelete") {
    String slot = args.get("slot");
    BrewSlotResult result = slot.length() == 1 ? brewHistoryDeleteSlot(slot[0]) : BREW_SLOT_INVALID;
    switch (result) {
      case BREW_SLOT_SAVED:   respond(out, 200, "text/plain", "Slot " + slot + " deleted"); break;
      case BREW_SLOT_INVALID: respond(out, 400, "text/plain", "Unknown slot"); break;
      default:                respond(out, 500, "text/plain", "Could not delete the slot"); break;
    }
  }
  return true;
}
