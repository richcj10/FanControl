/**
 * @file web_server.h
 * @brief AsyncWebServer setup and REST API declaration.
 *
 * Call @ref webServerSetup() once during setup() after LittleFS has been
 * mounted.  The function registers all REST API routes and then adds the
 * LittleFS static-file catch-all, so API paths always take priority.
 *
 * ## REST API summary
 * | Method | Path                  | Body / Response                          |
 * |--------|-----------------------|------------------------------------------|
 * | GET    | /api/status           | JSON: IP, array of fan states            |
 * | GET    | /api/config           | JSON: MQTT settings + alarm thresholds   |
 * | POST   | /api/fan/pwm          | `{"fan":N,"pwm":0-255}`                  |
 * | POST   | /api/fan/mode         | `{"fan":N,"mode":"pwm"\|"rpm"}`          |
 * | POST   | /api/fan/target       | `{"fan":N,"rpm":NNNN}`                   |
 * | POST   | /api/config/mqtt      | MQTT credential fields                   |
 * | POST   | /api/config/alarms    | `{"alarms":[{"minRpm":N,"maxRpm":N}…]}`  |
 */

#pragma once
#include <ESPAsyncWebServer.h>

/**
 * @brief Register all HTTP routes and start the web server.
 *
 * Must be called after LittleFS.begin() and after ETH has been initialised
 * (though the server can start before Ethernet is fully up — requests will
 * simply not arrive until a link is established).
 *
 * @param server Reference to an AsyncWebServer instance (created in main.cpp).
 */
void webServerSetup(AsyncWebServer &server);
