#pragma once

// FIX decimal strings <-> the wire's int64 at 10^-8 (docs/spec.md §4).
// Exact, by string arithmetic: no floating point anywhere near a price.
// Pure functions, so the output codec that uses them is a pure function
// of the journal record (spec §7).

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace exchange::fix {

inline constexpr int kDecimals = 8;  // -priceExponent
inline constexpr std::int64_t kScale = 100000000;

// "101.25" -> 10125000000. Optional sign is not accepted (prices and
// quantities are positive or zero on the wire); more than eight
// decimals, an empty string, a lone ".", or overflow -> nullopt.
inline std::optional<std::int64_t> parseDecimal(std::string_view text) noexcept {
  if (text.empty()) {
    return std::nullopt;
  }
  std::int64_t whole = 0;
  std::size_t i = 0;
  bool digits = false;
  for (; i < text.size() && text[i] != '.'; ++i) {
    const char c = text[i];
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    if (whole > (INT64_MAX - 9) / 10) {
      return std::nullopt;
    }
    whole = whole * 10 + (c - '0');
    digits = true;
  }
  std::int64_t frac = 0;
  int fracDigits = 0;
  if (i < text.size()) {
    ++i;  // '.'
    for (; i < text.size(); ++i) {
      const char c = text[i];
      if (c < '0' || c > '9') {
        return std::nullopt;
      }
      if (++fracDigits > kDecimals) {
        return std::nullopt;
      }
      frac = frac * 10 + (c - '0');
      digits = true;
    }
    for (; fracDigits < kDecimals; ++fracDigits) {
      frac *= 10;
    }
  }
  if (!digits) {
    return std::nullopt;
  }
  if (whole > (INT64_MAX - frac) / kScale) {
    return std::nullopt;
  }
  return whole * kScale + frac;
}

// 10125000000 -> "101.25"; 500000000 -> "5"; 0 -> "0". Trailing zeros
// trimmed, no exponent, no sign for negatives (which never reach here;
// a negative is rendered with '-' so a bug is visible, not hidden).
struct Decimal {
  std::array<char, 32> text{};
  std::size_t size = 0;
  std::string_view view() const noexcept { return {text.data(), size}; }
};

inline Decimal formatDecimal(std::int64_t value) noexcept {
  Decimal out;
  char* p = out.text.data();
  std::uint64_t magnitude = value < 0 ? static_cast<std::uint64_t>(-(value + 1)) + 1 : static_cast<std::uint64_t>(value);
  if (value < 0) {
    *p++ = '-';
  }
  const std::uint64_t whole = magnitude / kScale;
  std::uint64_t frac = magnitude % kScale;
  // whole part
  char digits[24];
  int n = 0;
  std::uint64_t w = whole;
  do {
    digits[n++] = static_cast<char>('0' + w % 10);
    w /= 10;
  } while (w != 0);
  while (n > 0) {
    *p++ = digits[--n];
  }
  if (frac != 0) {
    *p++ = '.';
    char f[kDecimals];
    for (int k = kDecimals - 1; k >= 0; --k) {
      f[k] = static_cast<char>('0' + frac % 10);
      frac /= 10;
    }
    int last = kDecimals;
    while (last > 0 && f[last - 1] == '0') {
      --last;
    }
    for (int k = 0; k < last; ++k) {
      *p++ = f[k];
    }
  }
  out.size = static_cast<std::size_t>(p - out.text.data());
  return out;
}

}  // namespace exchange::fix
