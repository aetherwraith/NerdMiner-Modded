#ifndef CITIES_H
#define CITIES_H

// Cities offered on the LAN settings page as a quick way to set the location
// night dimming works sunrise and sunset out for. Only the coordinates are
// stored (Settings.Latitude / Longitude); anywhere within a few hundred km is
// good to a few minutes. Decimal degrees, north and east positive; sorted by
// name.
struct CityInfo
{
  const char *name;
  float latitude;
  float longitude;
};

extern const CityInfo kCities[];
extern const int kCityCount;

#endif // CITIES_H
