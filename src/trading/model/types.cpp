#include "trading/model/types.h"

#include <array>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <utility>

namespace trading {
namespace {

constexpr std::array<std::uint64_t, 10> kPowersOfTen{
    1,          10,          100,          1'000,       10'000,
    100'000,    1'000'000,   10'000'000,   100'000'000, 1'000'000'000};

struct ParsedDecimal {
  bool negative;
  std::uint64_t magnitude;
  std::uint8_t precision;
};

ParsedDecimal parseDecimal(std::string_view value, bool allow_negative) {
  if (value.empty()) {
    throw std::invalid_argument("decimal value must not be empty");
  }

  std::size_t index = 0;
  bool negative = false;
  if (value.front() == '-' || value.front() == '+') {
    negative = value.front() == '-';
    if (negative && !allow_negative) {
      throw std::invalid_argument("quantity must not be negative");
    }
    if (++index == value.size()) {
      throw std::invalid_argument("decimal value must contain digits");
    }
  }

  bool seen_dot = false;
  bool seen_digit = false;
  std::uint8_t precision = 0;
  std::uint64_t unscaled = 0;
  for (; index < value.size(); ++index) {
    const char ch = value[index];
    if (ch == '.') {
      if (seen_dot) {
        throw std::invalid_argument("decimal value contains multiple points");
      }
      seen_dot = true;
      continue;
    }
    if (ch < '0' || ch > '9') {
      throw std::invalid_argument("decimal value contains a non-digit");
    }
    seen_digit = true;
    if (seen_dot) {
      if (precision == Price::kMaxPrecision) {
        throw std::out_of_range("decimal precision exceeds 9 places");
      }
      ++precision;
    }
    const auto digit = static_cast<std::uint64_t>(ch - '0');
    if (unscaled > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
      throw std::out_of_range("decimal value exceeds representable range");
    }
    unscaled = unscaled * 10 + digit;
  }
  if (!seen_digit) {
    throw std::invalid_argument("decimal value must contain digits");
  }

  const auto multiplier = kPowersOfTen[Price::kMaxPrecision - precision];
  if (unscaled > std::numeric_limits<std::uint64_t>::max() / multiplier) {
    throw std::out_of_range("decimal value exceeds representable range");
  }
  return ParsedDecimal{negative, unscaled * multiplier, precision};
}

std::string formatDecimal(std::uint64_t magnitude, bool negative,
                          std::uint8_t precision) {
  if (precision > Price::kMaxPrecision) {
    throw std::out_of_range("decimal precision exceeds 9 places");
  }
  const auto divisor = kPowersOfTen[Price::kMaxPrecision - precision];
  const auto displayed = magnitude / divisor;
  const auto fractional_divisor = kPowersOfTen[precision];
  const auto whole = displayed / fractional_divisor;
  const auto fractional = displayed % fractional_divisor;

  std::string result;
  if (negative && magnitude != 0) {
    result.push_back('-');
  }
  result += std::to_string(whole);
  if (precision == 0) {
    return result;
  }
  result.push_back('.');
  auto digits = std::to_string(fractional);
  result.append(static_cast<std::size_t>(precision) - digits.size(), '0');
  result += digits;
  return result;
}

std::int64_t toSignedRaw(const ParsedDecimal& parsed) {
  const auto positive_limit =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  const auto negative_limit = positive_limit + 1;
  const auto limit = parsed.negative ? negative_limit : positive_limit;
  if (parsed.magnitude > limit) {
    throw std::out_of_range("decimal value exceeds signed range");
  }
  if (!parsed.negative) {
    return static_cast<std::int64_t>(parsed.magnitude);
  }
  if (parsed.magnitude == negative_limit) {
    return std::numeric_limits<std::int64_t>::min();
  }
  return -static_cast<std::int64_t>(parsed.magnitude);
}

std::uint64_t magnitude(std::int64_t raw) {
  if (raw >= 0) {
    return static_cast<std::uint64_t>(raw);
  }
  return static_cast<std::uint64_t>(-(raw + 1)) + 1;
}

std::int64_t checkedRawAdd(std::int64_t lhs, std::int64_t rhs,
                           const char* message) {
  if ((rhs > 0 && lhs > std::numeric_limits<std::int64_t>::max() - rhs) ||
      (rhs < 0 && lhs < std::numeric_limits<std::int64_t>::min() - rhs)) {
    throw std::overflow_error(message);
  }
  return lhs + rhs;
}

void validateRawPrecision(std::uint64_t magnitude,
                          std::uint8_t precision) {
  if (precision > Price::kMaxPrecision) {
    throw std::out_of_range("decimal precision exceeds 9 places");
  }
  const auto divisor = kPowersOfTen[Price::kMaxPrecision - precision];
  if (magnitude % divisor != 0) {
    throw std::invalid_argument(
        "raw value has fractional digits beyond its precision");
  }
}

}  // namespace

Price Price::fromString(std::string_view value) {
  const auto parsed = parseDecimal(value, true);
  return Price(toSignedRaw(parsed), parsed.precision);
}

Price Price::fromRaw(std::int64_t raw, std::uint8_t precision) {
  validateRawPrecision(magnitude(raw), precision);
  return Price(raw, precision);
}

std::string Price::toString() const {
  return formatDecimal(magnitude(raw_), raw_ < 0, precision_);
}

Quantity Quantity::fromString(std::string_view value) {
  const auto parsed = parseDecimal(value, false);
  if (parsed.magnitude >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    throw std::out_of_range("quantity exceeds signed position range");
  }
  return Quantity(parsed.magnitude, parsed.precision);
}

Quantity Quantity::fromRaw(std::uint64_t raw, std::uint8_t precision) {
  if (raw > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    throw std::out_of_range("quantity exceeds signed position range");
  }
  validateRawPrecision(raw, precision);
  return Quantity(raw, precision);
}

std::string Quantity::toString() const {
  return formatDecimal(raw_, false, precision_);
}

Decimal Decimal::fromString(std::string_view value) {
  const auto parsed = parseDecimal(value, true);
  return Decimal(toSignedRaw(parsed), parsed.precision);
}

Decimal Decimal::fromRaw(std::int64_t raw, std::uint8_t precision) {
  validateRawPrecision(magnitude(raw), precision);
  return Decimal(raw, precision);
}

std::string Decimal::toString() const {
  return formatDecimal(magnitude(raw_), raw_ < 0, precision_);
}

Money Money::fromString(std::string_view value, Currency currency) {
  return Money(Decimal::fromString(value), std::move(currency));
}

Money Money::fromRaw(std::int64_t raw, Currency currency,
                     std::uint8_t precision) {
  return Money(Decimal::fromRaw(raw, precision), std::move(currency));
}

Money Money::zero(Currency currency) {
  return Money(Decimal{}, std::move(currency));
}

Money::Money(Decimal amount, Currency currency)
    : amount_(amount), currency_(std::move(currency)) {
  if (currency_.empty()) {
    throw std::invalid_argument("money currency must not be empty");
  }
}

std::string Money::toString() const {
  return amount_.toString() + " " + currency_;
}

Money Money::checkedAdd(const Money& other) const {
  if (currency_ != other.currency_) {
    throw std::invalid_argument("money currencies do not match");
  }
  return Money::fromRaw(
      checkedRawAdd(raw(), other.raw(), "money addition overflow"), currency_);
}

Money Money::checkedSubtract(const Money& other) const {
  if (currency_ != other.currency_) {
    throw std::invalid_argument("money currencies do not match");
  }
  if (other.raw() == std::numeric_limits<std::int64_t>::min()) {
    throw std::overflow_error("money subtraction overflow");
  }
  return Money::fromRaw(
      checkedRawAdd(raw(), -other.raw(), "money subtraction overflow"),
      currency_);
}

std::ostream& operator<<(std::ostream& stream, Price value) {
  return stream << value.toString();
}

std::ostream& operator<<(std::ostream& stream, Quantity value) {
  return stream << value.toString();
}

std::ostream& operator<<(std::ostream& stream, Decimal value) {
  return stream << value.toString();
}

std::ostream& operator<<(std::ostream& stream, const Money& value) {
  return stream << value.toString();
}

}  // namespace trading
