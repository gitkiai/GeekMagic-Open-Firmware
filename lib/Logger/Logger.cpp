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

#include <ctime>
#include "Logger.h"

void Logger::log(LogLevel level, const char* message, const char* className) {
    char entry[128];
    char timeBuf[12];
    std::time_t t = std::time(nullptr);
    std::tm* now = std::localtime(&t);
    snprintf(timeBuf, sizeof(timeBuf), "[%02d:%02d:%02d]", now->tm_hour, now->tm_min, now->tm_sec);

    const char* classStr = (className != nullptr && className[0] != '\0') ? className : "Global";

    snprintf(entry, sizeof(entry), "%s(%s)::%s: %s", timeBuf, levelToString(level), classStr, message);

    Serial.println(entry);
}

void Logger::debug(const char* message, const char* className) { log(LOG_DEBUG, message, className); }
void Logger::info(const char* message, const char* className) { log(LOG_INFO, message, className); }
void Logger::warn(const char* message, const char* className) { log(LOG_WARN, message, className); }
void Logger::error(const char* message, const char* className) { log(LOG_ERROR, message, className); }

const char* Logger::levelToString(LogLevel level) {
    switch (level) {
        case LOG_DEBUG:
            return "DEBUG";
        case LOG_INFO:
            return "INFO";
        case LOG_WARN:
            return "WARN";
        case LOG_ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}
