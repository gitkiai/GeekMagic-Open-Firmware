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

#include "display/JpegDisplay.h"
#include "display/DisplayManager.h"
#include "config/ConfigManager.h"

#include <JPEGDEC.h>
#include <LittleFS.h>
#include <Arduino_GFX_Library.h>
#include <Logger.h>
#include <new>

static File s_jpegFile;
static bool s_jpegFileOpen = false;

static void* jpegOpen(const char* filename, int32_t* pSize) {
    s_jpegFile = LittleFS.open(filename, "r");
    if (!s_jpegFile) {
        return nullptr;
    }
    s_jpegFileOpen = true;
    *pSize = static_cast<int32_t>(s_jpegFile.size());
    return &s_jpegFile;
}

static void jpegClose(void* pHandle) {
    (void)pHandle;
    if (s_jpegFileOpen && s_jpegFile) {
        s_jpegFile.close();
    }
    s_jpegFileOpen = false;
}

static int32_t jpegRead(JPEGFILE* pFile, uint8_t* pBuf, int32_t iLen) {
    auto* f = reinterpret_cast<File*>(pFile->fHandle);
    if (f == nullptr || !(*f)) {
        return 0;
    }
    return static_cast<int32_t>(f->read(pBuf, static_cast<size_t>(iLen)));
}

static int32_t jpegSeek(JPEGFILE* pFile, int32_t iPosition) {
    auto* f = reinterpret_cast<File*>(pFile->fHandle);
    if (f == nullptr || !(*f)) {
        return 0;
    }
    f->seek(static_cast<uint32_t>(iPosition), SeekSet);
    return iPosition;
}

static int jpegDraw(JPEGDRAW* pDraw) {
    auto* gfx = DisplayManager::getGfx();
    if (gfx == nullptr) {
        return 0;
    }

    auto* tft = reinterpret_cast<Arduino_TFT*>(gfx);

    int16_t x = static_cast<int16_t>(pDraw->x);
    int16_t y = static_cast<int16_t>(pDraw->y);
    uint16_t w = static_cast<uint16_t>(pDraw->iWidth);
    uint16_t h = static_cast<uint16_t>(pDraw->iHeight);

    tft->startWrite();
    for (uint16_t row = 0; row < h; row++) {
        tft->writeAddrWindow(x, static_cast<int16_t>(y + row), w, 1);
        tft->writePixels(&pDraw->pPixels[row * w], static_cast<uint32_t>(w));
        yield();
    }
    tft->endWrite();

    return 1;
}

bool JpegDisplay::drawFromFile(const String& path) {
    String filePath = path;
    if (!filePath.startsWith("/")) {
        filePath = "/" + filePath;
    }

    if (!LittleFS.exists(filePath)) {
        Logger::error(("JPEG file not found: " + filePath).c_str(), "JpegDisplay");
        return false;
    }

    DisplayManager::stopGif();

    JPEGDEC* jpeg = new (std::nothrow) JPEGDEC();
    if (jpeg == nullptr) {
        Logger::error("JPEG: not enough memory", "JpegDisplay");
        return false;
    }

    if (jpeg->open(filePath.c_str(), jpegOpen, jpegClose, jpegRead, jpegSeek, jpegDraw) <= 0) {
        Logger::error(("Failed to open JPEG: " + filePath).c_str(), "JpegDisplay");
        delete jpeg;
        return false;
    }

    int imgW = jpeg->getWidth();
    int imgH = jpeg->getHeight();

    Logger::info(("JPEG: " + String(imgW) + "x" + String(imgH)).c_str(), "JpegDisplay");

    jpeg->setPixelType(RGB565_LITTLE_ENDIAN);
    bool ok = jpeg->decode(0, 0, 0) == 1;
    jpeg->close();
    delete jpeg;

    if (ok) {
        Logger::info("JPEG displayed", "JpegDisplay");
    } else {
        Logger::error("JPEG decode failed", "JpegDisplay");
    }

    return ok;
}
