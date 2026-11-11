#pragma once

// HTTPS for the web pages (https://<name>.local, port 443), for browsers
// that insist on HTTPS. A small TLS front end: it accepts one HTTPS connection
// at a time, decrypts it and passes the bytes to the HTTP server on port 80
// (over the loopback interface), so every page and endpoint works the same
// over both. The certificate is self-signed (a .local device cannot get a
// trusted one): the browser warns once and remembers the exception. It is
// created on the device at the first start (its private key never leaves it),
// kept in Preferences, and made again when the device name changes.

// Starts listening on port 443 (WiFi connected, HTTP server running).
void httpsStart(void);
// Stops listening and waits until the port is released.
void httpsStop(void);
// True while the HTTPS front end is running.
bool httpsRunning(void);
