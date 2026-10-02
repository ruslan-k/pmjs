#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace pmjs {
namespace renderer_shader_source {

inline bool isIdentifierCharacter(char value) {
  return (value >= 'a' && value <= 'z') ||
         (value >= 'A' && value <= 'Z') ||
         (value >= '0' && value <= '9') || value == '_';
}

inline bool isWhitespace(char value) {
  return value == ' ' || value == '\t' || value == '\r' ||
         value == '\n' || value == '\f' || value == '\v';
}

inline std::size_t skipTrivia(std::string_view source, std::size_t position) {
  while (position < source.size()) {
    if (isWhitespace(source[position])) {
      ++position;
      continue;
    }
    if (position + 1 < source.size() && source[position] == '/' &&
        source[position + 1] == '/') {
      const std::size_t newline = source.find('\n', position + 2);
      if (newline == std::string_view::npos) return source.size();
      position = newline + 1;
      continue;
    }
    if (position + 1 < source.size() && source[position] == '/' &&
        source[position + 1] == '*') {
      const std::size_t end = source.find("*/", position + 2);
      if (end == std::string_view::npos) return source.size();
      position = end + 2;
      continue;
    }
    break;
  }
  return position;
}

inline std::string normalizeForGles3(std::string_view source) {
  std::size_t first = 0;
  while (first < source.size() && isWhitespace(source[first])) ++first;
  if (source.substr(first, 8) != "#version") return std::string(source);

  std::string normalized(source.substr(first));
  constexpr std::string_view builtinName = "textureSize";
  constexpr std::string_view uniformName = "pmjsTextureSize";
  std::size_t position = 0;
  while (position < normalized.size()) {
    if (position + 1 < normalized.size() && normalized[position] == '/' &&
        normalized[position + 1] == '/') {
      const std::size_t newline = normalized.find('\n', position + 2);
      position = newline == std::string::npos ? normalized.size() : newline + 1;
      continue;
    }
    if (position + 1 < normalized.size() && normalized[position] == '/' &&
        normalized[position + 1] == '*') {
      const std::size_t end = normalized.find("*/", position + 2);
      position = end == std::string::npos ? normalized.size() : end + 2;
      continue;
    }
    if (!isIdentifierCharacter(normalized[position])) {
      ++position;
      continue;
    }

    const std::size_t start = position;
    while (position < normalized.size() &&
           isIdentifierCharacter(normalized[position])) {
      ++position;
    }
    if (std::string_view(normalized).substr(start, position - start) !=
        builtinName) {
      continue;
    }

    const std::size_t next = skipTrivia(normalized, position);
    if (next < normalized.size() && normalized[next] == '(') continue;
    normalized.replace(start, builtinName.size(), uniformName);
    position = start + uniformName.size();
  }
  return normalized;
}

}  // namespace renderer_shader_source
}  // namespace pmjs
