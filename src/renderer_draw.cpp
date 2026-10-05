#include "renderer.hpp"

#include <GLES3/gl3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace pmjs {
namespace {
constexpr std::array<float, 4> zeroColor{};
std::array<int, 4> effectFrame(const std::array<int, 4>* clip,
                               int width, int height) {
  if (!clip) return {0, 0, width, height};
  return {std::clamp((*clip)[0], 0, width),
          std::clamp((*clip)[1], 0, height),
          std::clamp((*clip)[2], 0, width),
          std::clamp((*clip)[3], 0, height)};
}

}  // namespace

void Renderer::applyBlendMode(BlendMode mode) {
  switch (mode) {
    case BlendMode::normal:
      glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
                          GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      break;
    case BlendMode::additive:
      glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
      break;
    case BlendMode::multiply:
      glBlendFuncSeparate(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA,
                          GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      break;
    case BlendMode::screen:
      glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_COLOR,
                          GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      break;
  }
}

void Renderer::render() {
  renderScene();
  presentToDrawable();
}

int Renderer::filterBoundsPadding(scene_packet::FilterKind kind,
                                  const std::array<float, 21>& parameters) {
  using scene_packet::FilterKind;
  switch (kind) {
    case FilterKind::colorMatrix:
      return parameters[19] == 0.0F ? 0 : -1;
    case FilterKind::mzColor:
    case FilterKind::adjustment:
    case FilterKind::alpha:
    case FilterKind::alphaMask:
      return 0;
    default:
      return -1;
  }
}

void Renderer::computeFilterContentBounds() {
  constexpr std::size_t maxRegions = FilterContentBounds::maxRegions;
  const std::size_t commandCount = frame_.commands.size();
  if (filterBounds_.capacity() > 2048 &&
      commandCount * 4 < filterBounds_.capacity()) {
    std::vector<FilterContentBounds>().swap(filterBounds_);
  }
  filterBounds_.resize(commandCount);
  if (filterRegionSets_.capacity() > 256 &&
      commandCount * 4 < filterRegionSets_.capacity()) {
    std::vector<FilterRegionSet>().swap(filterRegionSets_);
  }
  filterRegionSets_.clear();
  for (std::size_t index = 0; index < commandCount; ++index) {
    auto& bounds = filterBounds_[index];
    bounds.bounded = false;
    bounds.regionsValid = false;
    bounds.rect = {};
    bounds.regionSetIndex = 0;
  }
  struct Accumulator {
    std::size_t beginIndex = 0;
    bool hasContent = false;
    bool unbounded = false;
    float minX = 0.0F;
    float minY = 0.0F;
    float maxX = 0.0F;
    float maxY = 0.0F;
    std::array<std::array<float, 4>, maxRegions> regions{};
    std::size_t regionCount = 0;
  };
  // Filter depth is bounded by the scene-packet contract, so keep the whole
  // stack inline and avoid vector state/allocation entirely.
  std::array<Accumulator, scene_packet::maxFilterDepth> stack{};
  std::size_t stackDepth = 0;
  const auto overlapsOrTouches = [](const auto& a, const auto& b) {
    return a[0] <= b[2] && b[0] <= a[2] &&
           a[1] <= b[3] && b[1] <= a[3];
  };
  const auto mergeRegion = [](const auto& a, const auto& b) {
    return std::array<float, 4>{std::min(a[0], b[0]),
                                std::min(a[1], b[1]),
                                std::max(a[2], b[2]),
                                std::max(a[3], b[3])};
  };
  const auto regionArea = [](const auto& rect) {
    return std::max(0.0F, rect[2] - rect[0]) *
           std::max(0.0F, rect[3] - rect[1]);
  };
  const auto addRegion = [&](Accumulator& acc,
                             std::array<float, 4> incoming) {
    if (incoming[0] >= incoming[2] || incoming[1] >= incoming[3]) return;

    // Keep the representation bounded while commands are accumulated instead
    // of collecting hundreds of rectangles and compacting them with repeated
    // O(n^2) scans/erase shifts at filterEnd. Eight conservative regions are
    // sufficient for the bounded-filter optimization; merging may only enlarge
    // work, never omit pixels.
    for (std::size_t index = 0; index < acc.regionCount;) {
      if (!overlapsOrTouches(acc.regions[index], incoming)) {
        ++index;
        continue;
      }
      incoming = mergeRegion(acc.regions[index], incoming);
      acc.regions[index] = acc.regions[acc.regionCount - 1];
      --acc.regionCount;
      index = 0;
    }

    if (acc.regionCount < maxRegions) {
      acc.regions[acc.regionCount++] = incoming;
      return;
    }

    std::size_t best = 0;
    float bestWaste = std::numeric_limits<float>::infinity();
    const float incomingArea = regionArea(incoming);
    for (std::size_t index = 0; index < acc.regionCount; ++index) {
      const auto joined = mergeRegion(acc.regions[index], incoming);
      const float waste = regionArea(joined) -
                          regionArea(acc.regions[index]) - incomingArea;
      if (waste < bestWaste) {
        bestWaste = waste;
        best = index;
      }
    }
    acc.regions[best] = mergeRegion(acc.regions[best], incoming);

    // The chosen merge can now touch another retained region. Fold those
    // overlaps in-place; the set remains <= maxRegions at all times.
    for (std::size_t index = 0; index < acc.regionCount;) {
      if (index == best ||
          !overlapsOrTouches(acc.regions[best], acc.regions[index])) {
        ++index;
        continue;
      }
      acc.regions[best] = mergeRegion(acc.regions[best], acc.regions[index]);
      acc.regions[index] = acc.regions[acc.regionCount - 1];
      --acc.regionCount;
      if (best == acc.regionCount) best = index;
      index = 0;
    }
  };
  const auto unite = [&](Accumulator& acc, float x0, float y0, float x1,
                         float y1) {
    if (!acc.hasContent) {
      acc.minX = x0;
      acc.minY = y0;
      acc.maxX = x1;
      acc.maxY = y1;
      acc.hasContent = true;
    } else {
      acc.minX = std::min(acc.minX, x0);
      acc.minY = std::min(acc.minY, y0);
      acc.maxX = std::max(acc.maxX, x1);
      acc.maxY = std::max(acc.maxY, y1);
    }
    addRegion(acc, {x0, y0, x1, y1});
  };
  for (std::size_t index = 0; index < frame_.commands.size(); ++index) {
    const RenderCommand& command = frame_.commands[index];
    if (command.action == RenderCommand::Action::filterBegin) {
      if (stackDepth >= scene_packet::maxFilterDepth) continue;
      Accumulator level;
      level.beginIndex = index;
      stack[stackDepth++] = level;
      continue;
    }
    if (command.action == RenderCommand::Action::filterEnd) {
      if (stackDepth == 0) continue;
      Accumulator level = std::move(stack[--stackDepth]);
      const RenderCommand& begun = frame_.commands[level.beginIndex];
      FilterContentBounds& out = filterBounds_[level.beginIndex];
      float ex0 = 0.0F;
      float ey0 = 0.0F;
      float ex1 = 0.0F;
      float ey1 = 0.0F;
      bool effective = false;
      const bool regionsValid = !level.unbounded &&
          filterBoundsPadding(filterKind(begun), filterParams(begun)) == 0;
      if (customPlan(begun)) {
        const auto& frame = customPlan(begun)->frame;
        ex0 = frame[0]; ey0 = frame[1];
        ex1 = frame[0] + frame[2]; ey1 = frame[1] + frame[3];
        effective = true;
      } else if (filterKind(begun) == scene_packet::FilterKind::custom && level.hasContent &&
          !level.unbounded) {
        const float padding = filterParams(begun)[0];
        ex0 = level.minX - padding;
        ey0 = level.minY - padding;
        ex1 = level.maxX + padding;
        ey1 = level.maxY + padding;
        effective = true;
      } else if (!level.hasContent && !level.unbounded) {
        effective = true;
      } else if (!level.unbounded &&
                 filterBoundsPadding(filterKind(begun),
                                     filterParams(begun)) == 0) {
        ex0 = level.minX;
        ey0 = level.minY;
        ex1 = level.maxX;
        ey1 = level.maxY;
        effective = true;
      } else if (begun.clipped) {
        ex0 = static_cast<float>(commandClip(begun)[0]);
        ey0 = static_cast<float>(commandClip(begun)[1]);
        ex1 = static_cast<float>(commandClip(begun)[2]);
        ey1 = static_cast<float>(commandClip(begun)[3]);
        effective = true;
      }
      if (effective && begun.clipped) {
        ex0 = std::max(ex0, static_cast<float>(commandClip(begun)[0]));
        ey0 = std::max(ey0, static_cast<float>(commandClip(begun)[1]));
        ex1 = std::min(ex1, static_cast<float>(commandClip(begun)[2]));
        ey1 = std::min(ey1, static_cast<float>(commandClip(begun)[3]));
      }
      if (!effective) {
        out.bounded = false;
      } else {
        out.bounded = true;
        out.regionsValid = regionsValid;
        const float loX = std::clamp(ex0, -1000000.0F, 1000000.0F);
        const float loY = std::clamp(ey0, -1000000.0F, 1000000.0F);
        const float hiX = std::clamp(ex1, -1000000.0F, 1000000.0F);
        const float hiY = std::clamp(ey1, -1000000.0F, 1000000.0F);
        out.rect = {static_cast<int>(std::floor(loX)),
                    static_cast<int>(std::floor(loY)),
                    static_cast<int>(std::ceil(hiX)),
                    static_cast<int>(std::ceil(hiY))};
        FilterRegionSet regionSet{};
        const std::size_t regionCount = regionsValid ? level.regionCount : 0;
        for (std::size_t regionIndex = 0; regionIndex < regionCount; ++regionIndex) {
          const auto& region = level.regions[regionIndex];
          int left = static_cast<int>(std::floor(region[0]));
          int top = static_cast<int>(std::floor(region[1]));
          int right = static_cast<int>(std::ceil(region[2]));
          int bottom = static_cast<int>(std::ceil(region[3]));
          if (begun.clipped) {
            left = std::max(left, commandClip(begun)[0]);
            top = std::max(top, commandClip(begun)[1]);
            right = std::min(right, commandClip(begun)[2]);
            bottom = std::min(bottom, commandClip(begun)[3]);
          }
          left = std::clamp(left, 0, width_);
          top = std::clamp(top, 0, height_);
          right = std::clamp(right, 0, width_);
          bottom = std::clamp(bottom, 0, height_);
          if (left < right && top < bottom &&
              regionSet.count < FilterContentBounds::maxRegions) {
            regionSet.regions[regionSet.count++] = {left, top, right, bottom};
          }
        }
        if (regionSet.count != 0) {
          filterRegionSets_.push_back(regionSet);
          out.regionSetIndex =
            static_cast<std::uint32_t>(filterRegionSets_.size());
        }
      }
      if (stackDepth != 0) {
        Accumulator& parent = stack[stackDepth - 1];
        if (!effective) {
          parent.unbounded = true;
        } else if (ex0 < ex1 && ey0 < ey1) {
          unite(parent, ex0, ey0, ex1, ey1);
        }
      }
      continue;
    }
    if (stackDepth == 0) continue;
    Accumulator& top = stack[stackDepth - 1];
    if (top.unbounded) continue;
    if (command.colorMatrixIndex != 0) {
      top.unbounded = true;
      continue;
    }
    if (command.tileLayer != 0 ||
        command.primitive == RenderCommand::Primitive::tileLayer ||
        command.primitive == RenderCommand::Primitive::mesh ||
        command.primitive == RenderCommand::Primitive::effect) {
      top.unbounded = true;
      continue;
    }
    if (command.primitive == RenderCommand::Primitive::screenFill) {
      unite(top, 0.0F, 0.0F, static_cast<float>(width_),
            static_cast<float>(height_));
      continue;
    }
    const float dw = command.destination[0];
    const float dh = command.destination[1];
    if (!(dw > 0.0F) || !(dh > 0.0F)) continue;
    const auto* commandSpriteVertices = spriteVertices(command);
    if (command.spriteWorldVertices && commandSpriteVertices == nullptr) {
      top.unbounded = true;
      continue;
    }
    const auto& t = command.transform;
    const auto point = [&](std::size_t corner, float x, float y) {
      return command.spriteWorldVertices ? (*commandSpriteVertices)[corner] :
        std::array<float, 2>{t[0] * x + t[2] * y + t[4], t[1] * x + t[3] * y + t[5]};
    };
    const auto [x0, y0] = point(0, 0, 0);
    const auto [x1, y1] = point(1, dw, 0);
    const auto [x2, y2] = point(2, dw, dh);
    const auto [x3, y3] = point(3, 0, dh);
    if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) ||
        !std::isfinite(y1) || !std::isfinite(x2) || !std::isfinite(y2) ||
        !std::isfinite(x3) || !std::isfinite(y3)) {
      top.unbounded = true;
      continue;
    }
    unite(top, std::min(std::min(x0, x1), std::min(x2, x3)),
          std::min(std::min(y0, y1), std::min(y2, y3)),
          std::max(std::max(x0, x1), std::max(x2, x3)),
          std::max(std::max(y0, y1), std::max(y2, y3)));
  }
  for (std::size_t index = 0; index < stackDepth; ++index) {
    filterBounds_[stack[index].beginIndex].bounded = false;
  }
}

bool Renderer::filterBoundsRegions(
    const RenderCommand* filterBegin,
    std::array<std::array<int, 4>, FilterContentBounds::maxRegions>* regions,
    std::size_t* regionCount) const {
  if (regions == nullptr || regionCount == nullptr || filterBegin == nullptr ||
      filterKind(*filterBegin) != scene_packet::FilterKind::colorMatrix ||
      filterBoundsPadding(filterKind(*filterBegin),
                          filterParams(*filterBegin)) != 0) {
    return false;
  }
  std::array<int, 4> aabb{};
  if (!filterBoundsRect(filterBegin, &aabb)) return false;
  const std::size_t index = static_cast<std::size_t>(
      filterBegin - frame_.commands.data());
  if (index >= filterBounds_.size() ||
      !filterBounds_[index].regionsValid ||
      filterBounds_[index].regionSetIndex == 0 ||
      filterBounds_[index].regionSetIndex > filterRegionSets_.size()) {
    return false;
  }
  const auto& regionSet =
    filterRegionSets_[filterBounds_[index].regionSetIndex - 1U];
  if (regionSet.count < 2) return false;
  std::uint64_t regionArea = 0;
  for (std::size_t regionIndex = 0;
       regionIndex < regionSet.count; ++regionIndex) {
    const auto& region = regionSet.regions[regionIndex];
    regionArea += static_cast<std::uint64_t>(region[2] - region[0]) *
                  static_cast<std::uint64_t>(region[3] - region[1]);
  }
  const std::uint64_t aabbArea =
      static_cast<std::uint64_t>(std::max(0, aabb[2] - aabb[0])) *
      static_cast<std::uint64_t>(std::max(0, aabb[3] - aabb[1]));
  if (aabbArea == 0 || regionArea * 4 >= aabbArea * 3) return false;
  *regions = regionSet.regions;
  *regionCount = regionSet.count;
  return true;
}

bool Renderer::filterBoundsRect(const RenderCommand* filterBegin,
                                std::array<int, 4>* rect) const {
  if (!filterBoundsEnabled_ || filterBegin == nullptr || rect == nullptr) {
    return false;
  }
  if (filterBegin < frame_.commands.data() ||
      filterBegin >= frame_.commands.data() + frame_.commands.size()) {
    return false;
  }
  const std::size_t index =
      static_cast<std::size_t>(filterBegin - frame_.commands.data());
  if (index >= filterBounds_.size() || !filterBounds_[index].bounded) {
    return false;
  }
  const int pad = filterBoundsPadding(filterKind(*filterBegin),
                                        filterParams(*filterBegin));
  if (pad < 0) return false;
  int left = filterBounds_[index].rect[0] - pad;
  int top = filterBounds_[index].rect[1] - pad;
  int right = filterBounds_[index].rect[2] + pad;
  int bottom = filterBounds_[index].rect[3] + pad;
  left = std::clamp(left, 0, width_);
  top = std::clamp(top, 0, height_);
  right = std::clamp(right, 0, width_);
  bottom = std::clamp(bottom, 0, height_);
  if (filterBegin->clipped) {
    left = std::max(left, commandClip(*filterBegin)[0]);
    top = std::max(top, commandClip(*filterBegin)[1]);
    right = std::min(right, commandClip(*filterBegin)[2]);
    bottom = std::min(bottom, commandClip(*filterBegin)[3]);
  }
  *rect = {left, top, right, bottom};
  return true;
}

void Renderer::renderScene() {
  RenderTarget& rootTarget = offscreenRender_ ? offscreenTarget_ : sceneTarget_;
  ensureTarget(rootTarget, width_, height_);
  std::uint32_t& rootFramebuffer = rootTarget.framebuffer;
  std::uint32_t& rootTexture = rootTarget.texture;
  const bool shouldRenderScene =
      sceneSubmittedThisFrame_ || offscreenRender_ || !hasValidSceneFrame_;
  if (shouldRenderScene) {
    if (!offscreenRender_ && sceneHasEffect_) ensureDepthBuffer(rootTarget);
    if (!offscreenRender_) toneCompositionActive_ = false;
    glBindFramebuffer(GL_FRAMEBUFFER, rootFramebuffer);
    glViewport(0, 0, width_, height_);
    glClearColor(clearColor_[0], clearColor_[1], clearColor_[2], clearColor_[3]);
    glDepthMask(GL_TRUE);
    glClearDepthf(1);
    glClear(GL_COLOR_BUFFER_BIT | (rootTarget.depth ? GL_DEPTH_BUFFER_BIT : 0));

    const std::size_t requiredVertexFloats = frame_.commands.size() * 48U;
    vertices_.clear();
    if (vertices_.capacity() > 48U * 2048U &&
        requiredVertexFloats * 4U < vertices_.capacity()) {
      std::vector<float>().swap(vertices_);
    }
    if (vertices_.capacity() < requiredVertexFloats) {
      vertices_.reserve(requiredVertexFloats);
    }
    if (sceneHasFilter_ &&
        (filterBoundsEnabled_ || sceneHasCustomFilter_)) {
      computeFilterContentBounds();
    } else {
      filterBounds_.clear();
    }
  // These arrays scale with scene command count and used to malloc/free every
  // frame. Keep one thread-local backing store and clear values in place.
  // Trim only after a large scene collapses so peak maps do not pin memory.
  const std::size_t commandCount = frame_.commands.size();
  // Store the filter command as index+1 instead of an 8-byte pointer. The high
  // bit marks filter begin/end commands that disappear after inlining, avoiding
  // a second per-command boundary bitset. Scene packets are capped at 65536
  // nodes, so the remaining index bits have ample headroom.
  constexpr std::uint32_t inlineFilterBoundaryBit = 0x80000000U;
  static thread_local std::vector<std::uint32_t> inlineFilterCommand;
  if (sceneHasColorMatrixFilter_) {
    if (inlineFilterCommand.capacity() > 1024 &&
        commandCount * 4 < inlineFilterCommand.capacity()) {
      std::vector<std::uint32_t>().swap(inlineFilterCommand);
    }
    inlineFilterCommand.assign(commandCount, 0);
    const auto preservesAlpha = [](const std::array<float, 21>& matrix) {
    constexpr float epsilon = 0.000001F;
    return std::abs(matrix[15]) <= epsilon &&
           std::abs(matrix[16]) <= epsilon &&
           std::abs(matrix[17]) <= epsilon &&
           std::abs(matrix[18] - 1.0F) <= epsilon &&
           std::abs(matrix[19]) <= epsilon;
  };
  const auto distributesOverSourceOver = [&](
      const std::array<float, 21>& matrix) {
    constexpr float epsilon = 0.000001F;
    return preservesAlpha(matrix) &&
           std::abs(matrix[3]) <= epsilon &&
           std::abs(matrix[4]) <= epsilon &&
           std::abs(matrix[8]) <= epsilon &&
           std::abs(matrix[9]) <= epsilon &&
           std::abs(matrix[13]) <= epsilon &&
           std::abs(matrix[14]) <= epsilon;
  };
  std::size_t scannedFilterDepth = 0;
  for (std::size_t begin = 0; begin < frame_.commands.size(); ++begin) {
    const RenderCommand& filter = frame_.commands[begin];
    if (filter.action == RenderCommand::Action::filterEnd) {
      if (scannedFilterDepth > 0) --scannedFilterDepth;
      continue;
    }
    if (filter.action != RenderCommand::Action::filterBegin) continue;
    const bool topLevelFilter = scannedFilterDepth == 0;
    ++scannedFilterDepth;
    if (!topLevelFilter ||
        filter.blendMode != BlendMode::normal ||
        filterKind(filter) != scene_packet::FilterKind::colorMatrix ||
        !preservesAlpha(filterParams(filter))) continue;
    std::size_t depth = 1;
    std::size_t end = begin;
    static thread_local std::vector<std::uint32_t> drawIndices;
    drawIndices.clear();
    if (drawIndices.capacity() < 16) drawIndices.reserve(16);
    if (drawIndices.capacity() > 1024 &&
        frame_.commands.size() * 4 < drawIndices.capacity()) {
      std::vector<std::uint32_t>().swap(drawIndices);
      drawIndices.reserve(16);
    }
    bool eligible = true;
    for (std::size_t index = begin + 1;
         index < frame_.commands.size() && depth > 0; ++index) {
      const RenderCommand& command = frame_.commands[index];
      if (command.action == RenderCommand::Action::filterBegin) {
        eligible = false;
        ++depth;
      } else if (command.action == RenderCommand::Action::filterEnd) {
        --depth;
        if (depth == 0) end = index;
      } else if (depth == 1) {
        const bool drawable = command.colorMatrixIndex == 0 &&
            command.tileLayer == 0 && command.image != 0 &&
            (command.primitive == RenderCommand::Primitive::sprite ||
             command.primitive == RenderCommand::Primitive::tilingSprite) &&
            command.blendMode == BlendMode::normal;
        if (!drawable) {
          eligible = false;
        } else {
          drawIndices.push_back(static_cast<std::uint32_t>(index));
        }
      }
    }
    if (!eligible || end == begin || drawIndices.empty() ||
        (drawIndices.size() > 1 &&
         !distributesOverSourceOver(filterParams(filter)))) {
      continue;
    }
    inlineFilterCommand[begin] = inlineFilterBoundaryBit;
    inlineFilterCommand[end] = inlineFilterBoundaryBit;
    for (const std::size_t drawIndex : drawIndices) {
      inlineFilterCommand[drawIndex] = static_cast<std::uint32_t>(begin + 1);
    }
    begin = end;
    scannedFilterDepth = 0;
    }
  }
  std::uint32_t composedToneCommandIndex = 0;
  if (!offscreenRender_ && sceneHasToneAdjust_) {
    std::size_t toneIndex = frame_.commands.size();
    std::size_t toneCount = 0;
    std::size_t filterDepthAtTone = 0;
    std::size_t filterDepth = 0;
    for (std::size_t index = 0; index < frame_.commands.size(); ++index) {
      const RenderCommand& command = frame_.commands[index];
      if (command.action == RenderCommand::Action::filterBegin) ++filterDepth;
      if (command.colorMatrixIndex != 0) {
        toneIndex = index;
        filterDepthAtTone = filterDepth;
        ++toneCount;
      }
      if (command.action == RenderCommand::Action::filterEnd && filterDepth > 0) {
        --filterDepth;
      }
    }
    bool cleanTail = toneCount == 1 && filterDepthAtTone == 0;
    for (std::size_t index = toneIndex + 1;
         cleanTail && index < frame_.commands.size(); ++index) {
      const RenderCommand& command = frame_.commands[index];
      cleanTail = command.action == RenderCommand::Action::draw &&
                  command.primitive != RenderCommand::Primitive::effect &&
                  command.colorMatrixIndex == 0 &&
                  command.blendMode == BlendMode::normal;
    }
    if (cleanTail) composedToneCommandIndex =
      static_cast<std::uint32_t>(toneIndex + 1U);
  }
  struct DrawOperation {
    std::uint32_t tileLayer = 0;
    std::uint32_t texture;
    BlendMode blendMode;
    bool repeat : 1;
    bool nearest : 1;
    GLsizei first;
    GLsizei count;
    std::uint32_t commandIndex = 0;
    std::uint32_t clipOverrideIndex = 0;
    bool clipped = false;
    float textureWidth = 1;
    float textureHeight = 1;
    float blur = 0;
    ImageHandle maskImage = 0;
    std::uint32_t matrixCommandIndex = 0;
    RenderCommand::Action action = RenderCommand::Action::draw;
    bool appliesSpriteColor : 1 = false;
    bool pixiSpritePacking : 1 = false;
    bool spriteWorldVertices : 1 = false;
    bool premultipliedSpriteTexture : 1 = false;
    bool clampedTilingSampling : 1 = false;
    std::uint32_t inlineMatrixIndex = 0;
    RenderCommand::Primitive primitive = RenderCommand::Primitive::sprite;
    std::uint32_t viewportMappingIndex = 0;
  };
  static thread_local std::vector<DrawOperation> operations;
  static thread_local std::vector<std::array<int, 4>> operationClipOverrides;
  static thread_local std::vector<std::array<float, 4>> operationViewportMappings;
  if (operations.capacity() > 1024 &&
      frame_.commands.size() * 4 < operations.capacity()) {
    std::vector<DrawOperation>().swap(operations);
  }
  if (operationClipOverrides.capacity() > 1024 &&
      frame_.commands.size() * 4 < operationClipOverrides.capacity()) {
    std::vector<std::array<int, 4>>().swap(operationClipOverrides);
  }
  if (operationViewportMappings.capacity() > 256 &&
      frame_.commands.size() * 4 < operationViewportMappings.capacity()) {
    std::vector<std::array<float, 4>>().swap(operationViewportMappings);
  }
  operations.clear();
  operationClipOverrides.clear();
  operationViewportMappings.clear();
  if (operations.capacity() < frame_.commands.size()) {
    operations.reserve(frame_.commands.size());
  }
  const auto resolveOperationCommand = [&](const DrawOperation& operation)
      -> const RenderCommand* {
    if (operation.commandIndex == 0 ||
        operation.commandIndex > frame_.commands.size()) {
      return nullptr;
    }
    return &frame_.commands[operation.commandIndex - 1U];
  };
  const auto resolvedOperationClip = [&](const DrawOperation& operation)
      -> const std::array<int, 4>& {
    if (operation.clipOverrideIndex != 0) {
      return operationClipOverrides[operation.clipOverrideIndex - 1U];
    }
    return commandClip(*resolveOperationCommand(operation));
  };
  const std::array<float, 4> defaultViewportMapping{1, 1, 0, 0};
  const auto resolvedViewportMapping = [&](const DrawOperation& operation)
      -> const std::array<float, 4>& {
    if (operation.viewportMappingIndex != 0) {
      return operationViewportMappings[operation.viewportMappingIndex - 1U];
    }
    return defaultViewportMapping;
  };
  const auto appendQuad = [&](const std::array<float, 48>& quad) {
    const std::size_t baseVertex = vertices_.size() / 12U;
    const GLsizei firstIndex =
      static_cast<GLsizei>((baseVertex / 4U) * 6U);
    vertices_.insert(vertices_.end(), quad.begin(), quad.end());
    return firstIndex;
  };
  std::size_t preparingFilterDepth = 0;
  std::array<const RenderCommand*, scene_packet::maxFilterDepth> preparingFilters{};
  std::array<float, 4> viewportMapping{1, 1, 0, 0};
  for (std::size_t commandIndex = 0;
       commandIndex < frame_.commands.size(); ++commandIndex) {
    const RenderCommand& command = frame_.commands[commandIndex];
    const auto inlineFilterTag = sceneHasColorMatrixFilter_
      ? inlineFilterCommand[commandIndex] : 0U;
    if (inlineFilterTag & inlineFilterBoundaryBit) continue;
    if (command.action != RenderCommand::Action::draw) {
      viewportMapping = {1, 1, 0, 0};
      if (command.action == RenderCommand::Action::filterBegin) preparingFilters[preparingFilterDepth] = &command;
      if (command.action == RenderCommand::Action::filterEnd &&
          preparingFilterDepth > 0) --preparingFilterDepth;
      DrawOperation operation{};
      operation.commandIndex =
        static_cast<std::uint32_t>(commandIndex + 1U);
      operation.action = command.action;
      if (command.action == RenderCommand::Action::filterEnd) {
        const std::array<float, 48> vertices = {
          -1,  1, 0, 1, 1, 1, 1, 1, 0, 0, 1, 1,
           1,  1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1,
           1, -1, 1, 0, 1, 1, 1, 1, 0, 0, 1, 1,
          -1, -1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1,
        };
        operation.first = appendQuad(vertices);
        operation.count = 6;
      }
      operations.push_back(operation);
      if (command.action == RenderCommand::Action::filterBegin) {
        ++preparingFilterDepth;
      }
      continue;
    }
    if (command.colorMatrixIndex != 0) {
      const std::array<float, 48> vertices = {
        -1,  1, 0, 1, 1, 1, 1, 1, 0, 0, 1, 1,
         1,  1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1,
         1, -1, 1, 0, 1, 1, 1, 1, 0, 0, 1, 1,
        -1, -1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1,
      };
      DrawOperation operation{};
      operation.first = appendQuad(vertices);
      operation.count = 6;
      operation.matrixCommandIndex =
        static_cast<std::uint32_t>(commandIndex + 1U);
      operations.push_back(operation);
      continue;
    }
    if (command.primitive == RenderCommand::Primitive::effect) {
      if (command.effectIndex == 0 ||
          command.effectIndex > frame_.effects.size()) continue;
      DrawOperation operation{};
      operation.commandIndex =
        static_cast<std::uint32_t>(commandIndex + 1U);
      operation.primitive = command.primitive;
      operation.clipped = command.clipped;
      operations.push_back(operation);
      const auto* filter = preparingFilterDepth ? preparingFilters[preparingFilterDepth - 1] : nullptr;
      const auto frame = effectFrame(
        filter && filter->clipped ? &commandClip(*filter) : nullptr,
        width_, height_);
      const auto& effect = frame_.effects[command.effectIndex - 1];
      const float sx = effect.resetViewport[0] / std::max(1, frame[2] - frame[0]);
      const float sy = effect.resetViewport[1] / std::max(1, frame[3] - frame[1]);
      // Keep fractional projection offsets; rounding a virtual GL viewport changes edge pixels.
      viewportMapping = {sx, sy, frame[0] * (1 - sx), frame[1] * (1 - sy)};
      continue;
    }
    if (command.tileLayer != 0) {
      operations.push_back({command.tileLayer, 0, command.blendMode,
                            false, command.nearest, 0, 0,
                            static_cast<std::uint32_t>(commandIndex + 1U), 0,
                            command.clipped, 1, 1, 0, 0, {}});
      operations.back().primitive = command.primitive;
      if (viewportMapping != defaultViewportMapping) {
        operationViewportMappings.push_back(viewportMapping);
        operations.back().viewportMappingIndex =
          static_cast<std::uint32_t>(operationViewportMappings.size());
      }
      continue;
    }
    const auto info = command.image == 0 ? std::optional<ImageInfo>{} :
      (command.pixiSpritePacking && !command.premultipliedSpriteTexture ?
        images_.lookupPremultiplied(command.image) : images_.lookup(command.image));
    if (command.image != 0 && !info) continue;
    const float textureWidth = info ? static_cast<float>(info->width) : 1.0F;
    const float textureHeight = info ? static_cast<float>(info->height) : 1.0F;
    const std::uint32_t texture = info ? info->texture : whiteTexture_;
    const bool texturePremultiplied = command.premultipliedSpriteTexture || (info && info->premultiplied);
    const auto* commandSpriteVertices = spriteVertices(command);
    if (command.spriteWorldVertices && commandSpriteVertices == nullptr) continue;
    const auto* commandMaskTransform =
      command.maskImage ? maskTransform(command) : nullptr;
    if (command.maskImage && commandMaskTransform == nullptr) continue;
    const auto& t = command.transform;
    const auto point = [&](float x, float y, std::size_t corner) {
      float px = t[0] * x + t[2] * y + t[4];
      float py = t[1] * x + t[3] * y + t[5];
      if (command.spriteWorldVertices) { px = (*commandSpriteVertices)[corner][0]; py = (*commandSpriteVertices)[corner][1]; }
      if (command.roundPixels) {
        px = std::floor(px);
        py = std::floor(py);
      }
      px = px * viewportMapping[0] + viewportMapping[2];
      py = py * viewportMapping[1] + viewportMapping[3];
      return std::array<float, 2>{px / static_cast<float>(width_) * 2.0F - 1.0F,
                                  1.0F - py / static_cast<float>(height_) * 2.0F};
    };
    const float localWidth = command.destination[0];
    const float localHeight = command.destination[1];
    const auto p0 = point(0, 0, 0);
    const auto p1 = point(localWidth, 0, 1);
    const auto p2 = point(localWidth, localHeight, 2);
    const auto p3 = point(0, localHeight, 3);
    if (preparingFilterDepth == 0) {
      const bool left = p0[0] <= -1 && p1[0] <= -1 &&
                        p2[0] <= -1 && p3[0] <= -1;
      const bool right = p0[0] >= 1 && p1[0] >= 1 &&
                         p2[0] >= 1 && p3[0] >= 1;
      const bool above = p0[1] >= 1 && p1[1] >= 1 &&
                         p2[1] >= 1 && p3[1] >= 1;
      const bool below = p0[1] <= -1 && p1[1] <= -1 &&
                         p2[1] <= -1 && p3[1] <= -1;
      if (left || right || above || below) continue;
    }
    const float u0 = command.source[0] / textureWidth;
    const float v0 = command.source[1] / textureHeight;
    const float u1 = (command.source[0] + command.source[2]) / textureWidth;
    const float v1 = (command.source[1] + command.source[3]) / textureHeight;
    std::array<std::array<float, 2>, 4> sourceCorners = {
      std::array<float, 2>{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}
    };
    // GPU Bitmap views replace private textures; their storage atlas must not
    // introduce the atlas-wide uint16 UV rounding of an authored spritesheet.
    if (command.pixiSpritePacking && !command.standaloneBitmapRegion) {
      const auto packUv = [](double coordinate) {
        // JS ToUint16 truncates and wraps; an out-of-range C++ cast is undefined.
        double packed = std::fmod(std::trunc(coordinate * 65535.0), 65536.0);
        if (packed < 0) packed += 65536.0;
        return static_cast<float>(packed / 65535.0);
      };
      const float packedU0 = packUv(double(command.source[0]) / textureWidth);
      const float packedV0 = packUv(double(command.source[1]) / textureHeight);
      const float packedU1 = packUv((double(command.source[0]) + command.source[2]) / textureWidth);
      const float packedV1 = packUv((double(command.source[1]) + command.source[3]) / textureHeight);
      sourceCorners = {{{packedU0, packedV0}, {packedU1, packedV0},
                        {packedU1, packedV1}, {packedU0, packedV1}}};
    }
    constexpr std::array<std::array<std::uint8_t, 4>, 8> rotatedCorners = {{
      {{0, 1, 2, 3}}, {{1, 2, 3, 0}}, {{2, 3, 0, 1}}, {{3, 0, 1, 2}},
      {{3, 2, 1, 0}}, {{0, 3, 2, 1}}, {{1, 0, 3, 2}}, {{2, 1, 0, 3}}
    }};
    const auto& uvOrder = rotatedCorners[command.textureRotation];
    const auto& uv0 = sourceCorners[uvOrder[0]];
    const auto& uv1 = sourceCorners[uvOrder[1]];
    const auto& uv2 = sourceCorners[uvOrder[2]];
    const auto& uv3 = sourceCorners[uvOrder[3]];
    auto color = command.color;
    if (command.pixiSpritePacking && !command.packedSpriteColor) {
      const float alpha = std::clamp(color[3], 0.0F, 1.0F);
      for (std::size_t channel = 0; channel < 3; ++channel) {
        color[channel] = std::floor(color[channel] * 255.0F * alpha + 0.5F) / 255.0F;
      }
      color[3] = std::floor(alpha * 255.0F) / 255.0F;
    }
    const bool worldVertices = command.spriteWorldVertices && viewportMapping == std::array<float, 4>{1, 1, 0, 0};
    const auto vertex0 = worldVertices ? (*commandSpriteVertices)[0] : p0;
    const auto vertex1 = worldVertices ? (*commandSpriteVertices)[1] : p1;
    const auto vertex2 = worldVertices ? (*commandSpriteVertices)[2] : p2;
    const auto vertex3 = worldVertices ? (*commandSpriteVertices)[3] : p3;
    const std::size_t baseVertex = vertices_.size() / 12U;
    const GLsizei quadFirstIndex =
      static_cast<GLsizei>((baseVertex / 4U) * 6U);
    const std::size_t vertexOffset = vertices_.size();
    vertices_.resize(vertexOffset + 48U);
    const auto writeVertex = [&](std::size_t vertexIndex,
                                 const std::array<float, 2>& position,
                                 const std::array<float, 2>& uv) {
      float* out = vertices_.data() + vertexOffset + vertexIndex * 12U;
      out[0] = position[0];
      out[1] = position[1];
      out[2] = uv[0];
      out[3] = uv[1];
      out[4] = color[0];
      out[5] = color[1];
      out[6] = color[2];
      out[7] = color[3];
      out[8] = u0;
      out[9] = v0;
      out[10] = u1;
      out[11] = v1;
    };
    writeVertex(0, vertex0, uv0);
    writeVertex(1, vertex1, uv1);
    writeVertex(2, vertex2, uv2);
    writeVertex(3, vertex3, uv3);
    const auto inlineFilterIndex = inlineFilterTag;
    const RenderCommand* inlineFilter = inlineFilterIndex == 0 ? nullptr :
      &frame_.commands[inlineFilterIndex - 1U];
    bool operationClipped = command.clipped;
    std::array<int, 4> operationClip = command.clipped ? commandClip(command) : std::array<int, 4>{};
    if (inlineFilter && inlineFilter->clipped) {
      if (command.clipped) {
        operationClip = {
          std::max(commandClip(*inlineFilter)[0], commandClip(command)[0]),
          std::max(commandClip(*inlineFilter)[1], commandClip(command)[1]),
          std::min(commandClip(*inlineFilter)[2], commandClip(command)[2]),
          std::min(commandClip(*inlineFilter)[3], commandClip(command)[3]),
        };
      } else {
        operationClip = commandClip(*inlineFilter);
      }
      operationClipped = true;
    }
    const auto* commandColorEffect =
      command.appliesSpriteColor ? colorEffect(command) : nullptr;
    if (command.appliesSpriteColor && commandColorEffect == nullptr) continue;
    const RenderCommand* previousCommand =
      operations.empty() ? nullptr : resolveOperationCommand(operations.back());
    const auto* previousMaskTransform =
      command.maskImage && previousCommand
        ? maskTransform(*previousCommand) : nullptr;
    const auto* previousColorEffect =
      command.appliesSpriteColor && previousCommand
        ? colorEffect(*previousCommand) : nullptr;
    if (operations.empty() || operations.back().tileLayer != 0 ||
        operations.back().texture != texture ||
        operations.back().blendMode != command.blendMode ||
        operations.back().repeat != command.repeat ||
        operations.back().nearest != command.nearest ||
        operations.back().clampedTilingSampling != command.clampedTilingSampling ||
        operations.back().blur != command.blur ||
        operations.back().maskImage != command.maskImage ||
        (command.maskImage &&
         (previousMaskTransform == nullptr ||
          *previousMaskTransform != *commandMaskTransform)) ||
        operations.back().appliesSpriteColor != command.appliesSpriteColor ||
        operations.back().pixiSpritePacking != command.pixiSpritePacking ||
        operations.back().spriteWorldVertices != worldVertices ||
        operations.back().premultipliedSpriteTexture != texturePremultiplied ||
        (command.appliesSpriteColor &&
         (previousCommand == nullptr ||
          previousCommand->source != command.source)) ||
        (command.appliesSpriteColor &&
         (previousColorEffect == nullptr ||
          previousColorEffect->colorTone != commandColorEffect->colorTone ||
          previousColorEffect->blendColor != commandColorEffect->blendColor)) ||
        operations.back().inlineMatrixIndex != inlineFilterIndex ||
        operations.back().primitive != command.primitive ||
        operations.back().clipped != operationClipped ||
        (operationClipped &&
         resolvedOperationClip(operations.back()) != operationClip)) {
      operations.push_back({0, texture, command.blendMode, command.repeat,
        command.nearest,
        quadFirstIndex, 6,
        static_cast<std::uint32_t>(commandIndex + 1U),
        0, operationClipped, textureWidth, textureHeight,
        command.blur, command.maskImage});
      if (operationClipped &&
          (!command.clipped || operationClip != commandClip(command))) {
        operationClipOverrides.push_back(operationClip);
        operations.back().clipOverrideIndex =
          static_cast<std::uint32_t>(operationClipOverrides.size());
      }
      operations.back().appliesSpriteColor = command.appliesSpriteColor;
      operations.back().pixiSpritePacking = command.pixiSpritePacking;
      operations.back().spriteWorldVertices = worldVertices;
      operations.back().premultipliedSpriteTexture = texturePremultiplied;
      operations.back().clampedTilingSampling = command.clampedTilingSampling;
      operations.back().inlineMatrixIndex = inlineFilterIndex;
      operations.back().primitive = command.primitive;
    } else {
      operations.back().count += 6;
    }
  }

  if (!vertices_.empty()) {
    glBindVertexArray(vertexArray_);
    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertices_.size() * sizeof(float)),
                 vertices_.data(), GL_STREAM_DRAW);
    const std::size_t quadCount = vertices_.size() / (12U * 4U);
    if (quadCount > quadIndexCapacity_) {
      std::size_t newCapacity = quadIndexCapacity_ == 0 ? 256U :
        quadIndexCapacity_ * 2U;
      newCapacity = std::max(newCapacity, quadCount);
      std::vector<std::uint32_t> indices(newCapacity * 6U);
      for (std::size_t quad = 0; quad < newCapacity; ++quad) {
        const std::uint32_t base = static_cast<std::uint32_t>(quad * 4U);
        const std::size_t offset = quad * 6U;
        indices[offset] = base;
        indices[offset + 1] = base + 1U;
        indices[offset + 2] = base + 2U;
        indices[offset + 3] = base;
        indices[offset + 4] = base + 2U;
        indices[offset + 5] = base + 3U;
      }
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, quadIndexBuffer_);
      glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                   static_cast<GLsizeiptr>(indices.size() *
                                          sizeof(std::uint32_t)),
                   indices.data(), GL_STATIC_DRAW);
      quadIndexCapacity_ = newCapacity;
    }
    if (diagnostics_) ++stats_.bufferUploads;
  }

  const auto drawQuadOperation = [](const DrawOperation& operation) {
    glDrawElements(
      GL_TRIANGLES, operation.count, GL_UNSIGNED_INT,
      reinterpret_cast<const void*>(
        static_cast<std::uintptr_t>(operation.first) *
        sizeof(std::uint32_t)));
  };

  BlendMode activeBlend = BlendMode::normal;
  std::uint32_t activeProgram = 0;
  bool scissorActive = false;
  std::size_t filterDepth = 0;
  std::array<const RenderCommand*, scene_packet::maxFilterDepth> filterCommands{};
  std::array<float, scene_packet::maxFilterDepth> rasterResolutions{};
  float rasterResolution = 1;
  std::array<float, 4> rasterFrame{0, 0, static_cast<float>(width_), static_cast<float>(height_)};
  std::array<std::array<float, 4>, scene_packet::maxFilterDepth> rasterFrames{};
  bool targetYDown = offscreenRender_;
  const auto projectTarget = [&](std::uint32_t program) {
    auto [entry, inserted] = targetProjectionLocations_.try_emplace(program, -1);
    if (inserted) entry->second = glGetUniformLocation(program, "targetProjection");
    glUniform4f(entry->second, width_ / std::max(1.0F, rasterFrame[2]),
      height_ / std::max(1.0F, rasterFrame[3]),
      (width_ - 2 * rasterFrame[0]) / std::max(1.0F, rasterFrame[2]) - 1,
      (targetYDown ? -1.0F : 1.0F) * (1 - (height_ - 2 * rasterFrame[1]) / std::max(1.0F, rasterFrame[3])));
  };
  const auto rasterScissor = [&](int x, int y, int width, int height) {
    const float localY = targetYDown ? height_ - y - height - rasterFrame[1] :
      y - (height_ - rasterFrame[1] - rasterFrame[3]);
    glScissor(std::lround((x - rasterFrame[0]) * rasterResolution), std::lround(localY * rasterResolution),
      std::max(0L, std::lround(width * rasterResolution)), std::max(0L, std::lround(height * rasterResolution)));
  };
  const auto maskMatrix = [&](const float* matrix) {
    std::array<float, 6> result{};
    std::copy_n(matrix, 6, result.begin());
    result[4] += matrix[0] * rasterFrame[0] + matrix[2] * rasterFrame[1];
    result[5] += matrix[1] * rasterFrame[0] + matrix[3] * rasterFrame[1];
    for (int index = 0; index < 4; ++index) result[index] /= rasterResolution;
    return result;
  };
  std::array<bool, scene_packet::maxFilterDepth> savedScissor{};
  std::array<std::array<int, 4>, scene_packet::maxFilterDepth> savedClip{};
  std::array<int, 4> activeClip{};
  constexpr std::size_t maxFilterRegions = FilterContentBounds::maxRegions;
  std::array<std::array<std::array<int, 4>, maxFilterRegions>,
      scene_packet::maxFilterDepth> filterRegions{};
  std::array<std::size_t, scene_packet::maxFilterDepth> filterRegionCounts{};
  applyBlendMode(activeBlend);
  for (const auto& operation : operations) {
    const RenderCommand* operationCommand = resolveOperationCommand(operation);
    if (operation.action == RenderCommand::Action::filterBegin) {
      targetYDown = true;
      const auto* operationCustomPlan = customPlan(*operationCommand);
      rasterResolution = operationCustomPlan ?
        operationCustomPlan->resolutions[0] : 1.0F;
      rasterResolutions[filterDepth] = rasterResolution;
      rasterFrame = operationCustomPlan ? operationCustomPlan->frame :
        std::array<float, 4>{0, 0, static_cast<float>(width_), static_cast<float>(height_)};
      rasterFrames[filterDepth] = rasterFrame;
      if (activeProgram) projectTarget(activeProgram);
      const auto pot = [](int size) { int result = 1; while (result < size) result *= 2; return result; };
      const int targetWidth = std::max(1, static_cast<int>(std::ceil(rasterFrame[2] * rasterResolution)));
      const int targetHeight = std::max(1, static_cast<int>(std::ceil(rasterFrame[3] * rasterResolution)));
      if (diagnostics_) ++stats_.filterTargetAcquires;
      if (groupTargets_[filterDepth].texture &&
          groupTargets_[filterDepth].width == targetWidth &&
          groupTargets_[filterDepth].height == targetHeight) {
        if (diagnostics_) ++stats_.filterTargetReuses;
      }
      ensureTarget(groupTargets_[filterDepth],
        operationCustomPlan ? pot(targetWidth) : targetWidth,
        operationCustomPlan ? pot(targetHeight) : targetHeight);
      std::array<int, 4> boundedRect{};
      const bool bounded = !operationCustomPlan &&
        filterBoundsRect(operationCommand, &boundedRect);
      filterRegionCounts[filterDepth] = 0;
      const bool multiRegion = !customPlan(*operationCommand) && filterBoundsRegions(
          operationCommand, &filterRegions[filterDepth],
          &filterRegionCounts[filterDepth]);
      if (bounded) {
        savedScissor[filterDepth] = true;
        savedClip[filterDepth] = boundedRect;
      } else {
        savedScissor[filterDepth] = operationCommand->clipped;
        savedClip[filterDepth] = operationCommand->clipped ? commandClip(*operationCommand) : std::array<int, 4>{};
      }
      filterCommands[filterDepth] = operationCommand;
      if (scissorActive) {
        glDisable(GL_SCISSOR_TEST);
        scissorActive = false;
      }
      glBindFramebuffer(GL_FRAMEBUFFER, groupTargets_[filterDepth].framebuffer);
      glViewport(0, 0, std::lround(rasterFrame[2] * rasterResolution), std::lround(rasterFrame[3] * rasterResolution));
      if (bounded && !multiRegion) {
        glEnable(GL_SCISSOR_TEST);
        rasterScissor(boundedRect[0], height_ - boundedRect[3],
                  std::max(0, boundedRect[2] - boundedRect[0]),
                  std::max(0, boundedRect[3] - boundedRect[1]));
        scissorActive = true;
        activeClip = boundedRect;
      }
      glClearColor(0, 0, 0, 0);
      if (multiRegion) {
        glDisable(GL_SCISSOR_TEST);
        scissorActive = false;
      }
      glClear(GL_COLOR_BUFFER_BIT);
      if (diagnostics_) ++stats_.filterTargetClears;
      activeBlend = BlendMode::normal;
      applyBlendMode(activeBlend);
      ++filterDepth;
      continue;
    }
    if (operation.action == RenderCommand::Action::filterEnd) {
      --filterDepth;
      targetYDown = offscreenRender_ || filterDepth > 0;
      const float sourceResolution = rasterResolutions[filterDepth];
      rasterResolution = filterDepth ? rasterResolutions[filterDepth - 1] : 1.0F;
      rasterFrame = filterDepth ? rasterFrames[filterDepth - 1] :
        std::array<float, 4>{0, 0, static_cast<float>(width_), static_cast<float>(height_)};
      const RenderCommand& filter = *filterCommands[filterDepth];
      if (diagnostics_) ++stats_.filterApplications[static_cast<std::size_t>(filterKind(filter))];
      std::array<int, 4> boundedRect{};
      if (diagnostics_ && filterBoundsRect(filterCommands[filterDepth], &boundedRect)) {
        ++stats_.filterBoundedApplications;
      }
      if (scissorActive) {
        glDisable(GL_SCISSOR_TEST);
        scissorActive = false;
      }
      glViewport(0, 0, std::lround(rasterFrame[2] * rasterResolution), std::lround(rasterFrame[3] * rasterResolution));
      glUseProgram(program_);
      projectTarget(program_);
      activeProgram = program_;
      glUniform1i(filterTargetYDownUniform_, 0);
      // Authored filter content projects downward; internal passes project upward.
      bool sourceYDown = true;
      glUniform1i(filterImageYDownUniform_, sourceYDown);
      glBindVertexArray(vertexArray_);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, groupTargets_[filterDepth].texture);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      textureNearestState_[groupTargets_[filterDepth].texture] = false;
      glUniform2f(textureSizeUniform_,
                  static_cast<float>(width_) / filterResolution(filter),
                  static_cast<float>(height_) / filterResolution(filter));
      glUniform1i(maskEnabledUniform_, 0);
      glUniform1i(colorMatrixEnabledUniform_, 0);
      glUniform1i(spriteColorEnabledUniform_, 0);
      glUniform1i(displacementEnabledUniform_, 0);
      glUniform1i(noiseGlitchEnabledUniform_, 0);
      glUniform1i(pixiFilterKindUniform_, 0);
      glUniform1i(premultipliedInputUniform_, 1);

      const bool pictureBlend =
        filterKind(filter) == scene_packet::FilterKind::pictureBlend;
      if (pictureBlend) {
        ensureTarget(filterTarget_, width_, height_);
        glBindFramebuffer(GL_FRAMEBUFFER, filterDepth == 0 ? rootFramebuffer :
                          groupTargets_[filterDepth - 1].framebuffer);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, filterTarget_.texture);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width_, height_);
        if (diagnostics_) ++stats_.framebufferCopies;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glUniform1i(bloomImageUniform_, 3);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, groupTargets_[filterDepth].texture);
      }

      std::uint32_t compositeTexture = groupTargets_[filterDepth].texture;
      if (filterKind(filter) == scene_packet::FilterKind::blur) {
        ensureTarget(filterTarget_, width_, height_);
        glUniform1fv(pixiFilterParametersUniform_, 3, filterParams(filter).data());
        glUniform1f(blurUniform_, filterParams(filter)[0]);
        glUniform2f(blurDirectionUniform_, 1, 0);
        glDisable(GL_BLEND);
        std::uint32_t sourceTexture = groupTargets_[filterDepth].texture;
        const int passCount = static_cast<int>(filterParams(filter)[1]);
        for (int pass = 0; pass < passCount; ++pass) {
          const bool targetFilter = sourceTexture == groupTargets_[filterDepth].texture;
          glBindFramebuffer(GL_FRAMEBUFFER,
                            targetFilter ? filterTarget_.framebuffer :
                                           groupTargets_[filterDepth].framebuffer);
          glBindTexture(GL_TEXTURE_2D, sourceTexture);
          drawQuadOperation(operation);
          sourceYDown = false;
          glUniform1i(filterImageYDownUniform_, sourceYDown);
          if (diagnostics_) {
            ++stats_.drawCalls;
            ++stats_.filterDrawCalls;
          }
          sourceTexture = targetFilter ? filterTarget_.texture :
                                         groupTargets_[filterDepth].texture;
        }
        glUniform2f(blurDirectionUniform_, 0, 1);
        for (int pass = 1; pass < passCount; ++pass) {
          const bool targetFilter = sourceTexture == groupTargets_[filterDepth].texture;
          glBindFramebuffer(GL_FRAMEBUFFER,
                            targetFilter ? filterTarget_.framebuffer :
                                           groupTargets_[filterDepth].framebuffer);
          glBindTexture(GL_TEXTURE_2D, sourceTexture);
          drawQuadOperation(operation);
          sourceYDown = false;
          glUniform1i(filterImageYDownUniform_, sourceYDown);
          if (diagnostics_) {
            ++stats_.drawCalls;
            ++stats_.filterDrawCalls;
          }
          sourceTexture = targetFilter ? filterTarget_.texture :
                                         groupTargets_[filterDepth].texture;
        }
        compositeTexture = sourceTexture;
      } else if (filterKind(filter) == scene_packet::FilterKind::blurX ||
                 filterKind(filter) == scene_packet::FilterKind::blurY) {
        glUniform1fv(pixiFilterParametersUniform_, 3, filterParams(filter).data());
        if (filterParams(filter)[1] > 1) {
          ensureTarget(filterTarget_, width_, height_);
        }
        glUniform1f(blurUniform_, filterParams(filter)[0]);
        glUniform2f(blurDirectionUniform_,
                    filterKind(filter) == scene_packet::FilterKind::blurX ? 1 : 0,
                    filterKind(filter) == scene_packet::FilterKind::blurY ? 1 : 0);
        glDisable(GL_BLEND);
        std::uint32_t sourceTexture = groupTargets_[filterDepth].texture;
        const int passCount = static_cast<int>(filterParams(filter)[1]);
        for (int pass = 1; pass < passCount; ++pass) {
          const bool targetFilter = sourceTexture == groupTargets_[filterDepth].texture;
          glBindFramebuffer(GL_FRAMEBUFFER,
                            targetFilter ? filterTarget_.framebuffer :
                                           groupTargets_[filterDepth].framebuffer);
          glBindTexture(GL_TEXTURE_2D, sourceTexture);
          drawQuadOperation(operation);
          sourceYDown = false;
          glUniform1i(filterImageYDownUniform_, sourceYDown);
          if (diagnostics_) {
            ++stats_.drawCalls;
            ++stats_.filterDrawCalls;
          }
          sourceTexture = targetFilter ? filterTarget_.texture :
                                         groupTargets_[filterDepth].texture;
        }
        compositeTexture = sourceTexture;
      } else if (filterKind(filter) == scene_packet::FilterKind::displacement) {
        glUniform1f(blurUniform_, 0);
        glUniform2f(blurDirectionUniform_, 0, 0);
        const auto displacement = images_.lookup(filter.image);
        if (!displacement) continue;
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, displacement->texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        textureNearestState_[displacement->texture] = false;
        textureRepeatState_[displacement->texture] = true;
        glUniform1i(displacementImageUniform_, 2);
        glUniform4fv(displacementBoundsUniform_, 1,
                     filterParams(filter).data());
        glUniform2fv(displacementScaleUniform_, 1,
                     filterParams(filter).data() + 4);
        glUniform1i(displacementEnabledUniform_, 1);
        glActiveTexture(GL_TEXTURE0);
      } else if (filterKind(filter) == scene_packet::FilterKind::alphaMask) {
        glUniform1f(blurUniform_, 0);
        glUniform2f(blurDirectionUniform_, 0, 0);
        const auto mask = images_.lookup(filter.image);
        if (!mask) continue;
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, mask->texture);
        glUniform1i(maskImageUniform_, 1);
        glUniform1fv(maskTransformUniform_, 6,
                     maskMatrix(filterParams(filter).data()).data());
        glUniform4fv(maskFrameUniform_, 1,
                     filterParams(filter).data() + 6);
        glUniform2f(maskTextureSizeUniform_, static_cast<float>(mask->width),
                    static_cast<float>(mask->height));
        glUniform1f(maskScreenHeightUniform_, rasterFrame[3] * rasterResolution);
        glUniform1f(maskAlphaUniform_, filterParams(filter)[10]);
        glUniform1i(maskUsesRedUniform_, filterParams(filter)[11] != 0);
        glUniform1i(maskRotationUniform_,
                    static_cast<int>(filterParams(filter)[12]) / 2);
        glUniform2f(maskLocalSizeUniform_, filterParams(filter)[13],
                    filterParams(filter)[14]);
        glUniform1i(maskEnabledUniform_, 1);
        glActiveTexture(GL_TEXTURE0);
      } else if (filterKind(filter) == scene_packet::FilterKind::noiseGlitch) {
        glUniform1f(blurUniform_, 0);
        glUniform2f(blurDirectionUniform_, 0, 0);
        glUniform4fv(noiseGlitchParametersUniform_, 1,
                     filterParams(filter).data());
        glUniform1i(noiseGlitchEnabledUniform_, 1);
      } else if (filterKind(filter) == scene_packet::FilterKind::zoomBlur ||
                 filterKind(filter) == scene_packet::FilterKind::shockwave) {
        glUniform1f(blurUniform_, 0);
        glUniform2f(blurDirectionUniform_, 0, 0);
        glUniform1fv(pixiFilterParametersUniform_, 10, filterParams(filter).data());
        glUniform1i(pixiFilterKindUniform_,
                    filterKind(filter) == scene_packet::FilterKind::zoomBlur ? 1 : 2);
      } else if (filterKind(filter) == scene_packet::FilterKind::advancedBloom) {
        ensureTarget(bloomTarget_, width_, height_);
        ensureTarget(filterTarget_, width_, height_);
        glUniform1f(blurUniform_, 0);
        glUniform2f(blurDirectionUniform_, 0, 0);
        glDisable(GL_BLEND);
        const std::array<float, 10> extractParameters = {
          filterParams(filter)[2], 0, 0, 0, 0, 0, 0, 0, 0, 0};
        glUniform1fv(pixiFilterParametersUniform_, 10,
                     extractParameters.data());
        glUniform1i(pixiFilterKindUniform_, 3);
        glBindFramebuffer(GL_FRAMEBUFFER, bloomTarget_.framebuffer);
        drawQuadOperation(operation);
        sourceYDown = false;
        glUniform1i(filterImageYDownUniform_, sourceYDown);
        if (diagnostics_) {
          ++stats_.drawCalls;
          ++stats_.filterDrawCalls;
        }

        std::uint32_t bloomSource = bloomTarget_.texture;
        const int passCount = static_cast<int>(filterParams(filter)[3]);
        for (int pass = 0; pass < passCount; ++pass) {
          const bool targetFilter = bloomSource == bloomTarget_.texture;
          glBindFramebuffer(GL_FRAMEBUFFER,
                            targetFilter ? filterTarget_.framebuffer :
                                           bloomTarget_.framebuffer);
          glBindTexture(GL_TEXTURE_2D, bloomSource);
          const std::array<float, 10> passParameters = {
            filterParams(filter)[6 + pass], filterParams(filter)[4],
            filterParams(filter)[5], 0, 0, 0, 0, 0, 0, 0};
          glUniform1fv(pixiFilterParametersUniform_, 10,
                       passParameters.data());
          glUniform1i(pixiFilterKindUniform_, 21);
          drawQuadOperation(operation);
          sourceYDown = false;
          glUniform1i(filterImageYDownUniform_, sourceYDown);
          if (diagnostics_) {
            ++stats_.drawCalls;
            ++stats_.filterDrawCalls;
          }
          bloomSource = targetFilter ? filterTarget_.texture : bloomTarget_.texture;
        }
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, bloomSource);
        glUniform1i(bloomImageUniform_, 3);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, groupTargets_[filterDepth].texture);
        sourceYDown = true;
        glUniform1i(filterImageYDownUniform_, sourceYDown);
        const std::array<float, 10> compositeParameters = {
          filterParams(filter)[0], filterParams(filter)[1],
          0, 0, 0, 0, 0, 0, 0, 0};
        glUniform1fv(pixiFilterParametersUniform_, 10,
                     compositeParameters.data());
        glUniform1i(pixiFilterKindUniform_, 22);
      } else if (filterKind(filter) == scene_packet::FilterKind::crt) {
        glUniform1f(blurUniform_, 0);
        glUniform2f(blurDirectionUniform_, 0, 0);
        glUniform1fv(pixiFilterParametersUniform_, 10, filterParams(filter).data());
        glUniform1i(pixiFilterKindUniform_, 4);
      } else if (filterKind(filter) == scene_packet::FilterKind::adjustment ||
                 filterKind(filter) == scene_packet::FilterKind::pixelate ||
                 filterKind(filter) == scene_packet::FilterKind::rgbSplit ||
                 filterKind(filter) == scene_packet::FilterKind::bulgePinch) {
        glUniform1f(blurUniform_, 0);
        glUniform2f(blurDirectionUniform_, 0, 0);
        glUniform1fv(pixiFilterParametersUniform_, 10,
                     filterParams(filter).data());
        glUniform1i(pixiFilterKindUniform_,
          5 + static_cast<int>(filterKind(filter)) -
            static_cast<int>(scene_packet::FilterKind::adjustment));
      } else if (filterKind(filter) == scene_packet::FilterKind::twist ||
                 filterKind(filter) == scene_packet::FilterKind::ascii ||
                 filterKind(filter) == scene_packet::FilterKind::dot ||
                 filterKind(filter) == scene_packet::FilterKind::emboss ||
                 filterKind(filter) == scene_packet::FilterKind::crossHatch) {
        glUniform1f(blurUniform_, 0);
        glUniform2f(blurDirectionUniform_, 0, 0);
        glUniform1fv(pixiFilterParametersUniform_, 10,
                     filterParams(filter).data());
        glUniform1i(pixiFilterKindUniform_,
          9 + static_cast<int>(filterKind(filter)) -
            static_cast<int>(scene_packet::FilterKind::twist));
      } else if (filterKind(filter) == scene_packet::FilterKind::radialBlur ||
                 filterKind(filter) == scene_packet::FilterKind::reflection ||
                 filterKind(filter) == scene_packet::FilterKind::motionBlur) {
        glUniform1f(blurUniform_, 0);
        glUniform2f(blurDirectionUniform_, 0, 0);
        glUniform1fv(pixiFilterParametersUniform_, 10,
                     filterParams(filter).data());
        glUniform1i(pixiFilterKindUniform_,
          14 + static_cast<int>(filterKind(filter)) -
            static_cast<int>(scene_packet::FilterKind::radialBlur));
      } else if (filterKind(filter) == scene_packet::FilterKind::alpha) {
        glUniform1f(blurUniform_, 0);
        glUniform2f(blurDirectionUniform_, 0, 0);
        glUniform1fv(pixiFilterParametersUniform_, 10,
                     filterParams(filter).data());
        glUniform1i(pixiFilterKindUniform_, 17);
      } else if (filterKind(filter) == scene_packet::FilterKind::oldFilm) {
        glUniform1f(blurUniform_, 0);
        glUniform2f(blurDirectionUniform_, 0, 0);
        glUniform1fv(pixiFilterParametersUniform_, 10,
                     filterParams(filter).data());
        glUniform1i(pixiFilterKindUniform_, 18);
      } else if (filterKind(filter) == scene_packet::FilterKind::glow) {
        glUniform1f(blurUniform_, 0);
        glUniform2f(blurDirectionUniform_, 0, 0);
        glUniform1fv(pixiFilterParametersUniform_, 10,
                     filterParams(filter).data());
        glUniform1i(pixiFilterKindUniform_, 19);
      } else if (filterKind(filter) == scene_packet::FilterKind::godray) {
        glUniform1f(blurUniform_, 0);
        glUniform2f(blurDirectionUniform_, 0, 0);
        glUniform1fv(pixiFilterParametersUniform_, 10,
                     filterParams(filter).data());
        glUniform1i(pixiFilterKindUniform_, 20);
      } else if (filterKind(filter) == scene_packet::FilterKind::kawaseBlur) {
        ensureTarget(filterTarget_, width_, height_);
        glUniform1f(blurUniform_, 0);
        glUniform2f(blurDirectionUniform_, 0, 0);
        glDisable(GL_BLEND);
        std::uint32_t sourceTexture = groupTargets_[filterDepth].texture;
        const int passCount = static_cast<int>(filterParams(filter)[0]);
        for (int pass = 0; pass < passCount; ++pass) {
          const bool targetFilter = sourceTexture == groupTargets_[filterDepth].texture;
          glBindFramebuffer(GL_FRAMEBUFFER,
                            targetFilter ? filterTarget_.framebuffer :
                                           groupTargets_[filterDepth].framebuffer);
          glBindTexture(GL_TEXTURE_2D, sourceTexture);
          const std::array<float, 10> passParameters = {
            filterParams(filter)[3 + pass], filterParams(filter)[1],
            filterParams(filter)[2], 0, 0, 0, 0, 0, 0, 0};
          glUniform1fv(pixiFilterParametersUniform_, 10,
                       passParameters.data());
          glUniform1i(pixiFilterKindUniform_, 21);
          drawQuadOperation(operation);
          sourceYDown = false;
          glUniform1i(filterImageYDownUniform_, sourceYDown);
          if (diagnostics_) {
            ++stats_.drawCalls;
            ++stats_.filterDrawCalls;
          }
          sourceTexture = targetFilter ? filterTarget_.texture :
                                         groupTargets_[filterDepth].texture;
        }
        compositeTexture = sourceTexture;
        glUniform1i(pixiFilterKindUniform_, 0);
      } else if (filterKind(filter) == scene_packet::FilterKind::colorMatrix) {
        glUniform1f(blurUniform_, 0);
        glUniform2f(blurDirectionUniform_, 0, 0);
        glUniform1fv(colorMatrixUniform_, 20, filterParams(filter).data());
        glUniform1f(colorMatrixAlphaUniform_, filterParams(filter)[20]);
        glUniform1i(colorMatrixEnabledUniform_, 1);
      } else if (filterKind(filter) == scene_packet::FilterKind::mzColor) {
        glUniform1f(blurUniform_, 0);
        glUniform2f(blurDirectionUniform_, 0, 0);
        glUniform1fv(pixiFilterParametersUniform_, 10,
                     filterParams(filter).data());
        glUniform1i(pixiFilterKindUniform_, 26);
      } else if (filterKind(filter) == scene_packet::FilterKind::fxaa) {
        glUniform1f(blurUniform_, 0);
        glUniform2f(blurDirectionUniform_, 0, 0);
        glUniform1i(pixiFilterKindUniform_, 25);
      } else if (filterKind(filter) == scene_packet::FilterKind::pictureBlend) {
        glUniform1f(blurUniform_, 0);
        glUniform2f(blurDirectionUniform_, 0, 0);
        glUniform1i(pixiFilterKindUniform_,
          filterParams(filter)[0] == 0 ? 23 : 24);
      }

      if (filterKind(filter) == scene_packet::FilterKind::custom) {
        if (customPlan(filter)) {
          drawCustomFilterPlan(*customPlan(filter), groupTargets_[filterDepth].framebuffer,
            filterDepth == 0 ? rootFramebuffer : groupTargets_[filterDepth - 1].framebuffer,
            filter, sourceResolution, rasterResolution, targetYDown, rasterFrame);
          glUseProgram(program_);
          projectTarget(program_);
          activeProgram = program_;
          activeBlend = customPlan(filter)->passes.empty() ? BlendMode::normal :
            customPlan(filter)->passes.back().blend;
          scissorActive = false;
          continue;
        }
        const auto& custom = filterProgram(filterProgramHandle(filter));
        std::array<int, 4> bounds{0, 0, width_, height_};
        const auto beginIndex = static_cast<std::size_t>(filterCommands[filterDepth] - frame_.commands.data());
        if (filterBounds_[beginIndex].bounded) bounds = filterBounds_[beginIndex].rect;
        bounds[0] = std::clamp(bounds[0], 0, width_);
        bounds[1] = std::clamp(bounds[1], 0, height_);
        bounds[2] = std::clamp(bounds[2], bounds[0], width_);
        bounds[3] = std::clamp(bounds[3], bounds[1], height_);
        const int frameWidth = bounds[2] - bounds[0];
        const int frameHeight = bounds[3] - bounds[1];
        const auto powerOfTwo = [](int size) {
          int result = 1;
          while (result < size) result *= 2;
          return result;
        };
        auto& input = customFilterTargets_[filterDepth];
        ensureTarget(input, powerOfTwo(frameWidth), powerOfTwo(frameHeight));
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, input.framebuffer);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, groupTargets_[filterDepth].framebuffer);
        if (frameWidth > 0 && frameHeight > 0) {
          // Filter texture coordinates start at the top of the cropped source frame.
          glBlitFramebuffer(bounds[0], bounds[1], bounds[2], bounds[3],
            0, 0, frameWidth, frameHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, filterDepth == 0 ? rootFramebuffer :
                          groupTargets_[filterDepth - 1].framebuffer);
        glBindTexture(GL_TEXTURE_2D, input.texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        textureNearestState_[input.texture] = false;
        glUseProgram(custom.program);
        glUniform1i(glGetUniformLocation(custom.program, "pmjsTargetYDown"), targetYDown);
        glUniform1i(glGetUniformLocation(custom.program, "uSampler"), 0);
        glUniform2f(glGetUniformLocation(custom.program, "pmjsScreenSize"), width_, height_);
        glUniform4f(glGetUniformLocation(custom.program, "pmjsFilterFrame"),
          bounds[0], bounds[1], frameWidth, frameHeight);
        glUniform2f(glGetUniformLocation(custom.program, "pmjsFilterTextureSize"), input.width, input.height);
        glUniform4f(glGetUniformLocation(custom.program, "filterArea"),
          input.width, input.height, bounds[0], bounds[1]);
        glUniform4f(glGetUniformLocation(custom.program, "filterClamp"), 0, 0,
          float(frameWidth - 1) / input.width, float(frameHeight - 1) / input.height);
        int offset = 1;
        for (const auto& uniform : custom.uniforms) {
          const float* data = filterParams(filter).data() + offset;
          switch (uniform.type) {
            case GL_FLOAT: glUniform1fv(uniform.location, uniform.count, data); break;
            case GL_FLOAT_VEC2: glUniform2fv(uniform.location, uniform.count, data); break;
            case GL_FLOAT_VEC3: glUniform3fv(uniform.location, uniform.count, data); break;
            case GL_FLOAT_VEC4: glUniform4fv(uniform.location, uniform.count, data); break;
            case GL_FLOAT_MAT2: glUniformMatrix2fv(uniform.location, uniform.count, GL_FALSE, data); break;
            case GL_FLOAT_MAT3: glUniformMatrix3fv(uniform.location, uniform.count, GL_FALSE, data); break;
            case GL_FLOAT_MAT4: glUniformMatrix4fv(uniform.location, uniform.count, GL_FALSE, data); break;
          }
          offset += uniform.components * uniform.count;
        }
        glEnable(GL_SCISSOR_TEST);
        rasterScissor(bounds[0], height_ - bounds[3], bounds[2] - bounds[0], bounds[3] - bounds[1]);
        activeBlend = filter.blendMode;
        glEnable(GL_BLEND);
        applyBlendMode(activeBlend);
        drawQuadOperation(operation);
        if (diagnostics_) {
          ++stats_.drawCalls;
          ++stats_.filterDrawCalls;
        }
        glUseProgram(program_);
      projectTarget(program_);
        activeProgram = program_;
        glDisable(GL_SCISSOR_TEST);
        scissorActive = false;
        continue;
      }

      glUniform1i(filterTargetYDownUniform_, targetYDown);
      glUniform1i(filterImageYDownUniform_, sourceYDown);
      glBindFramebuffer(GL_FRAMEBUFFER, filterDepth == 0 ? rootFramebuffer :
                        groupTargets_[filterDepth - 1].framebuffer);
      glBindTexture(GL_TEXTURE_2D, compositeTexture);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      textureNearestState_[compositeTexture] = false;
      if (filterKind(filter) == scene_packet::FilterKind::blur) {
        glUniform2f(blurDirectionUniform_, 0, 1);
      }
      activeBlend = filter.blendMode;
      if (pictureBlend) {
        glDisable(GL_BLEND);
      } else {
        glEnable(GL_BLEND);
        applyBlendMode(activeBlend);
      }
      if (filterRegionCounts[filterDepth] != 0) {
        glEnable(GL_SCISSOR_TEST);
        for (std::size_t regionIndex = 0;
             regionIndex < filterRegionCounts[filterDepth]; ++regionIndex) {
          const auto& region = filterRegions[filterDepth][regionIndex];
          rasterScissor(region[0], height_ - region[3],
                    region[2] - region[0], region[3] - region[1]);
          drawQuadOperation(operation);
          if (diagnostics_) {
            ++stats_.drawCalls;
            ++stats_.filterDrawCalls;
          }
        }
        scissorActive = savedScissor[filterDepth];
        activeClip = savedClip[filterDepth];
        if (scissorActive) {
          rasterScissor(activeClip[0], height_ - activeClip[3],
                    std::max(0, activeClip[2] - activeClip[0]),
                    std::max(0, activeClip[3] - activeClip[1]));
        } else {
          glDisable(GL_SCISSOR_TEST);
        }
      } else {
        scissorActive = savedScissor[filterDepth];
        activeClip = savedClip[filterDepth];
        if (scissorActive) {
          glEnable(GL_SCISSOR_TEST);
          rasterScissor(activeClip[0], height_ - activeClip[3],
                    std::max(0, activeClip[2] - activeClip[0]),
                    std::max(0, activeClip[3] - activeClip[1]));
        }
        drawQuadOperation(operation);
        if (diagnostics_) {
          ++stats_.drawCalls;
          ++stats_.filterDrawCalls;
        }
      }
      if (pictureBlend) glEnable(GL_BLEND);
      glUniform1i(displacementEnabledUniform_, 0);
      glUniform1i(noiseGlitchEnabledUniform_, 0);
      glUniform1i(pixiFilterKindUniform_, 0);
      glUniform1i(premultipliedInputUniform_, 1);
      glUniform1i(maskEnabledUniform_, 0);
      glUniform1i(colorMatrixEnabledUniform_, 0);
      glUniform1i(spriteColorEnabledUniform_, 0);
      applyBlendMode(activeBlend);
      continue;
    }
    if (operation.matrixCommandIndex != 0) {
      const auto& matrixCommand =
        frame_.commands[operation.matrixCommandIndex - 1U];
      if (matrixCommand.colorMatrixIndex == 0 ||
          matrixCommand.colorMatrixIndex > frame_.colorMatrices.size()) {
        continue;
      }
      const auto& toneMatrix =
        frame_.colorMatrices[matrixCommand.colorMatrixIndex - 1];
      if (operation.matrixCommandIndex == composedToneCommandIndex) {
        ensureTarget(toneOverlayTarget_, width_, height_);
        presentationColorMatrix_ = toneMatrix;
        presentationColorMatrixAlpha_ = matrixCommand.color[3];
        toneCompositionActive_ = true;
        glBindFramebuffer(GL_FRAMEBUFFER, toneOverlayTarget_.framebuffer);
        glViewport(0, 0, std::lround(rasterFrame[2] * rasterResolution), std::lround(rasterFrame[3] * rasterResolution));
        glDisable(GL_SCISSOR_TEST);
        scissorActive = false;
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        activeBlend = BlendMode::normal;
        glEnable(GL_BLEND);
        applyBlendMode(activeBlend);
        continue;
      }
      ensureTarget(filterTarget_, width_, height_);
      glDisable(GL_BLEND);
      glBindFramebuffer(GL_FRAMEBUFFER, filterTarget_.framebuffer);
      glViewport(0, 0, std::lround(rasterFrame[2] * rasterResolution), std::lround(rasterFrame[3] * rasterResolution));
      glUseProgram(program_);
      projectTarget(program_);
      activeProgram = program_;
      glBindVertexArray(vertexArray_);
      glActiveTexture(GL_TEXTURE0);
      glUniform1i(filterTargetYDownUniform_, targetYDown);
      glUniform1i(filterImageYDownUniform_, targetYDown);
      glBindTexture(GL_TEXTURE_2D, filterDepth == 0 ? rootTexture :
                    groupTargets_[filterDepth - 1].texture);
      glUniform2f(textureSizeUniform_, static_cast<float>(width_), static_cast<float>(height_));
      glUniform1f(blurUniform_, 0);
      glUniform2f(blurDirectionUniform_, 0, 0);
      glUniform1i(displacementEnabledUniform_, 0);
      glUniform1i(noiseGlitchEnabledUniform_, 0);
      glUniform1i(pixiFilterKindUniform_, 0);
      glUniform1i(maskEnabledUniform_, 0);
      glUniform1i(colorMatrixEnabledUniform_, 1);
      glUniform1i(spriteColorEnabledUniform_, 0);
      glUniform1i(premultipliedInputUniform_, 1);
      glUniform1fv(colorMatrixUniform_, 20, toneMatrix.data());
      glUniform1f(colorMatrixAlphaUniform_,
                  matrixCommand.color[3]);
      drawQuadOperation(operation);
      if (diagnostics_) {
        ++stats_.drawCalls;
        ++stats_.filterDrawCalls;
        ++stats_.toneAdjustDrawCalls;
      }
      if (filterDepth == 0) {
        swapTargetColors(rootTarget, filterTarget_);
        glBindFramebuffer(GL_FRAMEBUFFER, rootFramebuffer);
      } else {
        swapTargetColors(groupTargets_[filterDepth - 1], filterTarget_);
        glBindFramebuffer(GL_FRAMEBUFFER,
                          groupTargets_[filterDepth - 1].framebuffer);
      }
      glEnable(GL_BLEND);
      applyBlendMode(activeBlend);
      glUniform1i(colorMatrixEnabledUniform_, 0);
      continue;
    }
    if (operation.clipped) {
      if (!scissorActive) {
        glEnable(GL_SCISSOR_TEST);
        scissorActive = true;
      }
      const auto& operationClip = resolvedOperationClip(operation);
      rasterScissor(operationClip[0], height_ - operationClip[3],
                std::max(0, operationClip[2] - operationClip[0]),
                std::max(0, operationClip[3] - operationClip[1]));
      activeClip = operationClip;
    } else if (scissorActive) {
      glDisable(GL_SCISSOR_TEST);
      scissorActive = false;
    }
    if (operation.primitive == RenderCommand::Primitive::effect) {
      if (!operationCommand || operationCommand->effectIndex == 0 ||
          operationCommand->effectIndex > frame_.effects.size()) continue;
      auto draw = frame_.effects[operationCommand->effectIndex - 1];
      const auto* filter = filterDepth ? filterCommands[filterDepth - 1] : nullptr;
      const auto filterFrame = effectFrame(
        filter && filter->clipped ? &commandClip(*filter) : nullptr,
        width_, height_);
      if (filterDepth > 0) {
        // MZ draws directly in the filter's local GL coordinates, bypassing Pixi's
        // projection. Crop the backdrop to that frame without reflecting particles,
        // which changes edge rasterization and model culling.
        const int frameWidth = filterFrame[2] - filterFrame[0];
        const int frameHeight = filterFrame[3] - filterFrame[1];
        if (frameWidth <= 0 || frameHeight <= 0) continue;
        ensureTarget(effectTarget_, frameWidth, frameHeight);
        GLint destination = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &destination);
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, destination);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, effectTarget_.framebuffer);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glBlitFramebuffer(filterFrame[0], filterFrame[1],
                          filterFrame[2], filterFrame[3],
                          0, 0, frameWidth, frameHeight,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, effectTarget_.framebuffer);
        const auto effectDrawCalls = effects_->draw(draw);
        if (diagnostics_) stats_.drawCalls += effectDrawCalls;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, effectTarget_.framebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, destination);
        if (scissorActive) glEnable(GL_SCISSOR_TEST);
        glBlitFramebuffer(0, 0, frameWidth, frameHeight,
                          filterFrame[0], filterFrame[1],
                          filterFrame[2], filterFrame[3],
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, destination);
      } else {
        const auto effectDrawCalls = effects_->draw(draw);
        if (diagnostics_) stats_.drawCalls += effectDrawCalls;
      }
      // Subsequent geometry already includes MZ's reset viewport mapping.
      glViewport(0, 0, std::lround(rasterFrame[2] * rasterResolution), std::lround(rasterFrame[3] * rasterResolution));
      continue;
    }
    if (operation.blendMode != activeBlend) {
      activeBlend = operation.blendMode;
      applyBlendMode(activeBlend);
    }
    if (operation.tileLayer != 0) {
      const auto layer = tileLayers_.find(operation.tileLayer);
      if (layer == tileLayers_.end() || !operationCommand) continue;
      const auto& command = *operationCommand;
      const auto& transform = command.transform;
      const auto& mapping = resolvedViewportMapping(operation);
      const std::array<float, 9> world = {
        transform[0] * mapping[0], transform[1] * mapping[1], 0.0F,
        transform[2] * mapping[0], transform[3] * mapping[1], 0.0F,
        transform[4] * mapping[0] + mapping[2], transform[5] * mapping[1] + mapping[3], 1.0F,
      };
      const auto& material = layer->second.material;
      const auto* triangleMaterial = std::get_if<TriangleBitmapMaterial>(&material);
      const auto* bitmapMaterial = std::get_if<MvBitmapMaterial>(&material);
      const bool usesOverlay = command.appliesMeshPostTintOverlay || triangleMaterial || bitmapMaterial;
      const bool canvasTriangleBitmap = triangleMaterial &&
        triangleMaterial->rasterRule == TriangleBitmapMaterial::RasterRule::canvasFourSample;
      const auto program = canvasTriangleBitmap ? canvasTriangleBitmapProgram_ :
        usesOverlay ? meshPostTintOverlayProgram_ : tileProgram_;
      const auto& uniforms = canvasTriangleBitmap ? canvasTriangleBitmapUniforms_ :
        usesOverlay ? meshPostTintOverlayUniforms_ : tileUniforms_;
      glUseProgram(program);
      projectTarget(program);
      activeProgram = program;
      glBindVertexArray(layer->second.vertexArray);
      glUniform1i(uniforms.targetYDown, targetYDown);
      glUniformMatrix3fv(uniforms.world, 1, GL_FALSE, world.data());
      glUniform2f(uniforms.screen, static_cast<float>(width_), static_cast<float>(height_));
      glUniform2f(uniforms.animation, command.tileAnimation[0],
                  command.tileAnimation[1]);
      glUniform4fv(uniforms.color, 1, command.color.data());
      if (usesOverlay) {
        const auto* commandColorEffect = colorEffect(command);
        const auto& overlayColor =
          commandColorEffect ? commandColorEffect->blendColor : zeroColor;
        glUniform4fv(uniforms.overlayColor, 1, overlayColor.data());
        glUniform1i(uniforms.trianglePaintEnabled, triangleMaterial != nullptr);
        glUniform1i(uniforms.mvBlendEnabled, bitmapMaterial != nullptr);
        glUniform1i(uniforms.nearestSampling, operation.nearest);
        if (triangleMaterial) glUniform1fv(uniforms.trianglePaint, 30, triangleMaterial->coefficients.data());
        if (bitmapMaterial != nullptr) {
          glUniform4fv(uniforms.mvBounds, 1, bitmapMaterial->texelBounds.data());
          glUniform1i(uniforms.mvPremultipliedInput, bitmapMaterial->alphaMode == AlphaMode::premultiplied);
        }
      }
      if (command.maskImage) {
        const auto* commandMaskTransform = maskTransform(command);
        const auto mask = images_.lookup(command.maskImage);
        if (!mask || commandMaskTransform == nullptr) continue;
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, mask->texture);
        glUniform1i(uniforms.maskImage, 1);
        glUniform1i(uniforms.maskEnabled, 1);
        glUniform1fv(uniforms.maskTransform, 6,
                     maskMatrix(commandMaskTransform->data()).data());
        glUniform4f(uniforms.maskFrame, 0, 0,
                    static_cast<float>(mask->width),
                    static_cast<float>(mask->height));
        glUniform2f(uniforms.maskTextureSize, static_cast<float>(mask->width),
                    static_cast<float>(mask->height));
        glUniform1f(uniforms.maskScreenHeight, rasterFrame[3] * rasterResolution);
        glActiveTexture(GL_TEXTURE0);
      } else {
        glUniform1i(uniforms.maskEnabled, 0);
      }
      for (const auto& batch : layer->second.batches) {
        glUniform1i(uniforms.texturePremultiplied, batch.premultiplied);
        if (bitmapMaterial) glUniform1i(uniforms.mvPremultipliedInput,
          batch.premultiplied || bitmapMaterial->alphaMode == AlphaMode::premultiplied);
        glUniform2f(uniforms.textureSize,
                    static_cast<float>(batch.textureWidth),
                    static_cast<float>(batch.textureHeight));
        glBindTexture(GL_TEXTURE_2D, batch.texture);
        const auto nearest = textureNearestState_.find(batch.texture);
        if (nearest == textureNearestState_.end() ||
            nearest->second != operation.nearest) {
          textureNearestState_[batch.texture] = operation.nearest;
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                          operation.nearest ? GL_NEAREST : GL_LINEAR);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                          operation.nearest ? GL_NEAREST : GL_LINEAR);
        }
        glDrawArrays(GL_TRIANGLES, batch.first, batch.count);
        if (diagnostics_) {
          ++stats_.drawCalls;
          if (operation.primitive == RenderCommand::Primitive::mesh) ++stats_.meshDrawCalls;
          else ++stats_.tileDrawCalls;
        }
      }
      continue;
    }

    const bool simpleSprite = operation.blur <= 0 && operation.maskImage == 0 &&
                              !operation.appliesSpriteColor &&
                              operation.inlineMatrixIndex == 0;
    const std::uint32_t spriteProgram = simpleSprite ? simpleProgram_ :
                                                     spriteEffectProgram_;
    if (activeProgram != spriteProgram) {
      glUseProgram(spriteProgram);
      projectTarget(spriteProgram);
      glBindVertexArray(vertexArray_);
      activeProgram = spriteProgram;
    }
    glBindTexture(GL_TEXTURE_2D, operation.texture);
    const auto nearest = textureNearestState_.find(operation.texture);
    if (nearest == textureNearestState_.end() ||
        nearest->second != operation.nearest) {
      textureNearestState_[operation.texture] = operation.nearest;
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                      operation.nearest ? GL_NEAREST : GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                      operation.nearest ? GL_NEAREST : GL_LINEAR);
    }
    // Pixi render targets project downward in GL; readback reflection cannot
    // reproduce sampling and edge coverage at exact pixel-center boundaries.
    const float ySign = targetYDown ? 1.0F : -1.0F;
    const std::array<float, 9> projection = {2.0F / width_, 0, 0, 0, ySign * 2.0F / height_, 0, -1, -ySign, 1};
    glUniform1i(simpleSprite ? simpleTargetYDownUniform_ : spriteEffectTargetYDownUniform_, targetYDown);
    glUniformMatrix3fv(simpleSprite ? simpleSpriteProjectionUniform_ : spriteEffectProjectionUniform_, 1, GL_FALSE, projection.data());
    glUniform1i(simpleSprite ? simpleTilingClampUniform_ : spriteEffectTilingClampUniform_,
                operation.clampedTilingSampling);
    if (simpleSprite) glUniform2f(simpleTextureSizeUniform_, operation.textureWidth, operation.textureHeight);
    glUniform1i(simpleSprite ? simpleSpriteVerticesUniform_ : spriteEffectVerticesUniform_, operation.spriteWorldVertices);
    glUniform1i(simpleSprite ? simpleSpritePackingUniform_ : spriteEffectPackingUniform_,
                operation.pixiSpritePacking ? 1 : 0);
    glUniform1i(simpleSprite ? simpleSpritePremultipliedUniform_ : spriteEffectPremultipliedUniform_,
                operation.premultipliedSpriteTexture ? 1 : 0);
    if (!simpleSprite) {
      if (operationCommand == nullptr) continue;
      const auto& frame = operationCommand->source;
      glUniform4f(spriteEffectFrameUniform_, frame[0], frame[1],
        frame[0] + frame[2] - 1, frame[1] + frame[3] - 1);
      glUniform1i(spriteEffectNearestUniform_, operation.nearest);
      glUniform1i(spriteEffectColorEnabledUniform_,
                  operation.appliesSpriteColor ? 1 : 0);
      if (operation.appliesSpriteColor) {
        const auto* operationColorEffect = colorEffect(*operationCommand);
        if (operationColorEffect == nullptr) continue;
        glUniform4fv(spriteEffectColorToneUniform_, 1,
                     operationColorEffect->colorTone.data());
        glUniform4fv(spriteEffectBlendColorUniform_, 1,
                     operationColorEffect->blendColor.data());
      }
      glUniform2f(spriteEffectTextureSizeUniform_, operation.textureWidth,
                  operation.textureHeight);
      glUniform1f(spriteEffectBlurUniform_, operation.blur);
      glUniform1i(spriteEffectMatrixEnabledUniform_,
                  operation.inlineMatrixIndex != 0 ? 1 : 0);
      if (operation.inlineMatrixIndex != 0) {
        const auto& inlineMatrix =
          frame_.commands[operation.inlineMatrixIndex - 1U];
        glUniform1fv(spriteEffectMatrixUniform_, 20,
                     filterParams(inlineMatrix).data());
        glUniform1f(spriteEffectMatrixAlphaUniform_,
                    filterParams(inlineMatrix)[20]);
      }
    }
    if (!simpleSprite && operation.maskImage) {
      const auto mask = images_.lookup(operation.maskImage);
      const auto* operationMaskTransform =
        operationCommand ? maskTransform(*operationCommand) : nullptr;
      if (!mask || operationMaskTransform == nullptr) continue;
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, mask->texture);
      glUniform1i(spriteEffectMaskImageUniform_, 1);
      glUniform1i(spriteEffectMaskEnabledUniform_, 1);
      glUniform1fv(spriteEffectMaskTransformUniform_, 6,
                   maskMatrix(operationMaskTransform->data()).data());
      glUniform2f(spriteEffectMaskTextureSizeUniform_,
                  static_cast<float>(mask->width),
                  static_cast<float>(mask->height));
      glUniform1f(spriteEffectScreenHeightUniform_,
                  rasterFrame[3] * rasterResolution);
      glActiveTexture(GL_TEXTURE0);
    } else if (!simpleSprite) {
      glUniform1i(spriteEffectMaskEnabledUniform_, 0);
    }
    const bool gpuRepeat = operation.repeat && !operation.clampedTilingSampling;
    const auto repeat = textureRepeatState_.find(operation.texture);
    if (repeat == textureRepeatState_.end() || repeat->second != gpuRepeat) {
      textureRepeatState_[operation.texture] = gpuRepeat;
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                      gpuRepeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                      gpuRepeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    }
    drawQuadOperation(operation);
    if (diagnostics_) {
      ++stats_.drawCalls;
      if (operation.primitive == RenderCommand::Primitive::tilingSprite) ++stats_.tilingSpriteDrawCalls;
      else if (operation.primitive == RenderCommand::Primitive::screenFill) ++stats_.screenFillDrawCalls;
      else ++stats_.spriteDrawCalls;
      if (simpleSprite) ++stats_.baseSpriteDrawCalls;
      else ++stats_.effectSpriteDrawCalls;
    }
  }
  if (scissorActive) glDisable(GL_SCISSOR_TEST);
  applyBlendMode(BlendMode::normal);
  if (diagnostics_) stats_.commands += frame_.commands.size();
  discardCommandsFrom(0);
  // RenderCommand used to own CustomFilterPlan directly, so discarding commands
  // released sampler-image leases immediately after scene execution. Preserve
  // that ownership boundary now that plans live in the frame side table.
  frame_.customFilterPlans.clear();
  if (!offscreenRender_ && sceneSubmittedThisFrame_) {
    hasValidSceneFrame_ = true;
  }
} else {
  discardCommandsFrom(0);
  frame_.customFilterPlans.clear();
  if (diagnostics_) ++stats_.retainedFrames;
}
if (diagnostics_) ++stats_.frames;
}

void Renderer::presentToDrawable() {
  if (offscreenRender_) return;
  const bool hasPresentationLayers = presentationCanvasOpacity_ < 1.0F ||
      (presentationVideo_ && presentationVideoOpacity_ > 0.0F) ||
      (presentationUpperCanvas_ && presentationUpperCanvasOpacity_ > 0.0F);
  const bool composePresentation = toneCompositionActive_ ||
      hasPresentationLayers;
  if (composePresentation) {
    ensureTarget(filterTarget_, width_, height_);
    drawToneComposition(filterTarget_.framebuffer, 0, 0, width_, height_, true);
    if (diagnostics_ && toneCompositionActive_) ++stats_.toneComposedPresentationFrames;
  }
  const bool identity = presentation_.viewportX == 0 &&
      presentation_.viewportY == 0 &&
      presentation_.viewportWidth == presentation_.drawableWidth &&
      presentation_.viewportHeight == presentation_.drawableHeight &&
      presentation_.drawableWidth == width_ &&
      presentation_.drawableHeight == height_;
  if (identity) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER,
                      composePresentation ? filterTarget_.framebuffer : sceneTarget_.framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, width_, height_, 0, 0,
                      presentationWidth_, presentationHeight_,
                      GL_COLOR_BUFFER_BIT,
                      presentation_.filter == PresentFilter::linear ?
                        GL_LINEAR : GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return;
  }
  if (diagnostics_) ++stats_.scaledPresentationFrames;
  if (presentation_.viewportWidth < presentation_.drawableWidth ||
      presentation_.viewportHeight < presentation_.drawableHeight) {
    if (diagnostics_) ++stats_.presentationLetterboxedFrames;
  }
  const int drawableWidth = presentation_.drawableWidth;
  const int drawableHeight = presentation_.drawableHeight;
  const int destX = presentation_.viewportX;
  const int destY = drawableHeight - presentation_.viewportY -
      presentation_.viewportHeight;
  const int destWidth = presentation_.viewportWidth;
  const int destHeight = presentation_.viewportHeight;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, drawableWidth, drawableHeight);
  glDisable(GL_SCISSOR_TEST);
  // Bars use a blit, not glClear: an unswapped window clear correlates
  // with stale FBO readback under llvmpipe. Revisit with Mali evidence.
  const bool fullWindow = destX == 0 && destY == 0 &&
      destWidth == drawableWidth && destHeight == drawableHeight;
  if (!fullWindow) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, blackFramebuffer_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, 1, 1, 0, 0, drawableWidth, drawableHeight,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
  }
  glBindFramebuffer(GL_READ_FRAMEBUFFER,
                    composePresentation ? filterTarget_.framebuffer : sceneTarget_.framebuffer);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glBlitFramebuffer(0, 0, width_, height_, destX, destY,
                    destX + destWidth, destY + destHeight,
                    GL_COLOR_BUFFER_BIT,
                    presentation_.filter == PresentFilter::linear ?
                      GL_LINEAR : GL_NEAREST);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::drawToneComposition(std::uint32_t framebuffer,
                                   int viewportX, int viewportY,
                                   int viewportWidth, int viewportHeight,
                                   bool screenPresentation) {
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  glViewport(viewportX, viewportY, viewportWidth, viewportHeight);
  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);
  glUseProgram(presentationProgram_);
  glBindVertexArray(vertexArray_);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, sceneTarget_.texture);
  glUniform1i(presentationSceneUniform_, 0);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, toneOverlayTarget_.texture);
  glUniform1i(presentationOverlayUniform_, 1);
  const auto video = images_.lookup(presentationVideo_);
  const auto upperCanvas = images_.lookup(presentationUpperCanvas_);
  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D, video ? video->texture : whiteTexture_);
  glUniform1i(presentationVideoUniform_, 2);
  glActiveTexture(GL_TEXTURE3);
  glBindTexture(GL_TEXTURE_2D,
                upperCanvas ? upperCanvas->texture : whiteTexture_);
  glUniform1i(presentationUpperCanvasUniform_, 3);
  glUniform1fv(presentationColorMatrixUniform_, 20,
               presentationColorMatrix_.data());
  glUniform1f(presentationColorMatrixAlphaUniform_,
              presentationColorMatrixAlpha_);
  const bool separateScreenPresentation = screenPresentation &&
      !offscreenRender_;
  glUniform1i(presentationToneEnabledUniform_, toneCompositionActive_);
  glUniform1i(presentationOpaqueBackgroundUniform_,
              separateScreenPresentation);
  glUniform1f(presentationCanvasOpacityUniform_, separateScreenPresentation
      ? presentationCanvasOpacity_ : 1.0F);
  glUniform1f(presentationVideoOpacityUniform_,
      separateScreenPresentation && video ? presentationVideoOpacity_ : 0.0F);
  glUniform1f(presentationUpperCanvasOpacityUniform_,
      separateScreenPresentation && upperCanvas
        ? presentationUpperCanvasOpacity_ : 0.0F);
  glUniform1i(presentationVideoPremultipliedUniform_, video && video->premultiplied);
  glUniform1i(presentationUpperCanvasPremultipliedUniform_, upperCanvas && upperCanvas->premultiplied);
  glDrawArrays(GL_TRIANGLES, 0, 6);
  if (diagnostics_) ++stats_.drawCalls;
  glActiveTexture(GL_TEXTURE0);
  glEnable(GL_BLEND);
  applyBlendMode(BlendMode::normal);
}

void Renderer::materializeToneComposition() {
  if (!toneCompositionActive_) return;
  ensureTarget(filterTarget_, width_, height_);
  drawToneComposition(filterTarget_.framebuffer, 0, 0, width_, height_);
  swapTargetColors(sceneTarget_, filterTarget_);
  toneCompositionActive_ = false;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}


}  // namespace pmjs
