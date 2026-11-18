#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// ============================================================================
// ESP32-C3 ESEMÉNY- ÉS HIBANAPLÓ (100 BEJEGYZÉSES KÖRKÖRÖS PUFFER RAM-BAN)
// ============================================================================
// - Dinamikus memóriafoglalás nélkül (nincs String / heap allokáció).
// - Nem blokkoló NTP + millis()/1000 időbélyeg.
// ============================================================================

enum LogLevel : uint8_t {
    LOG_INFO  = 0,
    LOG_WARN  = 1,
    LOG_ERROR = 2
};

struct LogEntry {
    uint32_t id;
    uint32_t uptimeSec;
    char     timestamp[20];
    uint8_t  level;
    char     code[16];
    char     message[48];
};

class Logger {
public:
    static constexpr size_t CAPACITY = 100;

    Logger() {
        memset(_logs, 0, sizeof(_logs));
    }

    void addEntry(uint8_t level, const char* code, const char* message, LogEntry* outEntry = nullptr) {
        LogEntry entry{};
        entry.id        = _nextId++;
        entry.uptimeSec = millis() / 1000UL;
        entry.level     = (level <= LOG_ERROR) ? level : LOG_INFO;

        formatTimestamp(entry.uptimeSec, entry.timestamp, sizeof(entry.timestamp));

        if (code) {
            strncpy(entry.code, code, sizeof(entry.code) - 1);
            entry.code[sizeof(entry.code) - 1] = '\0';
        } else {
            entry.code[0] = '\0';
        }

        if (message) {
            strncpy(entry.message, message, sizeof(entry.message) - 1);
            entry.message[sizeof(entry.message) - 1] = '\0';
        } else {
            entry.message[0] = '\0';
        }

        _logs[_head] = entry;
        _head = (_head + 1) % CAPACITY;
        if (_count < CAPACITY) {
            _count++;
        }

        if (outEntry) {
            *outEntry = entry;
        }
    }

    void clear() {
        _head  = 0;
        _count = 0;
        memset(_logs, 0, sizeof(_logs));
        // Az id számláló (_nextId) NEM nullázódik!
    }

    size_t count() const {
        return _count;
    }

    uint32_t nextId() const {
        return _nextId;
    }

    // offsetFromNewest = 0 a legfrissebb bejegyzés
    bool getNewest(size_t offsetFromNewest, LogEntry& out) const {
        if (offsetFromNewest >= _count) {
            return false;
        }
        size_t idx = (_head + CAPACITY - 1 - offsetFromNewest) % CAPACITY;
        out = _logs[idx];
        return true;
    }

    static const char* levelToStr(uint8_t level) {
        switch (level) {
            case LOG_WARN:  return "WARN";
            case LOG_ERROR: return "ERROR";
            case LOG_INFO:
            default:        return "INFO";
        }
    }

    static char levelToShortChar(uint8_t level) {
        switch (level) {
            case LOG_WARN:  return 'W';
            case LOG_ERROR: return 'E';
            case LOG_INFO:
            default:        return 'I';
        }
    }

private:
    LogEntry _logs[CAPACITY];
    size_t   _head   = 0;
    size_t   _count  = 0;
    uint32_t _nextId = 1;

    static void formatTimestamp(uint32_t uptimeSec, char* buf, size_t bufLen) {
        if (bufLen == 0) return;

        struct tm timeinfo{};
        if (getLocalTime(&timeinfo, 0) && (timeinfo.tm_year + 1900) >= 2024) {
            snprintf(buf, bufLen, "%04d-%02d-%02d %02d:%02d:%02d",
                     timeinfo.tm_year + 1900,
                     timeinfo.tm_mon + 1,
                     timeinfo.tm_mday,
                     timeinfo.tm_hour,
                     timeinfo.tm_min,
                     timeinfo.tm_sec);
        } else {
            snprintf(buf, bufLen, "UP %lus", static_cast<unsigned long>(uptimeSec));
        }
    }
};

extern Logger logger;
void logEvent(uint8_t level, const char* code, const char* message);
