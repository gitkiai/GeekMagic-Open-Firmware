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
#include <LittleFS.h>
#include <Logger.h>

#include "web/Webserver.h"
#include "web/HacsApi.h"
#include "display/DisplayManager.h"
#include "display/JpegDisplay.h"

static constexpr const char* IMAGE_DIR = "/image";

// Current device state
static int s_currentTheme = 0;
static String s_currentImage;

/**
 * @brief Ensure the /image directory exists on LittleFS
 */
static void ensureImageDir() {
    if (!LittleFS.exists(IMAGE_DIR)) {
        LittleFS.mkdir(IMAGE_DIR);
    }
}

/**
 * @brief Delete all files in the /image directory
 */
static void clearAllImages() {
    Dir dir = LittleFS.openDir(IMAGE_DIR);
    while (dir.next()) {
        String path = String(IMAGE_DIR) + "/" + dir.fileName();
        LittleFS.remove(path);
    }
    s_currentImage = "";
}

/**
 * @brief Handle GET /app.json - device state
 *
 * Returns: {"theme": N, "brt": N, "img": "/image/filename.jpg"}
 */
static void handleAppJson(Webserver* webserver) {
    String json = "{\"theme\":";
    json += String(s_currentTheme);
    json += ",\"brt\":";
    json += String(DisplayManager::getBrightness());
    if (s_currentImage.length() > 0) {
        json += ",\"img\":\"";
        json += s_currentImage;
        json += "\"";
    }
    json += "}";

    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Handle GET /space.json - storage info
 *
 * Returns: {"total": N, "free": N}
 */
static void handleSpaceJson(Webserver* webserver) {
    FSInfo fsInfo;
    LittleFS.info(fsInfo);

    String json = "{\"total\":";
    json += String(fsInfo.totalBytes);
    json += ",\"free\":";
    json += String(fsInfo.totalBytes - fsInfo.usedBytes);
    json += "}";

    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Handle GET /brt.json - brightness info
 *
 * Returns: {"brt": "N"} (note: string value, matching original firmware)
 */
static void handleBrtJson(Webserver* webserver) {
    String json = "{\"brt\":\"";
    json += String(DisplayManager::getBrightness());
    json += "\"}";

    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Handle GET /set?key=value - settings control
 *
 * Supported parameters:
 *   brt=N       Set brightness (0-100)
 *   theme=N     Set theme number
 *   img=PATH    Set displayed image
 *   clear=image Clear all images
 *   page=N      Navigate pages (1=next, -1=prev)
 *   enter=N     Enter button press
 *   reboot=1    Reboot device
 */
static void handleSet(Webserver* webserver) {
    ESP8266WebServer& server = webserver->raw();

    if (server.hasArg("brt")) {
        int brt = server.arg("brt").toInt();
        DisplayManager::setBrightness(brt);
        Logger::info(("HACS: brightness set to " + String(brt)).c_str(), "HacsApi");
        server.send(HTTP_CODE_OK, "text/plain", "OK");
        return;
    }

    if (server.hasArg("theme")) {
        s_currentTheme = server.arg("theme").toInt();
        Logger::info(("HACS: theme set to " + String(s_currentTheme)).c_str(), "HacsApi");
        server.send(HTTP_CODE_OK, "text/plain", "OK");
        return;
    }

    if (server.hasArg("img")) {
        String imgPath = server.arg("img");
        s_currentImage = imgPath;
        Logger::info(("HACS: display image " + imgPath).c_str(), "HacsApi");

        if (JpegDisplay::drawFromFile(imgPath)) {
            server.send(HTTP_CODE_OK, "text/plain", "OK");
        } else {
            server.send(HTTP_CODE_NOT_FOUND, "text/plain", "Image not found or decode failed");
        }
        return;
    }

    if (server.hasArg("clear")) {
        String what = server.arg("clear");
        if (what == "image") {
            clearAllImages();
            DisplayManager::clearScreen();
            Logger::info("HACS: cleared all images", "HacsApi");
        }
        server.send(HTTP_CODE_OK, "text/plain", "OK");
        return;
    }

    if (server.hasArg("page")) {
        // Navigation: not directly applicable to open firmware, but acknowledge
        Logger::info(("HACS: page navigate " + server.arg("page")).c_str(), "HacsApi");
        server.send(HTTP_CODE_OK, "text/plain", "OK");
        return;
    }

    if (server.hasArg("enter")) {
        Logger::info("HACS: enter button", "HacsApi");
        server.send(HTTP_CODE_OK, "text/plain", "OK");
        return;
    }

    if (server.hasArg("reboot")) {
        Logger::info("HACS: reboot requested", "HacsApi");
        server.send(HTTP_CODE_OK, "text/plain", "OK");
        delay(500);
        ESP.restart();
        return;
    }

    server.send(HTTP_CODE_BAD_REQUEST, "text/plain", "Unknown parameter");
}

// Upload state for multipart file handling
static File s_uploadFile;
static String s_uploadDir;
static bool s_uploadActive = false;

/**
 * @brief Handle POST /doUpload - multipart file upload
 *
 * Query parameter: dir=/image/ (target directory)
 * Multipart form field: file (the image data)
 */
static void handleDoUploadData(Webserver* webserver) {
    ESP8266WebServer& server = webserver->raw();
    HTTPUpload& upload = server.upload();

    if (upload.status == UPLOAD_FILE_START) {
        // Get target directory from query parameter
        s_uploadDir = IMAGE_DIR;
        if (server.hasArg("dir")) {
            s_uploadDir = server.arg("dir");
            // Normalize: remove trailing slash
            if (s_uploadDir.endsWith("/") && s_uploadDir.length() > 1) {
                s_uploadDir = s_uploadDir.substring(0, s_uploadDir.length() - 1);
            }
        }

        ensureImageDir();

        String filename = upload.filename;
        if (filename.isEmpty()) {
            Logger::error("HACS: upload with empty filename", "HacsApi");
            s_uploadActive = false;
            return;
        }

        String path = s_uploadDir + "/" + filename;
        Logger::info(("HACS: upload start -> " + path).c_str(), "HacsApi");

        s_uploadFile = LittleFS.open(path, "w");
        if (!s_uploadFile) {
            Logger::error(("HACS: failed to open for write: " + path).c_str(), "HacsApi");
            s_uploadActive = false;
            return;
        }
        s_uploadActive = true;

    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (s_uploadActive && s_uploadFile) {
            s_uploadFile.write(upload.buf, upload.currentSize);
            yield();
        }

    } else if (upload.status == UPLOAD_FILE_END) {
        if (s_uploadActive && s_uploadFile) {
            s_uploadFile.close();
            Logger::info(("HACS: upload complete, " + String(upload.totalSize) + " bytes").c_str(), "HacsApi");
        }
        s_uploadActive = false;

    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        if (s_uploadActive && s_uploadFile) {
            s_uploadFile.close();
        }
        s_uploadActive = false;
        Logger::warn("HACS: upload aborted", "HacsApi");
    }
}

static void handleDoUploadFinished(Webserver* webserver) {
    webserver->raw().send(HTTP_CODE_OK, "text/plain", "OK");
}

/**
 * @brief Handle GET /delete?file=PATH - delete a file
 */
static void handleDeleteFile(Webserver* webserver) {
    ESP8266WebServer& server = webserver->raw();

    if (!server.hasArg("file")) {
        server.send(HTTP_CODE_BAD_REQUEST, "text/plain", "Missing file parameter");
        return;
    }

    String path = server.arg("file");
    if (LittleFS.exists(path)) {
        LittleFS.remove(path);
        Logger::info(("HACS: deleted " + path).c_str(), "HacsApi");
        server.send(HTTP_CODE_OK, "text/plain", "OK");
    } else {
        server.send(HTTP_CODE_NOT_FOUND, "text/plain", "File not found");
    }
}

/**
 * @brief Register all HACS compatibility endpoints
 *
 * These endpoints emulate the original GeekMagic firmware API that the
 * geekmagic-hacs Home Assistant integration (https://github.com/adrienbrault/geekmagic-hacs)
 * expects. This allows the open firmware to work as a drop-in replacement.
 *
 * Endpoints:
 *   GET  /app.json              Device state (theme, brightness, current image)
 *   GET  /space.json            Storage info (total/free bytes)
 *   GET  /brt.json              Brightness level
 *   GET  /set?key=value         Control brightness, theme, image, reboot, etc.
 *   POST /doUpload?dir=/image/  Upload image files (multipart)
 *   GET  /delete?file=PATH      Delete a file
 */
void registerHacsEndpoints(Webserver* webserver) {
    Logger::info("Registering HACS compatibility endpoints", "HacsApi");

    ensureImageDir();

    webserver->raw().on("/app.json", HTTP_GET, [webserver]() { handleAppJson(webserver); });

    webserver->raw().on("/space.json", HTTP_GET, [webserver]() { handleSpaceJson(webserver); });

    webserver->raw().on("/brt.json", HTTP_GET, [webserver]() { handleBrtJson(webserver); });

    webserver->raw().on("/set", HTTP_GET, [webserver]() { handleSet(webserver); });

    webserver->raw().on(
        "/doUpload", HTTP_POST, [webserver]() { handleDoUploadFinished(webserver); },
        [webserver]() { handleDoUploadData(webserver); });

    webserver->raw().on("/delete", HTTP_GET, [webserver]() { handleDeleteFile(webserver); });

    Logger::info("HACS compatibility endpoints registered", "HacsApi");
}
