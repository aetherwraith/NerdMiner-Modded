#ifndef CURRENCY_H
#define CURRENCY_H

#include <Arduino.h>

// Fiat currencies the BTC price can be shown in. "code" is the lowercase
// vs_currency id used by the CoinGecko API (and stored in Settings.Currency);
// "prefix" is ASCII-only so every display font can render it; "symbol" is the
// real currency sign for fonts that carry the glyph.
struct CurrencyInfo
{
  const char *code;
  const char *label;
  const char *prefix; // ASCII-only
  const char *symbol; // UTF-8, for displays whose font has the glyph (Pulse)
};

extern const CurrencyInfo kCurrencies[];
extern const int kCurrencyCount;

// Returns the matching entry, or USD when the code is unknown.
const CurrencyInfo &currencyFor(const String &code);

#endif // CURRENCY_H
