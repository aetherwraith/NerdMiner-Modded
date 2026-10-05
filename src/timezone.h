#ifndef TIMEZONE_H
#define TIMEZONE_H

#include <Arduino.h>

struct TimeZoneInfo
{
  const char *id;     // Canonical IANA / key, e.g. "Europe/London"
  const char *label;  // Searchable display label
  const char *posix;  // POSIX TZ string with automatic DST rules
};

extern const TimeZoneInfo kTimeZones[];
extern const int kTimeZoneCount;

// Resolves a stored timezone setting (ID, POSIX string, or legacy numeric offset) to a valid POSIX TZ string.
String getPosixTz(const String &tz);

// Searches for a timezone by ID or POSIX string. Returns nullptr if not found.
const TimeZoneInfo* findTimeZone(const String &tz);

#endif // TIMEZONE_H
