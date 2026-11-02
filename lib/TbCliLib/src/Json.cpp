/*
 Copyright (C) 2026 Drew Williams

 This file is part of TrenchBroom.

 TrenchBroom is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 TrenchBroom is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with TrenchBroom. If not, see <http://www.gnu.org/licenses/>.
 */

#include "cli/Json.h"

#include <fmt/format.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace tb::cli
{
namespace
{

const Json& nullJson()
{
  static const auto null = Json{};
  return null;
}

void dumpString(std::string& out, const std::string_view s)
{
  out.push_back('"');
  for (const auto c : s)
  {
    switch (c)
    {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20)
      {
        out += fmt::format("\\u{:04x}", static_cast<unsigned char>(c));
      }
      else
      {
        out.push_back(c);
      }
    }
  }
  out.push_back('"');
}

void dumpValue(std::string& out, const Json& json)
{
  if (json.isNull())
  {
    out += "null";
  }
  else if (json.isBool())
  {
    out += json.asBool() ? "true" : "false";
  }
  else if (json.isNumber())
  {
    out += formatNumber(json.asNumber());
  }
  else if (json.isString())
  {
    dumpString(out, json.asString());
  }
  else if (json.isArray())
  {
    out.push_back('[');
    auto first = true;
    for (const auto& element : json.asArray())
    {
      if (!first)
      {
        out.push_back(',');
      }
      first = false;
      dumpValue(out, element);
    }
    out.push_back(']');
  }
  else
  {
    out.push_back('{');
    auto first = true;
    for (const auto& [key, value] : json.asObject())
    {
      if (!first)
      {
        out.push_back(',');
      }
      first = false;
      dumpString(out, key);
      out.push_back(':');
      dumpValue(out, value);
    }
    out.push_back('}');
  }
}

void appendUtf8(std::string& out, const uint32_t cp)
{
  if (cp < 0x80)
  {
    out.push_back(char(cp));
  }
  else if (cp < 0x800)
  {
    out.push_back(char(0xC0 | (cp >> 6)));
    out.push_back(char(0x80 | (cp & 0x3F)));
  }
  else if (cp < 0x10000)
  {
    out.push_back(char(0xE0 | (cp >> 12)));
    out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(char(0x80 | (cp & 0x3F)));
  }
  else
  {
    out.push_back(char(0xF0 | (cp >> 18)));
    out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(char(0x80 | (cp & 0x3F)));
  }
}

class JsonParser
{
private:
  std::string_view m_str;
  size_t m_pos = 0;
  size_t m_line = 1;

public:
  explicit JsonParser(const std::string_view str)
    : m_str{str}
  {
  }

  Json parseDocument()
  {
    auto result = parseValue();
    skipWhitespace();
    if (m_pos != m_str.size())
    {
      fail("unexpected trailing content");
    }
    return result;
  }

private:
  [[noreturn]] void fail(std::string msg) const
  {
    throw JsonParseError{std::move(msg), m_line};
  }

  void skipWhitespace()
  {
    while (m_pos < m_str.size())
    {
      const auto c = m_str[m_pos];
      if (c == '\n')
      {
        ++m_line;
      }
      else if (c != ' ' && c != '\t' && c != '\r')
      {
        break;
      }
      ++m_pos;
    }
  }

  char peek() const { return m_pos < m_str.size() ? m_str[m_pos] : '\0'; }

  bool isDigit() const { return peek() >= '0' && peek() <= '9'; }

  void expect(const char c)
  {
    if (peek() != c)
    {
      fail(fmt::format("expected '{}'", c));
    }
    ++m_pos;
  }

  void expectLiteral(const std::string_view literal)
  {
    if (m_str.substr(m_pos, literal.size()) != literal)
    {
      fail("invalid literal");
    }
    m_pos += literal.size();
  }

  Json parseValue()
  {
    skipWhitespace();
    switch (peek())
    {
    case '{':
      return parseObject();
    case '[':
      return parseArray();
    case '"':
      return Json{parseString()};
    case 't':
      expectLiteral("true");
      return Json{true};
    case 'f':
      expectLiteral("false");
      return Json{false};
    case 'n':
      expectLiteral("null");
      return Json{};
    case '\0':
      fail("unexpected end of input");
    default:
      return parseNumber();
    }
  }

  Json parseObject()
  {
    expect('{');
    auto result = Json::object();
    skipWhitespace();
    if (peek() == '}')
    {
      ++m_pos;
      return result;
    }
    while (true)
    {
      skipWhitespace();
      if (peek() != '"')
      {
        fail("expected object key");
      }
      auto key = parseString();
      skipWhitespace();
      expect(':');
      result.set(std::move(key), parseValue());
      skipWhitespace();
      if (peek() != ',')
      {
        expect('}');
        return result;
      }
      ++m_pos;
    }
  }

  Json parseArray()
  {
    expect('[');
    auto result = Json::array();
    skipWhitespace();
    if (peek() == ']')
    {
      ++m_pos;
      return result;
    }
    while (true)
    {
      result.push(parseValue());
      skipWhitespace();
      if (peek() != ',')
      {
        expect(']');
        return result;
      }
      ++m_pos;
    }
  }

  uint32_t parseHex4()
  {
    if (m_pos + 4 > m_str.size())
    {
      fail("invalid unicode escape");
    }
    auto result = uint32_t{0};
    for (size_t i = 0; i < 4; ++i)
    {
      const auto c = m_str[m_pos++];
      result <<= 4;
      if (c >= '0' && c <= '9')
      {
        result |= uint32_t(c - '0');
      }
      else if (c >= 'a' && c <= 'f')
      {
        result |= uint32_t(c - 'a' + 10);
      }
      else if (c >= 'A' && c <= 'F')
      {
        result |= uint32_t(c - 'A' + 10);
      }
      else
      {
        fail("invalid unicode escape");
      }
    }
    return result;
  }

  std::string parseString()
  {
    expect('"');
    auto result = std::string{};
    while (true)
    {
      if (m_pos >= m_str.size() || m_str[m_pos] == '\n')
      {
        fail("unterminated string");
      }
      const auto c = m_str[m_pos++];
      if (c == '"')
      {
        return result;
      }
      if (c != '\\')
      {
        result.push_back(c);
        continue;
      }
      if (m_pos >= m_str.size())
      {
        fail("unterminated string");
      }
      switch (const auto e = m_str[m_pos++])
      {
      case '"':
      case '\\':
      case '/':
        result.push_back(e);
        break;
      case 'b':
        result.push_back('\b');
        break;
      case 'f':
        result.push_back('\f');
        break;
      case 'n':
        result.push_back('\n');
        break;
      case 'r':
        result.push_back('\r');
        break;
      case 't':
        result.push_back('\t');
        break;
      case 'u': {
        auto cp = parseHex4();
        if (cp >= 0xD800 && cp < 0xDC00 && m_str.substr(m_pos, 2) == "\\u")
        {
          m_pos += 2;
          const auto low = parseHex4();
          if (low < 0xDC00 || low >= 0xE000)
          {
            fail("invalid unicode escape");
          }
          cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        }
        appendUtf8(result, cp);
        break;
      }
      default:
        fail("invalid escape sequence");
      }
    }
  }

  void parseDigits()
  {
    if (!isDigit())
    {
      fail("invalid number");
    }
    while (isDigit())
    {
      ++m_pos;
    }
  }

  Json parseNumber()
  {
    const auto start = m_pos;
    if (peek() == '-')
    {
      ++m_pos;
    }
    if (!isDigit())
    {
      fail("unexpected character");
    }
    parseDigits();
    if (peek() == '.')
    {
      ++m_pos;
      parseDigits();
    }
    if (peek() == 'e' || peek() == 'E')
    {
      ++m_pos;
      if (peek() == '+' || peek() == '-')
      {
        ++m_pos;
      }
      parseDigits();
    }
    const auto text = std::string{m_str.substr(start, m_pos - start)};
    return Json{std::strtod(text.c_str(), nullptr)};
  }
};

} // namespace

Json::Json() = default;

Json::Json(std::nullptr_t) {}

Json::Json(const bool b)
  : m_value{b}
{
}

Json::Json(const double d)
  : m_value{d}
{
}

Json::Json(const char* s)
  : m_value{std::string{s}}
{
}

Json::Json(const std::string_view s)
  : m_value{std::string{s}}
{
}

Json::Json(std::string s)
  : m_value{std::move(s)}
{
}

Json::Json(Array a)
  : m_value{std::move(a)}
{
}

Json::Json(Object o)
  : m_value{std::move(o)}
{
}

Json Json::object()
{
  return Json{Object{}};
}

Json Json::array()
{
  return Json{Array{}};
}

bool Json::isNull() const
{
  return std::holds_alternative<std::nullptr_t>(m_value);
}

bool Json::isBool() const
{
  return std::holds_alternative<bool>(m_value);
}

bool Json::isNumber() const
{
  return std::holds_alternative<double>(m_value);
}

bool Json::isString() const
{
  return std::holds_alternative<std::string>(m_value);
}

bool Json::isArray() const
{
  return std::holds_alternative<Array>(m_value);
}

bool Json::isObject() const
{
  return std::holds_alternative<Object>(m_value);
}

bool Json::asBool() const
{
  return std::get<bool>(m_value);
}

double Json::asNumber() const
{
  return std::get<double>(m_value);
}

const std::string& Json::asString() const
{
  return std::get<std::string>(m_value);
}

const Json::Array& Json::asArray() const
{
  return std::get<Array>(m_value);
}

const Json::Object& Json::asObject() const
{
  return std::get<Object>(m_value);
}

const Json* Json::find(const std::string_view key) const
{
  if (const auto* object = std::get_if<Object>(&m_value))
  {
    for (const auto& member : *object)
    {
      if (member.key == key)
      {
        return &member.value;
      }
    }
  }
  return nullptr;
}

const Json& Json::operator[](const std::string_view key) const
{
  const auto* value = find(key);
  return value ? *value : nullJson();
}

const Json& Json::operator[](const size_t index) const
{
  const auto* array = std::get_if<Array>(&m_value);
  return array && index < array->size() ? (*array)[index] : nullJson();
}

size_t Json::size() const
{
  if (const auto* array = std::get_if<Array>(&m_value))
  {
    return array->size();
  }
  if (const auto* object = std::get_if<Object>(&m_value))
  {
    return object->size();
  }
  return 0;
}

Json& Json::set(std::string key, Json value)
{
  auto& object = std::get<Object>(m_value);
  for (auto& member : object)
  {
    if (member.key == key)
    {
      member.value = std::move(value);
      return *this;
    }
  }
  object.push_back(JsonMember{std::move(key), std::move(value)});
  return *this;
}

Json& Json::push(Json value)
{
  std::get<Array>(m_value).push_back(std::move(value));
  return *this;
}

std::string Json::dump() const
{
  auto result = std::string{};
  dumpValue(result, *this);
  return result;
}

bool operator==(const Json& lhs, const Json& rhs)
{
  return lhs.m_value == rhs.m_value;
}

std::string formatNumber(const double d)
{
  if (!std::isfinite(d))
  {
    return "null";
  }

  if (std::abs(d) >= 1e15)
  {
    // too large for 3 decimals to matter, and too large to scale without error
    return fmt::format("{:.0f}", d);
  }

  const auto rounded = std::round(d * 1000.0) / 1000.0;
  if (rounded == std::trunc(rounded))
  {
    // the integer cast also turns -0 into 0
    return fmt::format("{}", static_cast<int64_t>(rounded));
  }

  auto result = fmt::format("{:.3f}", rounded);
  while (result.back() == '0')
  {
    result.pop_back();
  }
  return result;
}

kdl::result<Json, JsonParseError> parseJson(const std::string_view str)
{
  try
  {
    return JsonParser{str}.parseDocument();
  }
  catch (const JsonParseError& e)
  {
    return e;
  }
}

} // namespace tb::cli
