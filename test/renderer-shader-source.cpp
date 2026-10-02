#include "renderer_shader_source.hpp"
#include "renderer_shaders.hpp"

#include <iostream>
#include <string>

namespace {

int failures = 0;

void expectContains(const std::string& value, const std::string& expected,
                    const char* label) {
  if (value.find(expected) == std::string::npos) {
    std::cerr << "FAIL: " << label << "\n";
    ++failures;
  }
}

void expectNotContains(const std::string& value, const std::string& unexpected,
                       const char* label) {
  if (value.find(unexpected) != std::string::npos) {
    std::cerr << "FAIL: " << label << "\n";
    ++failures;
  }
}


void expectMaliSafeBuiltin(const char* source, const char* label) {
  if (!source || std::string(source).rfind("#version 300 es", 0) != 0) {
    std::cerr << "FAIL: " << label << " does not start with #version at byte 0\n";
    ++failures;
    return;
  }
  if (std::string(source).find("uniform vec2 textureSize;") != std::string::npos) {
    std::cerr << "FAIL: " << label << " still declares colliding textureSize uniform\n";
    ++failures;
  }
}

}  // namespace

int main() {

  using namespace pmjs::renderer_shaders;
  expectMaliSafeBuiltin(primitiveSurfaceFragmentSource, "primitiveSurfaceFragmentSource");
  expectMaliSafeBuiltin(vertexSource, "vertexSource");
  expectMaliSafeBuiltin(fragmentSource, "fragmentSource");
  expectMaliSafeBuiltin(tileVertexSource, "tileVertexSource");
  expectMaliSafeBuiltin(tileFragmentSource, "tileFragmentSource");
  expectMaliSafeBuiltin(simpleFragmentSource, "simpleFragmentSource");
  expectMaliSafeBuiltin(generatedTextureFragmentSource, "generatedTextureFragmentSource");
  expectMaliSafeBuiltin(spriteEffectFragmentSource, "spriteEffectFragmentSource");
  expectMaliSafeBuiltin(presentationVertexSource, "presentationVertexSource");
  expectMaliSafeBuiltin(presentationFragmentSource, "presentationFragmentSource");

  const std::string source =
      " \n\t#version 300 es\n"
      "precision highp float;\n"
      "uniform sampler2D tiles;\n"
      "uniform vec2 textureSize;\n"
      "void main() {\n"
      "  vec2 builtin0 = textureSize(tiles, 0);\n"
      "  vec2 builtin1 = textureSize /* keep builtin */ (tiles, 0);\n"
      "  vec2 scale = gl_FragCoord.xy / textureSize;\n"
      "  vec2 names = mytextureSize + textureSizeSuffix;\n"
      "  // textureSize in comments is not an identifier use\n"
      "}\n";

  const std::string normalized =
      pmjs::renderer_shader_source::normalizeForGles3(source);
  if (normalized.rfind("#version 300 es", 0) != 0) {
    std::cerr << "FAIL: #version is not the first token\n";
    ++failures;
  }
  expectContains(normalized, "uniform vec2 pmjsTextureSize;",
                 "uniform collision is renamed");
  expectContains(normalized, "gl_FragCoord.xy / pmjsTextureSize",
                 "uniform use is renamed");
  expectContains(normalized, "textureSize(tiles, 0)",
                 "builtin call without trivia is preserved");
  expectContains(normalized, "textureSize /* keep builtin */ (tiles, 0)",
                 "builtin call with trivia is preserved");
  expectContains(normalized, "mytextureSize + textureSizeSuffix",
                 "neighboring identifiers are preserved");
  expectContains(normalized, "// textureSize in comments",
                 "comment text is preserved");
  expectNotContains(normalized, "uniform vec2 textureSize;",
                    "colliding uniform name is absent");

  if (failures != 0) {
    std::cerr << failures << " shader source regression(s) failed\n";
    return 1;
  }
  std::cout << "native renderer shader source normalization: PASS\n";
  return 0;
}
