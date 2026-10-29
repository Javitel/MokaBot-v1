#pragma once

// Data endpoints used by the brew and settings pages (/brewHistory,
// /settingsInfo, /set, /brewSave...), independent of the transport: the web
// server (WiFi) and the Bluetooth link (Waveshare board) answer them through
// apiHandle(), so both behave the same.

#include <Arduino.h>

// Query arguments of a request.
class ApiArgs {
public:
  virtual ~ApiArgs() {}
  virtual bool has(const char *name) const = 0;
  virtual String get(const char *name) const = 0;
};

// Arguments parsed from a "a=1&b=x%20y" query string.
class ApiQueryArgs : public ApiArgs {
public:
  explicit ApiQueryArgs(const String &query) : query(query) {}
  bool has(const char *name) const override;
  String get(const char *name) const override;
private:
  bool find(const char *name, String *value) const;
  String query;
};

struct ApiResponse {
  int status = 200;
  const char *contentType = "text/plain";
  String body;
};

// Paths apiHandle() answers, for registering them with the web server.
extern const char *const API_GET_PATHS[];
extern const char *const API_POST_PATHS[];

// Handles a request to one of the data endpoints. Returns false when path is
// not one of them (the caller answers 404). body is the request body (only
// /brewImport uses it).
bool apiHandle(const String &method, const String &path, const ApiArgs &args,
               const String &body, ApiResponse &out);
