#include "timezone.h"
#include <ctype.h>

const TimeZoneInfo kTimeZones[] = {
  // UTC
  {"UTC0", "UTC - Coordinated Universal Time", "UTC0"},

  // United Kingdom & Ireland
  {"Europe/London", "London, UK / Dublin (GMT/BST with DST)", "GMT0BST,M3.5.0/1,M10.5.0"},
  {"Atlantic/Reykjavik", "Reykjavik, Iceland (GMT, no DST)", "GMT0"},

  // Western & Central Europe
  {"Europe/Paris", "Paris, France / Amsterdam / Brussels (CET/CEST)", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/Berlin", "Berlin, Germany / Frankfurt / Munich (CET/CEST)", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/Rome", "Rome, Italy / Milan (CET/CEST)", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/Madrid", "Madrid, Spain / Barcelona (CET/CEST)", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/Zurich", "Zurich, Switzerland / Vienna (CET/CEST)", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/Stockholm", "Stockholm, Sweden / Oslo / Copenhagen (CET/CEST)", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/Warsaw", "Warsaw, Poland / Prague / Budapest (CET/CEST)", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/Lisbon", "Lisbon, Portugal (WET/WEST with DST)", "WET0WEST,M3.5.0/1,M10.5.0"},

  // Eastern Europe
  {"Europe/Athens", "Athens, Greece / Bucharest (EET/EEST)", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
  {"Europe/Helsinki", "Helsinki, Finland / Tallinn / Riga / Vilnius (EET/EEST)", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
  {"Europe/Kyiv", "Kyiv, Ukraine (EET/EEST)", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
  {"Europe/Istanbul", "Istanbul, Turkey (TRT, UTC+3)", "TRT-3"},
  {"Europe/Moscow", "Moscow, Russia (MSK, UTC+3)", "MSK-3"},

  // North America
  {"America/New_York", "New York, USA / Boston / Miami / Toronto (Eastern Time)", "EST5EDT,M3.2.0,M11.1.0"},
  {"America/Chicago", "Chicago, USA / Dallas / Houston (Central Time)", "CST6CDT,M3.2.0,M11.1.0"},
  {"America/Denver", "Denver, USA / Salt Lake City / Calgary (Mountain Time)", "MST7MDT,M3.2.0,M11.1.0"},
  {"America/Phoenix", "Phoenix, Arizona, USA (MST, no DST)", "MST7"},
  {"America/Los_Angeles", "Los Angeles, USA / San Francisco / Seattle / Vancouver (Pacific Time)", "PST8PDT,M3.2.0,M11.1.0"},
  {"America/Anchorage", "Anchorage, Alaska, USA (AKST/AKDT)", "AKST9AKDT,M3.2.0,M11.1.0"},
  {"Pacific/Honolulu", "Honolulu, Hawaii, USA (HST, no DST)", "HST10"},
  {"America/Halifax", "Halifax, Canada (Atlantic Time)", "AST4ADT,M3.2.0,M11.1.0"},
  {"America/St_Johns", "St. John's, Newfoundland, Canada", "NST3:30NDT,M3.2.0,M11.1.0"},

  // Latin America
  {"America/Mexico_City", "Mexico City, Mexico (CST, no DST)", "CST6"},
  {"America/Bogota", "Bogota, Colombia / Lima / Quito (COT, UTC-5)", "COT5"},
  {"America/Sao_Paulo", "Sao Paulo, Brazil / Rio de Janeiro (BRT, UTC-3)", "<-03>3"},
  {"America/Buenos_Aires", "Buenos Aires, Argentina (ART, UTC-3)", "<-03>3"},
  {"America/Santiago", "Santiago, Chile (CLT/CLST)", "CLT4CLST,M9.1.0/24,M4.1.0/24"},

  // Asia
  {"Asia/Tokyo", "Tokyo, Japan / Seoul, South Korea (JST, UTC+9)", "JST-9"},
  {"Asia/Shanghai", "Beijing / Shanghai, China / Hong Kong / Taipei (CST, UTC+8)", "CST-8"},
  {"Asia/Singapore", "Singapore / Kuala Lumpur, Malaysia (SGT, UTC+8)", "SGT-8"},
  {"Asia/Bangkok", "Bangkok, Thailand / Hanoi / Jakarta (ICT, UTC+7)", "ICT-7"},
  {"Asia/Kolkata", "Kolkata / Mumbai / New Delhi, India (IST, UTC+5:30)", "IST-5:30"},
  {"Asia/Karachi", "Karachi, Pakistan (PKT, UTC+5)", "PKT-5"},
  {"Asia/Tashkent", "Tashkent, Uzbekistan (UZT, UTC+5)", "UZT-5"},
  {"Asia/Almaty", "Almaty, Kazakhstan (UTC+5)", "<-05>5"},
  {"Asia/Dubai", "Dubai, UAE / Abu Dhabi / Muscat (GST, UTC+4)", "GST-4"},
  {"Asia/Riyadh", "Riyadh, Saudi Arabia / Doha / Kuwait (AST, UTC+3)", "AST-3"},
  {"Asia/Jerusalem", "Jerusalem, Israel / Tel Aviv (IST/IDT)", "IST-2IDT,M3.4.4/26,M10.5.0"},

  // Africa
  {"Africa/Cairo", "Cairo, Egypt (EET/EEST with DST)", "EET-2EEST,M4.5.5/0,M10.5.4/24"},
  {"Africa/Johannesburg", "Johannesburg, South Africa / Cape Town (SAST, UTC+2)", "SAST-2"},
  {"Africa/Nairobi", "Nairobi, Kenya / Addis Ababa (EAT, UTC+3)", "EAT-3"},
  {"Africa/Lagos", "Lagos, Nigeria / Accra (WAT, UTC+1)", "WAT-1"},
  {"Africa/Casablanca", "Casablanca, Morocco (WET/WEST)", "<-01>1<+00>,M4.2.0/2,M2.4.0/3"},

  // Australia & Pacific
  {"Australia/Sydney", "Sydney / Melbourne / Canberra, Australia (AEST/AEDT)", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
  {"Australia/Brisbane", "Brisbane, Queensland, Australia (AEST, no DST)", "AEST-10"},
  {"Australia/Adelaide", "Adelaide, South Australia (ACST/ACDT)", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
  {"Australia/Darwin", "Darwin, Northern Territory, Australia (ACST, no DST)", "ACST-9:30"},
  {"Australia/Perth", "Perth, Western Australia (AWST, no DST)", "AWST-8"},
  {"Pacific/Auckland", "Auckland / Wellington, New Zealand (NZST/NZDT)", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
  {"Pacific/Fiji", "Suva, Fiji (FJT, UTC+12)", "FJT-12"}
};

const int kTimeZoneCount = sizeof(kTimeZones) / sizeof(kTimeZones[0]);

String getPosixTz(const String &tz)
{
  if (tz.isEmpty())
    return "GMT0BST,M3.5.0/1,M10.5.0";

  for (int i = 0; i < kTimeZoneCount; i++) {
    if (tz.equalsIgnoreCase(kTimeZones[i].id) || tz.equalsIgnoreCase(kTimeZones[i].posix))
      return kTimeZones[i].posix;
  }

  // Check if it's a legacy pure integer offset (e.g. "2", "-5", "+1")
  bool isNumber = true;
  int startIdx = (tz[0] == '+' || tz[0] == '-') ? 1 : 0;
  if (startIdx >= tz.length()) isNumber = false;
  for (int i = startIdx; i < tz.length(); i++) {
    if (!isdigit(tz[i])) { isNumber = false; break; }
  }
  if (isNumber) {
    int offset = tz.toInt();
    // POSIX TZ specifies hours WEST of UTC, so sign is inverted
    if (offset == 0) return "UTC0";
    if (offset > 0) return "UTC-" + String(offset);
    return "UTC+" + String(-offset);
  }

  return tz;
}

const TimeZoneInfo* findTimeZone(const String &tz)
{
  for (int i = 0; i < kTimeZoneCount; i++) {
    if (tz.equalsIgnoreCase(kTimeZones[i].id) || tz.equalsIgnoreCase(kTimeZones[i].posix))
      return &kTimeZones[i];
  }
  return nullptr;
}
