#include "currency.h"

const CurrencyInfo kCurrencies[] = {
    {"usd", "USD - US Dollar", "$", "$"},
    {"gbp", "GBP - British Pound", "GBP ", "\xC2\xA3"},
    {"eur", "EUR - Euro", "EUR ", "\xE2\x82\xAC"},
    {"cad", "CAD - Canadian Dollar", "C$", "C$"},
    {"aud", "AUD - Australian Dollar", "A$", "A$"},
    {"nzd", "NZD - New Zealand Dollar", "NZ$", "NZ$"},
    {"chf", "CHF - Swiss Franc", "CHF ", "CHF "},
    {"jpy", "JPY - Japanese Yen", "JPY ", "\xC2\xA5"},
    {"cny", "CNY - Chinese Yuan", "CNY ", "\xC2\xA5"},
    {"inr", "INR - Indian Rupee", "INR ", "\xE2\x82\xB9"},
    {"krw", "KRW - South Korean Won", "KRW ", "\xE2\x82\xA9"},
    {"sgd", "SGD - Singapore Dollar", "S$", "S$"},
    {"hkd", "HKD - Hong Kong Dollar", "HK$", "HK$"},
    {"brl", "BRL - Brazilian Real", "R$", "R$"},
    {"mxn", "MXN - Mexican Peso", "MX$", "MX$"},
    {"sek", "SEK - Swedish Krona", "kr ", "kr "},
    {"nok", "NOK - Norwegian Krone", "kr ", "kr "},
    {"dkk", "DKK - Danish Krone", "kr ", "kr "},
    {"pln", "PLN - Polish Zloty", "PLN ", "PLN "},
    {"czk", "CZK - Czech Koruna", "CZK ", "CZK "},
    {"zar", "ZAR - South African Rand", "R", "R"},
    {"try", "TRY - Turkish Lira", "TRY ", "\xE2\x82\xBA"},
};
const int kCurrencyCount = sizeof(kCurrencies) / sizeof(kCurrencies[0]);

const CurrencyInfo &currencyFor(const String &code)
{
  for (int i = 0; i < kCurrencyCount; i++)
    if (code.equalsIgnoreCase(kCurrencies[i].code))
      return kCurrencies[i];
  return kCurrencies[0];
}
