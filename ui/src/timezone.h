// Time zones without a tz database: a standard UTC offset plus a daylight
// saving rule family. Pure civil-date arithmetic: no <ctime>, no clock.
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

enum class Dst { None, Eu, Us, Au, Nz };

struct Zone {
  const char* id;
  const char* label;
  int stdOffsetMin;  // standard (winter in the north) offset from UTC, minutes
  Dst dst;
};

// The supported zones, in a stable display order.
const std::vector<Zone>& zones();

// nullptr for an unknown id.
const Zone* find_zone(const std::string& id);

// Offset from UTC in seconds that applies at the UTC instant `utc`, daylight
// saving included. Dates before 2000 use the same current rules.
int offset_seconds(const Zone& zone, int64_t utc);

// As above by id; an unknown id gives 0.
int offset_seconds(const std::string& zoneId, int64_t utc);
