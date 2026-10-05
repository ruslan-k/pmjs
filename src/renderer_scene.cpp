#include "renderer.hpp"
#include "scene_packet.hpp"

#include <GLES3/gl3.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace pmjs {

bool Renderer::queueScene(std::uint32_t version, const std::uint32_t* metadata,
                          std::size_t metadataCount, const float* values,
                          std::size_t valueCount, std::size_t nodeCount) {
  using namespace scene_packet;
  if (version != scene_packet::version ||
      (nodeCount > 0 && (!metadata || !values)) ||
      nodeCount > maxNodes ||
      metadataCount < nodeCount * metadataStride ||
      valueCount < nodeCount * valueStride ||
      nodeCount * (metadataStride * sizeof(std::uint32_t) +
                   valueStride * sizeof(float)) > maxPacketBytes) return false;

  if (nodeCount > 0) {
    for (std::size_t index = 0; index < nodeCount * valueStride; ++index) {
      if (!std::isfinite(values[index])) return false;
    }
  }

  const std::size_t originalCommandCount = frame_.commands.size();
  const std::size_t originalEffectCount = frame_.effects.size();
  const std::size_t originalColorMatrixCount = frame_.colorMatrices.size();
  const std::size_t originalFilterPayloadCount = frame_.filterPayloads.size();
  const std::size_t originalFilterParameterCount =
    frame_.filterParameters.size();
  const std::size_t originalCustomFilterPlanCount =
    frame_.customFilterPlans.size();
  const std::size_t originalColorEffectCount = frame_.colorEffects.size();
  const std::size_t originalClipCount = frame_.clips.size();
  const std::size_t originalMaskTransformCount = frame_.maskTransforms.size();
  const std::size_t originalSpriteVerticesCount = frame_.spriteVertices.size();
  const bool originalSceneSubmitted = sceneSubmittedThisFrame_;
  const bool originalSceneHasEffect = sceneHasEffect_;
  const bool originalSceneHasCustomFilter = sceneHasCustomFilter_;
  // A scene packet can emit at most one command per node. Reserve once at the
  // packet boundary instead of repeatedly growing the retained command vector.
  if (frame_.commands.capacity() < originalCommandCount + nodeCount) {
    frame_.commands.reserve(originalCommandCount + nodeCount);
  }
  const auto build = [&]() -> bool {

  struct SceneState {
    std::array<float, 6> world;
    float alpha;
    std::array<int, 4> clip;
    bool clipped;
    ImageHandle maskImage;
    std::array<float, 6> maskTransform;

    SceneState(const std::array<float, 6>& worldValue, float alphaValue,
               const std::array<int, 4>& clipValue, bool clippedValue,
               ImageHandle maskImageValue,
               const std::array<float, 6>& maskTransformValue)
        : world(worldValue), alpha(alphaValue), clip(clipValue),
          clipped(clippedValue), maskImage(maskImageValue),
          maskTransform(maskTransformValue) {}
  };
  // Scene submission is a per-frame hot path. Reuse its state buffer so a
  // large map does not malloc/free the same block every frame. Drop an
  // excessively oversized buffer after a scene-size collapse to keep the
  // low-memory target bounded.
  static thread_local std::vector<SceneState> states;
  if (states.capacity() > 1024 && nodeCount * 4 < states.capacity()) {
    std::vector<SceneState>().swap(states);
  }
  states.clear();
  if (states.capacity() < nodeCount) states.reserve(nodeCount);
  std::size_t filterDepth = 0;
  for (std::size_t index = 0; index < nodeCount; ++index) {
    const std::size_t metadataOffset = index * metadataStride;
    const std::size_t valueOffset = index * valueStride;
    const std::uint32_t kind = metadata[metadataOffset];
    const std::uint32_t parentIndex = metadata[metadataOffset + 1];
    const std::uint32_t resource = metadata[metadataOffset + 2];
    const std::uint32_t tint = metadata[metadataOffset + 3];
    const auto blendValue = metadata[metadataOffset + 4];
    const auto flags = metadata[metadataOffset + 5];
    const auto maskImage = metadata[metadataOffset + 6];
    const bool filterMarker =
      kind == static_cast<std::uint32_t>(NodeKind::filterBegin) ||
      kind == static_cast<std::uint32_t>(NodeKind::filterEnd);
    if (kind != static_cast<std::uint32_t>(NodeKind::toneAdjust) &&
        !filterMarker &&
        blendValue > static_cast<std::uint32_t>(BlendMode::screen)) return false;
    const auto blendMode = static_cast<BlendMode>(blendValue);
    if (parentIndex != noParent && parentIndex >= index) return false;
    if ((flags & NodeFlags::clampedTilingSampling) &&
        kind != static_cast<std::uint32_t>(NodeKind::tilingSprite)) return false;
    if (flags & ~scene_packet::kAllowedNodeFlags) {
      return false;
    }
    const auto textureRotation = static_cast<std::uint8_t>(
      (flags & NodeFlags::textureRotationMask) >> NodeFlags::textureRotationShift);
    if (textureRotation != 0 &&
        kind != static_cast<std::uint32_t>(NodeKind::sprite)) return false;
    if ((flags & (NodeFlags::premultipliedSpriteTexture | NodeFlags::packedSpriteColor | NodeFlags::spriteWorldVertices | NodeFlags::standaloneBitmapRegion)) &&
        kind != static_cast<std::uint32_t>(NodeKind::sprite)) return false;
    if ((flags & NodeFlags::roundPixels) &&
        kind != static_cast<std::uint32_t>(NodeKind::sprite)) return false;
    if ((flags & (NodeFlags::hasMeshPostTintOverlay | NodeFlags::hasMvBitmapBlend)) &&
        kind != static_cast<std::uint32_t>(NodeKind::mesh)) return false;

    if ((flags & NodeFlags::standaloneBitmapRegion) && !(flags & NodeFlags::premultipliedSpriteTexture)) return false;
    if (parentIndex != noParent && (metadata[parentIndex * metadataStride + 5] & NodeFlags::spriteWorldVertices)) return false;
    const bool spriteVertices = flags & NodeFlags::spriteWorldVertices;
    const bool effectNode = kind == static_cast<std::uint32_t>(NodeKind::effect);
    const std::array<float, 6> local = effectNode ?
      std::array<float, 6>{1, 0, 0, 1, 0, 0} : std::array<float, 6>{
      values[valueOffset], values[valueOffset + 1],
      values[valueOffset + 2], values[valueOffset + 3],
      values[valueOffset + 4], values[valueOffset + 5]
    };
    static const SceneState rootState{
      {1, 0, 0, 1, 0, 0}, 1.0F, {}, false, 0, {}
    };
    const SceneState& parent = parentIndex == noParent
      ? rootState : states[parentIndex];
    std::array<float, 6> world = {
      parent.world[0] * local[0] + parent.world[2] * local[1],
      parent.world[1] * local[0] + parent.world[3] * local[1],
      parent.world[0] * local[2] + parent.world[2] * local[3],
      parent.world[1] * local[2] + parent.world[3] * local[3],
      parent.world[0] * local[4] + parent.world[2] * local[5] + parent.world[4],
      parent.world[1] * local[4] + parent.world[3] * local[5] + parent.world[5],
    };
    if (spriteVertices) world = parent.world;
    states.emplace_back(world, parent.alpha * values[valueOffset + 6],
      parent.clip, parent.clipped, parent.maskImage, parent.maskTransform);
    SceneState& state = states.back();
    if (flags & NodeFlags::hasAlphaMask) {
      if (state.maskImage || !images_.lookup(maskImage)) return false;
      state.maskImage = maskImage;
      std::copy_n(values + valueOffset + 22, 6, state.maskTransform.begin());
    }
    if (flags & NodeFlags::hasClipRectangle) {
      const float left = values[valueOffset + 17];
      const float top = values[valueOffset + 18];
      const float right = values[valueOffset + 19];
      const float bottom = values[valueOffset + 20];
      if (!std::isfinite(left) || !std::isfinite(top) ||
          !std::isfinite(right) || !std::isfinite(bottom) ||
          right < left || bottom < top) return false;
      std::array<int, 4> own = {
        static_cast<int>(std::floor(std::max(left, 0.0F))),
        static_cast<int>(std::floor(std::max(top, 0.0F))),
        static_cast<int>(std::ceil(std::max(right, 0.0F))),
        static_cast<int>(std::ceil(std::max(bottom, 0.0F))),
      };
      if (state.clipped) {
        own = {std::max(own[0], state.clip[0]),
               std::max(own[1], state.clip[1]),
               std::min(own[2], state.clip[2]),
               std::min(own[3], state.clip[3])};
      }
      state.clip = own;
      state.clipped = true;
    }
    if (filterMarker) {
      if (maskImage != 0 || flags & ~NodeFlags::hasClipRectangle) return false;
      RenderCommand command;
      FilterCommandPayload commandFilterPayload{};
      setCommandClip(command, state.clip, state.clipped);
      if (kind == static_cast<std::uint32_t>(NodeKind::filterBegin)) {
        if (filterDepth >= maxFilterDepth ||
            blendValue > static_cast<std::uint32_t>(FilterKind::custom)) {
          return false;
        }
        command.action = RenderCommand::Action::filterBegin;
        commandFilterPayload.kind = static_cast<FilterKind>(blendValue);
        std::array<float, 21> filterParameters{};
        std::shared_ptr<const CustomFilterPlan> customFilterPlan;
        std::copy_n(values + valueOffset + 7, 10,
                    filterParameters.begin());
        std::copy_n(values + valueOffset + 22, 11,
                    filterParameters.begin() + 10);
        commandFilterPayload.resolution = values[valueOffset + 33];
        const float compositeBlend = values[valueOffset + filterCompositeBlendOffset];
        if (compositeBlend < 0 || compositeBlend > 3 ||
            std::floor(compositeBlend) != compositeBlend) return false;
        command.blendMode = static_cast<BlendMode>(compositeBlend);
        if (commandFilterPayload.resolution == 0.0F) commandFilterPayload.resolution = 1.0F;
        if (!std::isfinite(commandFilterPayload.resolution) ||
            commandFilterPayload.resolution <= 0.0F ||
            commandFilterPayload.resolution > 16.0F) return false;
        if (commandFilterPayload.kind == FilterKind::custom) {
          sceneHasCustomFilter_ = true;
          const auto plan = filterPlans_.find(resource);
          if (plan != filterPlans_.end()) {
            customFilterPlan = plan->second.lock();
            if (!customFilterPlan) return false;
          } else if (resource == 0 || resource > filterPrograms_.size() ||
              filterParameters[0] < 0 ||
              filterParameters[0] > 65536 ||
              commandFilterPayload.resolution != 1) return false;
          commandFilterPayload.program = customFilterPlan ? 0 : resource;
          if (!customFilterPlan) {
            const auto& program = filterProgram(resource);
            std::size_t components = 0;
            if (program.pixiVertex) return false;
            for (const auto& uniform : program.uniforms) {
              if (uniform.type != GL_FLOAT && uniform.type != GL_FLOAT_VEC2 &&
                  uniform.type != GL_FLOAT_VEC3 && uniform.type != GL_FLOAT_VEC4 &&
                  uniform.type != GL_FLOAT_MAT2 && uniform.type != GL_FLOAT_MAT3 &&
                  uniform.type != GL_FLOAT_MAT4) return false;
              components += uniform.components * uniform.count;
            }
            if (components > 20) return false;
          }
        } else if (commandFilterPayload.kind == FilterKind::blur) {
          if (resource != 0 || filterParameters[0] < 0 ||
              filterParameters[1] < 1 ||
              filterParameters[1] > 15 ||
              (filterParameters[2] != 0 &&
               filterParameters[2] != 5)) return false;
        } else if (commandFilterPayload.kind == FilterKind::blurX ||
                   commandFilterPayload.kind == FilterKind::blurY) {
          if (resource != 0 || filterParameters[0] < 0 ||
              filterParameters[1] < 1 ||
              filterParameters[1] > 15 ||
              (filterParameters[2] != 0 &&
               filterParameters[2] != 5)) return false;
        } else if (commandFilterPayload.kind == FilterKind::mzColor) {
          if (resource != 0) return false;
        } else if (commandFilterPayload.kind == FilterKind::fxaa) {
          if (resource != 0) return false;
        } else if (commandFilterPayload.kind == FilterKind::displacement) {
          if (!images_.lookup(resource) || filterParameters[2] <= 0 ||
              filterParameters[3] <= 0) return false;
          if (!images_.beginUse(resource)) return false;
          command.image = resource;
        } else if (commandFilterPayload.kind == FilterKind::alphaMask) {
          if (!images_.lookup(resource) || filterParameters[8] <= 0 ||
              filterParameters[9] <= 0 ||
              filterParameters[10] < 0 ||
              filterParameters[10] > 1 ||
              (filterParameters[11] != 0 &&
               filterParameters[11] != 1) ||
              filterParameters[12] < 0 ||
              filterParameters[12] > 14 ||
              std::fmod(filterParameters[12], 2.0F) != 0 ||
              filterParameters[13] <= 0 ||
              filterParameters[14] <= 0) return false;
          if (!images_.beginUse(resource)) return false;
          command.image = resource;
        } else if (commandFilterPayload.kind == FilterKind::noiseGlitch) {
          if (resource != 0 || filterParameters[0] < 0 ||
              filterParameters[2] < 0) return false;
        } else if (commandFilterPayload.kind == FilterKind::zoomBlur) {
          if (resource != 0 || filterParameters[3] < 0) return false;
        } else if (commandFilterPayload.kind == FilterKind::shockwave) {
          if (resource != 0 || filterParameters[3] < 0) return false;
        } else if (commandFilterPayload.kind == FilterKind::advancedBloom) {
          if (resource != 0 || filterParameters[3] < 1 ||
              filterParameters[3] > 12 ||
              filterParameters[4] <= 0 ||
              filterParameters[5] <= 0) return false;
        } else if (commandFilterPayload.kind == FilterKind::crt) {
          if (resource != 0 || filterParameters[1] < 0 ||
              filterParameters[4] < 0 ||
              filterParameters[5] < 0 ||
              filterParameters[6] < 0 ||
              filterParameters[8] < 0) return false;
        } else if (commandFilterPayload.kind == FilterKind::adjustment) {
          if (resource != 0 || filterParameters[0] <= 0) return false;
        } else if (commandFilterPayload.kind == FilterKind::pixelate) {
          if (resource != 0 || filterParameters[0] < 1 ||
              filterParameters[1] < 1) return false;
        } else if (commandFilterPayload.kind == FilterKind::rgbSplit) {
          if (resource != 0) return false;
        } else if (commandFilterPayload.kind == FilterKind::bulgePinch) {
          if (resource != 0 || filterParameters[2] < 0) return false;
        } else if (commandFilterPayload.kind == FilterKind::twist) {
          if (resource != 0 || filterParameters[2] < 0) return false;
        } else if (commandFilterPayload.kind == FilterKind::ascii) {
          if (resource != 0 || filterParameters[0] < 1) return false;
        } else if (commandFilterPayload.kind == FilterKind::dot ||
                   commandFilterPayload.kind == FilterKind::emboss ||
                   commandFilterPayload.kind == FilterKind::crossHatch) {
          if (resource != 0) return false;
        } else if (commandFilterPayload.kind == FilterKind::radialBlur) {
          if (resource != 0 || filterParameters[3] < 1 ||
              filterParameters[3] > 64) return false;
        } else if (commandFilterPayload.kind == FilterKind::reflection) {
          if (resource != 0) return false;
        } else if (commandFilterPayload.kind == FilterKind::motionBlur) {
          if (resource != 0 || filterParameters[2] < 1 ||
              filterParameters[2] > 64) return false;
        } else if (commandFilterPayload.kind == FilterKind::alpha) {
          if (resource != 0 || filterParameters[0] < 0) return false;
        } else if (commandFilterPayload.kind == FilterKind::oldFilm) {
          if (resource != 0 || filterParameters[0] < 0 ||
              filterParameters[1] < 0 ||
              filterParameters[2] < 0 ||
              filterParameters[4] < 0 ||
              filterParameters[5] < 0 ||
              filterParameters[6] < 0 ||
              filterParameters[8] < 0) return false;
        } else if (commandFilterPayload.kind == FilterKind::glow) {
          if (resource != 0 || filterParameters[0] < 0 ||
              filterParameters[0] > 32 ||
              filterParameters[6] <= 0) return false;
        } else if (commandFilterPayload.kind == FilterKind::godray) {
          if (resource != 0) return false;
        } else if (commandFilterPayload.kind == FilterKind::kawaseBlur) {
          if (resource != 0 || filterParameters[0] < 1 ||
              filterParameters[0] > 15 ||
              filterParameters[1] <= 0 ||
              filterParameters[2] <= 0) return false;
        } else if (commandFilterPayload.kind == FilterKind::colorMatrix) {
          if (resource != 0 || filterParameters[20] < 0 ||
              filterParameters[20] > 1) return false;
        } else if (commandFilterPayload.kind == FilterKind::pictureBlend) {
          if (resource != 0 || (filterParameters[0] != 0 &&
              filterParameters[0] != 1)) return false;
        }
        frame_.filterParameters.push_back(filterParameters);
        commandFilterPayload.parametersIndex =
          static_cast<std::uint32_t>(frame_.filterParameters.size());
        if (customFilterPlan) {
          frame_.customFilterPlans.push_back(customFilterPlan);
          commandFilterPayload.customPlanIndex =
            static_cast<std::uint32_t>(frame_.customFilterPlans.size());
        }
        frame_.filterPayloads.push_back(commandFilterPayload);
        command.filterPayloadIndex =
          static_cast<std::uint32_t>(frame_.filterPayloads.size());
        ++filterDepth;
      } else {
        if (filterDepth == 0 || blendValue != 0 || resource != 0) return false;
        command.action = RenderCommand::Action::filterEnd;
        --filterDepth;
      }
      try {
        frame_.commands.push_back(command);
      } catch (...) {
        if (command.filterPayloadIndex != 0) {
          frame_.filterPayloads.pop_back();
          if (commandFilterPayload.customPlanIndex != 0) {
            frame_.customFilterPlans.pop_back();
          }
          if (commandFilterPayload.parametersIndex != 0) {
            frame_.filterParameters.pop_back();
          }
        }
        if (command.image) images_.endUse(command.image);
        throw;
      }
      continue;
    }
    if (kind == static_cast<std::uint32_t>(NodeKind::container) ||
        state.alpha <= 0) continue;
    if (effectNode) {
      if (flags != 0 || state.maskImage || !effects_ || !effects_->validHandle(resource)) return false;
      sceneHasEffect_ = true;
      EffectDraw effect{};
      effect.handle = resource;
      std::copy_n(values + valueOffset, 4, effect.viewport.begin());
      std::copy_n(values + valueOffset + 7, 16, effect.projection.begin());
      std::copy_n(values + valueOffset + 23, 16, effect.camera.begin());
      std::copy_n(values + valueOffset + 39, 2, effect.resetViewport.begin());
      for (const float value : effect.viewport) {
        if (std::abs(value) > 65536) return false;
      }
      if (effect.viewport[2] <= 0 || effect.viewport[3] <= 0) return false;
      for (const float value : effect.resetViewport) {
        if (value <= 0 || value > 65536) return false;
      }
      RenderCommand command{};
      command.primitive = RenderCommand::Primitive::effect;
      setCommandClip(command, state.clip, state.clipped);
      frame_.effects.push_back(effect);
      command.effectIndex = static_cast<std::uint32_t>(frame_.effects.size());
      try {
        frame_.commands.push_back(command);
      } catch (...) {
        frame_.effects.pop_back();
        throw;
      }
      continue;
    }
    if ((flags & NodeFlags::hasBlurFilter) &&
      kind != static_cast<std::uint32_t>(NodeKind::sprite)) return false;
    if (state.maskImage && kind != static_cast<std::uint32_t>(NodeKind::sprite) &&
        kind != static_cast<std::uint32_t>(NodeKind::tileLayer) &&
        kind != static_cast<std::uint32_t>(NodeKind::mesh) &&
        kind != static_cast<std::uint32_t>(NodeKind::container)) return false;

    if (kind == static_cast<std::uint32_t>(NodeKind::screenFill)) {
      const float red = static_cast<float>((tint >> 16U) & 0xffU) / 255.0F;
      const float green = static_cast<float>((tint >> 8U) & 0xffU) / 255.0F;
      const float blue = static_cast<float>(tint & 0xffU) / 255.0F;
      // Match the fractional-alpha ScreenSprite draw in Chromium 65/Pixi 4.
      queueQuad(0, 0, static_cast<float>(queueWidth_),
                static_cast<float>(queueHeight_),
                {red, green, blue, std::floor(state.alpha * 255.0F) / 255.0F});
      frame_.commands.back().primitive = RenderCommand::Primitive::screenFill;
      setCommandClip(frame_.commands.back(), state.clip, state.clipped);
      continue;
    }
    if (kind == static_cast<std::uint32_t>(NodeKind::toneAdjust)) {
      std::array<float, 20> matrix{};
      std::copy_n(values + valueOffset + 7, 20, matrix.begin());
      frame_.colorMatrices.push_back(matrix);
      RenderCommand command;
      command.color[3] = state.alpha;
      command.colorMatrixIndex =
        static_cast<std::uint32_t>(frame_.colorMatrices.size());
      try {
        frame_.commands.push_back(command);
      } catch (...) {
        frame_.colorMatrices.pop_back();
        throw;
      }
      continue;
    }
    if (kind == static_cast<std::uint32_t>(NodeKind::tileLayer) ||
        kind == static_cast<std::uint32_t>(NodeKind::mesh)) {
      const std::array<float, 2> animation =
        kind == static_cast<std::uint32_t>(NodeKind::tileLayer)
          ? std::array<float, 2>{values[valueOffset + 15],
                                 values[valueOffset + 16]}
          : std::array<float, 2>{0, 0};
      if (!queueTileLayer(resource, state.world,
            animation,
            state.alpha, tint, blendMode)) return false;
      setCommandClip(frame_.commands.back(), state.clip, state.clipped);
      if (state.maskImage && !images_.beginUse(state.maskImage)) return false;
      frame_.commands.back().maskImage = state.maskImage;
      if (state.maskImage) {
        frame_.maskTransforms.push_back(state.maskTransform);
        frame_.commands.back().maskTransformIndex =
          static_cast<std::uint32_t>(frame_.maskTransforms.size());
      }
      frame_.commands.back().nearest =
        kind == static_cast<std::uint32_t>(NodeKind::tileLayer) ||
        (flags & NodeFlags::nearestSampling);
      frame_.commands.back().primitive =
        kind == static_cast<std::uint32_t>(NodeKind::tileLayer)
          ? RenderCommand::Primitive::tileLayer
          : RenderCommand::Primitive::mesh;
      if (flags & NodeFlags::hasSpriteColor) return false;
      if (flags & (NodeFlags::hasMeshPostTintOverlay | NodeFlags::hasMvBitmapBlend)) {
        const auto* material = std::get_if<MvBitmapMaterial>(&tileLayers_.at(resource).material);
        const bool mvBlend = flags & NodeFlags::hasMvBitmapBlend;
        if (mvBlend != (material != nullptr) ||
            ((flags & NodeFlags::hasMeshPostTintOverlay) && mvBlend)) return false;
        frame_.commands.back().appliesMeshPostTintOverlay = !mvBlend;
        ColorEffectPayload colorEffect{};
        std::copy_n(values + valueOffset + 37, 4,
                    colorEffect.blendColor.begin());
        if (colorEffect.blendColor[3] < 0 ||
            colorEffect.blendColor[3] > 1) return false;
        frame_.colorEffects.push_back(colorEffect);
        frame_.commands.back().colorEffectIndex =
          static_cast<std::uint32_t>(frame_.colorEffects.size());
      }
      continue;
    }

    auto placed = state.world;
    const float localX = values[valueOffset + 7];
    const float localY = values[valueOffset + 8];
    placed[4] += state.world[0] * localX + state.world[2] * localY;
    placed[5] += state.world[1] * localX + state.world[3] * localY;
    const std::array<float, 2> destination = {
      values[valueOffset + 13], values[valueOffset + 14]
    };
    const auto corner = [&](float x, float y) {
      return std::array<float, 2>{placed[0] * x + placed[2] * y + placed[4],
                                  placed[1] * x + placed[3] * y + placed[5]};
    };
    std::array<std::array<float, 2>, 4> worldVertices{};
    if (spriteVertices) {
      for (std::size_t vertex = 0; vertex < 4; ++vertex) {
        const std::size_t offset = valueOffset + (vertex < 3 ? vertex * 2 : 15);
        worldVertices[vertex] = {values[offset], values[offset + 1]};
      }
    }
    const auto p0 = spriteVertices ? worldVertices[0] : corner(0, 0);
    const auto p1 = spriteVertices ? worldVertices[1] : corner(destination[0], 0);
    const auto p2 = spriteVertices ? worldVertices[2] : corner(destination[0], destination[1]);
    const auto p3 = spriteVertices ? worldVertices[3] : corner(0, destination[1]);
    const float minimumX = std::min({p0[0], p1[0], p2[0], p3[0]});
    const float maximumX = std::max({p0[0], p1[0], p2[0], p3[0]});
    const float minimumY = std::min({p0[1], p1[1], p2[1], p3[1]});
    const float maximumY = std::max({p0[1], p1[1], p2[1], p3[1]});
    if (filterDepth == 0 &&
        (maximumX <= 0 || maximumY <= 0 || minimumX >= queueWidth_ ||
         minimumY >= queueHeight_)) continue;

    const auto imageInfo = images_.lookup(resource);
    if (!imageInfo) return false;
    if (kind == static_cast<std::uint32_t>(NodeKind::sprite)) {
      if (!queueImage(resource, placed,
            {values[valueOffset + 9], values[valueOffset + 10],
             values[valueOffset + 11], values[valueOffset + 12]},
            state.alpha, tint, blendMode, &*imageInfo)) return false;
    } else if (kind == static_cast<std::uint32_t>(NodeKind::tilingSprite)) {
      if (!queueTiled(resource, placed,
            {values[valueOffset + 9], values[valueOffset + 10],
             values[valueOffset + 11], values[valueOffset + 12]},
            destination, state.alpha, tint, blendMode, &*imageInfo)) return false;
    } else {
      return false;
    }
    setCommandClip(frame_.commands.back(), state.clip, state.clipped);
    frame_.commands.back().blur =
      flags & NodeFlags::hasBlurFilter ? values[valueOffset + 21] : 0.0F;
    frame_.commands.back().nearest = flags & NodeFlags::nearestSampling;
    frame_.commands.back().clampedTilingSampling = flags & NodeFlags::clampedTilingSampling;
    frame_.commands.back().roundPixels = flags & NodeFlags::roundPixels;
    frame_.commands.back().textureRotation = textureRotation;
    frame_.commands.back().primitive =
      kind == static_cast<std::uint32_t>(NodeKind::tilingSprite)
        ? RenderCommand::Primitive::tilingSprite
        : RenderCommand::Primitive::sprite;
    frame_.commands.back().spriteWorldVertices = spriteVertices;
    if (spriteVertices) {
      frame_.spriteVertices.push_back(worldVertices);
      frame_.commands.back().spriteVerticesIndex =
        static_cast<std::uint32_t>(frame_.spriteVertices.size());
    }
    frame_.commands.back().standaloneBitmapRegion = flags & NodeFlags::standaloneBitmapRegion;
    frame_.commands.back().packedSpriteColor = flags & NodeFlags::packedSpriteColor;
    if (frame_.commands.back().packedSpriteColor) {
      frame_.commands.back().color = { float((tint >> 16) & 255) / 255,
        float((tint >> 8) & 255) / 255, float(tint & 255) / 255, float(tint >> 24) / 255 };
    }
    frame_.commands.back().premultipliedSpriteTexture =
      (flags & NodeFlags::premultipliedSpriteTexture) || imageInfo->premultiplied;
    frame_.commands.back().pixiSpritePacking = kind == static_cast<std::uint32_t>(NodeKind::sprite);
    frame_.commands.back().appliesSpriteColor = flags & NodeFlags::hasSpriteColor;
    if (frame_.commands.back().appliesSpriteColor) {
      if (kind != static_cast<std::uint32_t>(NodeKind::sprite)) return false;
      ColorEffectPayload colorEffect{};
      std::copy_n(values + valueOffset + 33, 4,
                  colorEffect.colorTone.begin());
      std::copy_n(values + valueOffset + 37, 4,
                  colorEffect.blendColor.begin());
      if (colorEffect.colorTone[3] < 0 ||
          colorEffect.colorTone[3] > 1 ||
          colorEffect.blendColor[3] < 0 ||
          colorEffect.blendColor[3] > 1) return false;
      frame_.colorEffects.push_back(colorEffect);
      frame_.commands.back().colorEffectIndex =
        static_cast<std::uint32_t>(frame_.colorEffects.size());
    }
    if (!std::isfinite(frame_.commands.back().blur) ||
        frame_.commands.back().blur < 0) return false;
    if (state.maskImage && !images_.beginUse(state.maskImage)) return false;
    frame_.commands.back().maskImage = state.maskImage;
    if (state.maskImage) {
      frame_.maskTransforms.push_back(state.maskTransform);
      frame_.commands.back().maskTransformIndex =
        static_cast<std::uint32_t>(frame_.maskTransforms.size());
    }
  }
  return filterDepth == 0;
  };

  try {
    if (build()) {
      sceneSubmittedThisFrame_ = true;
      return true;
    }
  } catch (...) {
    discardCommandsFrom(originalCommandCount);
    frame_.effects.resize(originalEffectCount);
    frame_.colorMatrices.resize(originalColorMatrixCount);
    frame_.filterPayloads.resize(originalFilterPayloadCount);
    frame_.filterParameters.resize(originalFilterParameterCount);
    frame_.customFilterPlans.resize(originalCustomFilterPlanCount);
    frame_.colorEffects.resize(originalColorEffectCount);
    frame_.clips.resize(originalClipCount);
    frame_.maskTransforms.resize(originalMaskTransformCount);
    frame_.spriteVertices.resize(originalSpriteVerticesCount);
    sceneSubmittedThisFrame_ = originalSceneSubmitted;
    sceneHasEffect_ = originalSceneHasEffect;
    sceneHasCustomFilter_ = originalSceneHasCustomFilter;
    throw;
  }
  discardCommandsFrom(originalCommandCount);
  frame_.effects.resize(originalEffectCount);
  frame_.colorMatrices.resize(originalColorMatrixCount);
  frame_.filterParameters.resize(originalFilterParameterCount);
  frame_.customFilterPlans.resize(originalCustomFilterPlanCount);
  frame_.colorEffects.resize(originalColorEffectCount);
  frame_.maskTransforms.resize(originalMaskTransformCount);
  frame_.spriteVertices.resize(originalSpriteVerticesCount);
  sceneSubmittedThisFrame_ = originalSceneSubmitted;
  sceneHasEffect_ = originalSceneHasEffect;
  sceneHasCustomFilter_ = originalSceneHasCustomFilter;
  return false;
}


}  // namespace pmjs
