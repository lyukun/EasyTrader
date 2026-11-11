#pragma once

#include <compare>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>

#include "trading/model/identifiers.h"

namespace trading {

class Price {
 public:
  static constexpr std::int64_t kScale = 1'000'000'000;
  static constexpr std::uint8_t kMaxPrecision = 9;

  static Price fromString(std::string_view value);
  static Price fromRaw(std::int64_t raw, std::uint8_t precision = kMaxPrecision);

  constexpr Price() = default;
  constexpr std::int64_t raw() const { return raw_; }
  constexpr std::uint8_t precision() const { return precision_; }
  std::string toString() const;

  friend constexpr bool operator==(Price lhs, Price rhs) {
    return lhs.raw_ == rhs.raw_;
  }
  friend constexpr auto operator<=>(Price lhs, Price rhs) {
    return lhs.raw_ <=> rhs.raw_;
  }

 private:
  constexpr Price(std::int64_t raw, std::uint8_t precision)
      : raw_(raw), precision_(precision) {}

  std::int64_t raw_{0};
  std::uint8_t precision_{0};
};

class Quantity {
 public:
  static constexpr std::uint64_t kScale = 1'000'000'000;
  static constexpr std::uint8_t kMaxPrecision = 9;

  static Quantity fromString(std::string_view value);
  static Quantity fromRaw(std::uint64_t raw,
                          std::uint8_t precision = kMaxPrecision);

  constexpr Quantity() = default;
  constexpr std::uint64_t raw() const { return raw_; }
  constexpr std::uint8_t precision() const { return precision_; }
  constexpr bool isZero() const { return raw_ == 0; }
  std::string toString() const;

  friend constexpr bool operator==(Quantity lhs, Quantity rhs) {
    return lhs.raw_ == rhs.raw_;
  }
  friend constexpr auto operator<=>(Quantity lhs, Quantity rhs) {
    return lhs.raw_ <=> rhs.raw_;
  }

 private:
  constexpr Quantity(std::uint64_t raw, std::uint8_t precision)
      : raw_(raw), precision_(precision) {}

  std::uint64_t raw_{0};
  std::uint8_t precision_{0};
};

class Decimal {
 public:
  static Decimal fromString(std::string_view value);
  static Decimal fromRaw(std::int64_t raw,
                         std::uint8_t precision = Price::kMaxPrecision);

  constexpr Decimal() = default;
  constexpr std::int64_t raw() const { return raw_; }
  constexpr std::uint8_t precision() const { return precision_; }
  std::string toString() const;

  friend constexpr bool operator==(Decimal lhs, Decimal rhs) {
    return lhs.raw_ == rhs.raw_;
  }
  friend constexpr auto operator<=>(Decimal lhs, Decimal rhs) {
    return lhs.raw_ <=> rhs.raw_;
  }

 private:
  constexpr Decimal(std::int64_t raw, std::uint8_t precision)
      : raw_(raw), precision_(precision) {}

  std::int64_t raw_{0};
  std::uint8_t precision_{0};
};

class Money {
 public:
  static Money fromString(std::string_view value, Currency currency);
  static Money fromRaw(std::int64_t raw, Currency currency,
                       std::uint8_t precision = Price::kMaxPrecision);
  static Money zero(Currency currency);

  Money() = default;
  Money(Decimal amount, Currency currency);

  constexpr const Decimal& amount() const { return amount_; }
  constexpr std::int64_t raw() const { return amount_.raw(); }
  constexpr std::uint8_t precision() const { return amount_.precision(); }
  const Currency& currency() const { return currency_; }
  bool isValid() const { return !currency_.empty(); }
  bool isZero() const { return amount_.raw() == 0; }
  std::string toString() const;

  Money checkedAdd(const Money& other) const;
  Money checkedSubtract(const Money& other) const;

  friend bool operator==(const Money&, const Money&) = default;

 private:
  Decimal amount_;
  Currency currency_;
};

std::ostream& operator<<(std::ostream& stream, Price value);
std::ostream& operator<<(std::ostream& stream, Quantity value);
std::ostream& operator<<(std::ostream& stream, Decimal value);
std::ostream& operator<<(std::ostream& stream, const Money& value);

}  // namespace trading
