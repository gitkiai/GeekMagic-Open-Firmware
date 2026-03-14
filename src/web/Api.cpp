// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * GeekMagic Open Firmware
 * Copyright (C) 2026 Times-Z
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <Arduino.h>
#include <Logger.h>
#include <ArduinoJson.h>
#include <Updater.h>

#include "web/Webserver.h"
#include "web/Api.h"
#include "display/DisplayManager.h"

#include "config/ConfigManager.h"
#include "wireless/WiFiManager.h"
#include "ntp/NTPClient.h"

extern ConfigManager configManager;
extern WiFiManager* wifiManager;
extern NTPClient* ntpClient;

static bool otaError = false;
static size_t otaSize = 0;
static String otaStatus;
static volatile bool otaInProgress = false;
static volatile bool otaCancelRequested = false;
static size_t otaTotal = 0;

static constexpr int OTA_TEXT_X_OFFSET = 50;
static constexpr int OTA_TEXT_Y_OFFSET = 80;
static constexpr int OTA_LOADING_Y_OFFSET = 110;

static void otaHandleStart(HTTPUpload& upload, int mode);
static void otaHandleWrite(HTTPUpload& upload);
static void otaHandleEnd(HTTPUpload& upload, int mode);
static void otaHandleAborted(HTTPUpload& upload);
static constexpr int WIFI_CONNECT_TIMEOUT_MS = 15000;
static constexpr size_t NTP_CONFIG_DOC_SIZE = 512;
static constexpr int BEARER_LEN = 7;

/**
 * @brief Register API endpoints for the webserver
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void registerApiEndpoints(Webserver* webserver) {
    Logger::info("Registering API endpoints", "API");

    // @openapi {get} /wifi/scan version=v1 group=WiFi summary="Scan available WiFi networks" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/wifi/scan", HTTP_GET, [webserver]() { handleWifiScan(webserver); });

    // @openapi {post} /wifi/connect version=v1 group=WiFi summary="Connect to a WiFi network" requiresAuth=true
    // requestBody=application/json requestBodySchema=ssid:string,password:string
    // example={"ssid":"MyNetwork","password":"password123"} responses=200:application/json,400:application/json,401:application/json
    webserver->raw().on("/api/v1/wifi/connect", HTTP_POST, [webserver]() { handleWifiConnect(webserver); });

    // @openapi {get} /wifi/status version=v1 group=WiFi summary="Get WiFi connection status" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/wifi/status", HTTP_GET, [webserver]() { handleWifiStatus(webserver); });

    // @openapi {post} /ntp/sync version=v1 group=NTP summary="Trigger NTP sync" requiresAuth=true responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/ntp/sync", HTTP_POST, [webserver]() { handleNtpSync(webserver); });

    // @openapi {get} /ntp/status version=v1 group=NTP summary="Get NTP status" requiresAuth=true responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/ntp/status", HTTP_GET, [webserver]() { handleNtpStatus(webserver); });

    // @openapi {get} /ntp/config version=v1 group=NTP summary="Get NTP configuration" requiresAuth=true responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/ntp/config", HTTP_GET, [webserver]() { handleNtpConfigGet(webserver); });

    // @openapi {post} /ntp/config version=v1 group=NTP summary="Set NTP configuration" requiresAuth=true requestBody=application/json
    // requestBodySchema=ntp_server:string example={"ntp_server":"pool.ntp.org"}
    // responses=200:application/json,400:application/json,401:application/json
    webserver->raw().on("/api/v1/ntp/config", HTTP_POST, [webserver]() { handleNtpConfigSet(webserver); });

    // @openapi {post} /reboot version=v1 group=System summary="Reboot the device" requiresAuth=true responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/reboot", HTTP_POST, [webserver]() { handleReboot(webserver); });

    // @openapi {post} /ota/fw version=v1 group=OTA summary="Upload firmware (OTA)" requiresAuth=true requestBody=multipart/form-data
    // responses=200:application/json,401:application/json
    webserver->raw().on(
        "/api/v1/ota/fw", HTTP_POST, [webserver]() { handleOtaFinished(webserver); },
        [webserver]() { handleOtaUpload(webserver, U_FLASH); });

    // @openapi {post} /ota/fs version=v1 group=OTA summary="Upload filesystem (OTA)" requiresAuth=true requestBody=multipart/form-data
    // responses=200:application/json,401:application/json
    webserver->raw().on(
        "/api/v1/ota/fs", HTTP_POST, [webserver]() { handleOtaFinished(webserver); },
        [webserver]() { handleOtaUpload(webserver, U_FS); });

    // @openapi {get} /ota/status version=v1 group=OTA summary="Get OTA status" requiresAuth=true responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/ota/status", HTTP_GET, [webserver]() { handleOtaStatus(webserver); });

    // @openapi {post} /ota/cancel version=v1 group=OTA summary="Cancel OTA" requiresAuth=true responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/ota/cancel", HTTP_POST, [webserver]() { handleOtaCancel(webserver); });

    // @openapi {get} /token/check version=v1 group=Authentication summary="Check bearer token validity"
    // requiresAuth=true responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/token/check", HTTP_GET, [webserver]() { handleTokenCheck(webserver); });

    // @openapi {post} /token/save version=v1 group=Authentication summary="Save a new bearer token" requiresAuth=true
    // requestBody=application/json requestBodySchema=token:string example={"token":"your_secure_token_value"}
    // responses=200:application/json,401:application/json,400:application/json
    webserver->raw().on("/api/v1/token/save", HTTP_POST, [webserver]() { handleTokenSave(webserver); });

    webserver->raw().onNotFound([webserver]() {
        if (webserver->raw().method() == HTTP_OPTIONS) {
            setCorsHeaders(webserver);
            webserver->raw().send(HTTP_CODE_OK);
        }
    });
}

/**
 * @brief Set CORS headers for API responses
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void setCorsHeaders(Webserver* webserver) {
    webserver->raw().sendHeader("Access-Control-Allow-Origin", "*");
    webserver->raw().sendHeader("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    webserver->raw().sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
    webserver->raw().sendHeader("Access-Control-Max-Age", "3600");
}

/**
 * @brief Validate bearer token from Authorization header
 * @param webserver Pointer to the Webserver instance
 *
 * @return true if token is valid false otherwise
 */
static auto validateBearerToken(Webserver* webserver) -> bool {
    if (!webserver->raw().hasHeader("Authorization")) {
        return false;
    }

    String authHeader = webserver->raw().header("Authorization");

    if (!authHeader.startsWith("Bearer ")) {
        return false;
    }

    String providedToken = authHeader.substring(BEARER_LEN);
    String storedToken = configManager.getApiToken();

    if (storedToken.length() == 0) {
        return false;
    }

    return providedToken.equals(storedToken);
}

/**
 * @brief Enforce bearer token check and send 401 response if invalid
 * @param webserver Pointer to the Webserver instance
 *
 * @return true if token is valid false otherwise
 */
static auto requireBearerToken(Webserver* webserver) -> bool {
    // Skip token check in AP mode (captive portal has no token)
    if (wifiManager != nullptr && wifiManager->isApMode()) {
        return true;
    }

    // Skip token check if no token has been configured yet
    String storedToken = configManager.getApiToken();
    if (storedToken.length() == 0) {
        return true;
    }

    if (validateBearerToken(webserver)) {
        return true;
    }

    JsonDocument doc;
    doc["status"] = "error";
    doc["message"] = "Invalid or missing token";

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_UNAUTHORIZED, "application/json", json);

    Logger::warn(
        ("Unauthorized request from " + webserver->raw().client().remoteIP().toString()).c_str(), "API");

    return false;
}

/**
 * @brief Check if bearer token is valid
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void handleTokenCheck(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "Token is valid";

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Save a new bearer token
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void handleTokenSave(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    if (!webserver->raw().hasArg("plain") || webserver->raw().arg("plain").length() == 0) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Missing JSON body";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    String body = webserver->raw().arg("plain");
    JsonDocument ddoc;
    DeserializationError err = deserializeJson(ddoc, body);

    if (err) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Invalid JSON";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        Logger::warn("Attempt to save API token with invalid JSON", "API");

        return;
    }

    const char* newToken = ddoc["token"] | "";

    if (strlen(newToken) == 0) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "token field is required";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);

        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        Logger::warn("Attempt to save empty API token", "API");
        return;
    }

    configManager.setApiToken(newToken);
    configManager.save();

    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "Token saved successfully";

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);

    Logger::info("API token updated", "API");
}

/**
 * @brief OTA status endpoint
 */
void handleOtaStatus(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    doc["inProgress"] = otaInProgress;
    doc["bytesWritten"] = otaSize;
    doc["totalBytes"] = otaTotal;
    doc["error"] = otaError;
    doc["message"] = otaStatus;

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief OTA cancel endpoint
 */
void handleOtaCancel(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    otaCancelRequested = true;
    otaStatus = "Cancel requested";

    JsonDocument doc;
    doc["status"] = "cancelling";
    doc["message"] = "Cancel request received";

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Reboot endpoint
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void handleReboot(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    int constexpr rebootDelayMs = 1000;

    doc["status"] = "rebooting";
    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);

    delay(rebootDelayMs);
    ESP.restart();  // NOLINT(readability-static-accessed-through-instance)
}

/**
 * @brief Manual NTP sync trigger endpoint
 */
void handleNtpSync(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;

    if (ntpClient == nullptr) {
        doc["status"] = "error";
        doc["message"] = "NTP client not initialized";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", json);

        return;
    }

    bool syncOk = ntpClient->syncNow();
    doc["status"] = syncOk ? "ok" : "error";
    doc["lastStatus"] = ntpClient->lastStatus();
    doc["lastSyncTime"] = ntpClient->lastSyncTime();

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Return NTP status
 */
void handleNtpStatus(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;

    if (ntpClient == nullptr) {
        doc["status"] = "error";
        doc["message"] = "NTP client not initialized";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", json);
        return;
    }

    doc["lastOk"] = ntpClient->lastSyncOk();
    doc["lastStatus"] = ntpClient->lastStatus();
    doc["lastSyncTime"] = ntpClient->lastSyncTime();

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Get NTP configuration
 */
void handleNtpConfigGet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    doc["ntp_server"] = configManager.getNtpServer();

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Set NTP configuration
 */
void handleNtpConfigSet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    if (!webserver->raw().hasArg("plain") || webserver->raw().arg("plain").length() == 0) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Missing JSON body";

        String json;

        serializeJson(doc, json);
        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    String body = webserver->raw().arg("plain");
    JsonDocument ddoc;
    DeserializationError err = deserializeJson(ddoc, body);

    if (err) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Invalid JSON";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    const char* server = ddoc["ntp_server"] | "";

    if (strlen(server) == 0) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "ntp_server missing";

        String json;
        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);

        return;
    }

    configManager.setNtpServer(server);

    if (!configManager.save()) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Failed to save config";

        String json;

        serializeJson(doc, json);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", json);

        return;
    }

    // optionally trigger a sync
    if (ntpClient != nullptr) {
        ntpClient->syncNow();
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["ntp_server"] = server;
    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Handle OTA upload
 * @param webserver Pointer to the Webserver instance
 * @param mode Update mode U_FLASH U_FS
 *
 * @return void
 */
void handleOtaUpload(Webserver* webserver, int mode) {
    HTTPUpload& upload = webserver->raw().upload();

    if (upload.status == UPLOAD_FILE_START && !validateBearerToken(webserver)) {
        otaError = true;
        otaStatus = "Unauthorized";

        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Invalid or missing token";

        String json;

        serializeJson(doc, json);
        setCorsHeaders(webserver);

        webserver->raw().send(HTTP_CODE_UNAUTHORIZED, "application/json", json);

        return;
    }

    switch (upload.status) {
        case UPLOAD_FILE_START:
            otaHandleStart(upload, mode);
            break;
        case UPLOAD_FILE_WRITE:
            otaHandleWrite(upload);
            break;
        case UPLOAD_FILE_END:
            otaHandleEnd(upload, mode);
            break;
        case UPLOAD_FILE_ABORTED:
            otaHandleAborted(upload);
            break;
        default:
            break;
    }
}

/**
 * @brief Handle OTA finished
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void handleOtaFinished(Webserver* webserver) {
    if (!validateBearerToken(webserver)) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Invalid or missing token";

        String json;
        serializeJson(doc, json);
        setCorsHeaders(webserver);

        webserver->raw().send(HTTP_CODE_UNAUTHORIZED, "application/json", json);

        return;
    }

    JsonDocument doc;
    int constexpr rebootDelayMs = 5000;

    doc["status"] = "Upload successful";
    doc["message"] = otaStatus;

    if (otaError) {
        doc["status"] = "Error";
    }

    otaInProgress = false;
    otaCancelRequested = false;

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);

    if (!otaError) {
        delay(rebootDelayMs);
        ESP.restart();  // NOLINT(readability-static-accessed-through-instance)
    }
}

/**
 * @brief Handle WiFi scan
 */
void handleWifiScan(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    JsonArray networks = doc["networks"].to<JsonArray>();

    if (wifiManager != nullptr) {
        WiFiManager::scanNetworks(networks);
    }

    String out;
    serializeJson(doc["networks"], out);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", out);
}

/**
 * @brief Handle WiFi connect request
 */
void handleWifiConnect(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    String body = webserver->raw().arg("plain");
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);

    if (err) {
        JsonDocument resp;

        resp["status"] = "error";
        resp["message"] = "invalid json";

        String jsonOut;
        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", jsonOut);

        return;
    }

    const char* ssid = doc["ssid"] | "";
    const char* password = doc["password"] | "";

    if (strlen(ssid) == 0) {
        JsonDocument resp;

        resp["status"] = "error";
        resp["message"] = "missing ssid";

        String jsonOut;

        serializeJson(resp, jsonOut);

        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", jsonOut);

        return;
    }

    bool connectOk = false;
    if (wifiManager != nullptr) {
        connectOk = wifiManager->connectToNetwork(ssid, password, WIFI_CONNECT_TIMEOUT_MS);
    }

    JsonDocument resp;

    resp["status"] = connectOk ? "connected" : "error";
    resp["ssid"] = ssid;

    if (connectOk) {
        resp["ip"] = wifiManager->getIP().toString();
        configManager.setWiFi(ssid, password);
        configManager.save();
    }

    if (!connectOk) {
        resp["message"] = "failed to connect";
    }

    String jsonOut;
    serializeJson(resp, jsonOut);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", jsonOut);
}

/**
 * @brief WiFi status
 */
void handleWifiStatus(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument resp;

    bool connected = (wifiManager != nullptr) && WiFiManager::isConnected();

    resp["connected"] = connected;
    resp["ssid"] = connected ? WiFiManager::getConnectedSSID() : "";
    resp["ip"] = connected ? wifiManager->getIP().toString() : "";

    String jsonOut;
    serializeJson(resp, jsonOut);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", jsonOut);
}

/**
 * @brief Handle OTA start
 *
 * @param upload Reference to the HTTPUpload object
 * @param mode Update mode U_FLASH or U_FS
 *
 * @return void
 */
static void otaHandleStart(HTTPUpload& upload, int mode) {
    Logger::info((String("OTA start: ") + upload.filename).c_str(), "API::OTA");

    otaError = false;
    otaSize = 0;
    otaStatus = "";
    otaInProgress = true;
    otaCancelRequested = false;
    otaTotal = static_cast<size_t>(upload.contentLength);

    DisplayManager::clearScreen();
    DisplayManager::drawTextWrapped(OTA_TEXT_X_OFFSET, OTA_TEXT_Y_OFFSET, "Uploading...", 2, LCD_WHITE, LCD_BLACK,
                                    true);
    DisplayManager::drawLoadingBar(0.0F, OTA_LOADING_Y_OFFSET);

    int constexpr security_space = 0x1000;
    u_int constexpr bin_mask = 0xFFFFF000;

    FSInfo fs_info;
    LittleFS.info(fs_info);
    size_t fsSize = fs_info.totalBytes;
    size_t maxSketchSpace =
        (ESP.getFreeSketchSpace() - security_space) &  // NOLINT(readability-static-accessed-through-instance)
        bin_mask;
    size_t place = (mode == U_FS) ? fsSize : maxSketchSpace;

    if (!Update.begin(place, mode)) {
        otaError = true;
        otaStatus = Update.getError() ? String(Update.getError()) : "unknown error";
        Logger::error((String("Update.begin failed: ") + otaStatus).c_str(), "API::OTA");
    }
}

/**
 * @brief Handle OTA write
 *
 * @param upload Reference to the HTTPUpload object
 *
 * @return void
 */
static void otaHandleWrite(HTTPUpload& upload) {
    if (!otaError) {
        if (otaCancelRequested) {
            Update.end();
            otaError = true;
            otaStatus = "Update canceled";
            otaInProgress = false;
            Logger::warn("OTA canceled by user", "API::OTA");

            DisplayManager::drawTextWrapped(OTA_TEXT_X_OFFSET, OTA_TEXT_Y_OFFSET, "Canceled", 2, LCD_WHITE, LCD_BLACK,
                                            true);
            DisplayManager::drawLoadingBar(0.0F, OTA_LOADING_Y_OFFSET);

            return;
        }

        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            otaError = true;
            otaStatus = Update.getError() ? String(Update.getError()) : "unknown error";
            Logger::error((String("Write failed: ") + otaStatus).c_str(), "API::OTA");
        }

        otaSize += upload.currentSize;

        float progress = 0.0F;
        if (otaTotal > 0) {
            progress = static_cast<float>(otaSize) / static_cast<float>(otaTotal);
        }

        DisplayManager::drawLoadingBar(progress, OTA_LOADING_Y_OFFSET);
    }
}

/**
 * @brief Handle OTA end
 *
 * @param upload Reference to the HTTPUpload object
 * @param mode Update mode U_FLASH or U_FS
 *
 * @return void
 */
static void otaHandleEnd(HTTPUpload& /*upload*/, int mode) {
    if (!otaError) {
        if (Update.end(true)) {
            if (mode == U_FS) {
                Logger::info("OTA FS update complete, mounting file system...", "API::OTA");
                LittleFS.begin();
            }

            otaStatus = String("Update OK (") + String(otaSize) + " bytes)";
            Logger::info(otaStatus.c_str(), "API::OTA");

            DisplayManager::drawLoadingBar(1.0F, OTA_LOADING_Y_OFFSET);
            DisplayManager::drawTextWrapped(OTA_TEXT_X_OFFSET, OTA_TEXT_Y_OFFSET, "Success!", 2, LCD_WHITE, LCD_BLACK,
                                            true);
        } else {
            otaError = true;
            otaStatus = Update.getError() ? String(Update.getError()) : "unknown error";
        }
    }
}

/**
 * @brief Handle OTA aborted
 *
 * @param upload Reference to the HTTPUpload object
 *
 * @return void
 */
static void otaHandleAborted(HTTPUpload& /*upload*/) {
    Update.end();
    otaError = true;
    otaStatus = "Update aborted";
    otaInProgress = false;
    otaCancelRequested = false;

    DisplayManager::drawTextWrapped(OTA_TEXT_X_OFFSET, OTA_TEXT_Y_OFFSET, "Aborted", 2, LCD_WHITE, LCD_BLACK, true);
    DisplayManager::drawLoadingBar(0.0F, OTA_LOADING_Y_OFFSET);
}
