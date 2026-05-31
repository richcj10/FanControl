/**
 * @file web_server.h
 * @brief AsyncWebServer setup, REST API, and web OTA declaration.
 *
 * Call @ref webServerSetup() once during @c setup() after LittleFS has been
 * mounted and @ref configLoad() has run.  API routes are registered before the
 * LittleFS static-file catch-all so /api/* paths always take priority.
 *
 * ## REST API
 * | Method | Path                    | Body / Response                                         |
 * |--------|-------------------------|---------------------------------------------------------|
 * | GET    | /api/status             | `{ip, mqttOnline, espTempC, fans[{rpm,pwm,disabled,…}]}`|
 * | GET    | /api/config             | `{hostname, mqttHost, …, otaWebEnabled, alarms[]}`      |
 * | POST   | /api/config/general     | `{"hostname":"…"}` — reboot to apply                   |
 * | POST   | /api/config/mqtt        | MQTT broker credentials + prefix                        |
 * | POST   | /api/config/alarms      | `{"alarms":[{"minRpm":N,"maxRpm":N}…]}`                 |
 * | POST   | /api/config/ota         | `{otaWebEnabled, otaArduinoEnabled, otaPassword}`        |
 * | GET    | /api/fan/defaults       | Per-fan boot defaults (pwm, mode, minDrive, …)          |
 * | POST   | /api/fan/defaults       | Update + persist fan boot defaults                      |
 * | POST   | /api/settings/upload    | Full settings.json body — merges all known keys         |
 * | GET    | /api/reboot             | —                                                       |
 * | POST   | /api/reboot             | Reboot the ESP32                                        |
 * | GET    | /update                 | OTA firmware upload page (only if otaWebEnabled)        |
 * | POST   | /update                 | OTA firmware flash (multipart/form-data)                |
 * | GET    | /                       | Serves LittleFS static files (index.html, app.js, …)   |
 */

#pragma once
#include <ESPAsyncWebServer.h>

/**
 * @brief Register all HTTP routes and start the web server.
 *
 * Must be called after LittleFS.begin() and configLoad().  The server
 * can start before Ethernet is fully up; requests will simply not arrive
 * until a link is established.
 *
 * @param server Reference to the AsyncWebServer instance (port 80, created in main.cpp).
 */
void webServerSetup(AsyncWebServer &server);

/** @brief Call from loop() — reserved for future per-loop web server tasks. */
void webServerLoop();
