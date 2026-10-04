#include "runtime_core.hpp"
#include "scene_packet.hpp"

#include <cstdlib>
#include <iostream>
#include <utility>
#include <vector>

namespace pmjs {

RuntimeCore::RuntimeCore(const std::filesystem::path& root, int width, int height,
                         const std::string& title)
    : width_(width), height_(height), platform_(width, height, title),
      canvases_(images_), renderer_(width, height, images_), vfs_(root),
      media_(root),
      dialog_(platform_, renderer_, canvases_, vfs_, width, height), effects_(vfs_, media_) {
  renderer_.setEffects(&effects_);
}

std::optional<ImageHandle> RuntimeCore::resolveImage(std::uint32_t handle) {
  return images_.lookup(handle) ? std::optional<ImageHandle>{handle}
                                : canvases_.prepareImage(handle);
}

bool RuntimeCore::submitScene(std::uint32_t version,
                              const std::uint32_t* metadata,
                              std::size_t metadataCount, const float* values,
                              std::size_t valueCount, std::size_t nodeCount) {
  const std::size_t requiredMetadata =
      nodeCount * scene_packet::metadataStride;
  if (version != scene_packet::version ||
      (nodeCount > 0 && (!metadata || !values)) ||
      nodeCount > scene_packet::maxNodes ||
      metadataCount < requiredMetadata ||
      valueCount < nodeCount * scene_packet::valueStride ||
      nodeCount * (scene_packet::metadataStride * sizeof(std::uint32_t) +
                   scene_packet::valueStride * sizeof(float)) >
        scene_packet::maxPacketBytes) return false;

  // Most scene packets already contain native ImageStore handles. Do not copy
  // the full metadata packet merely to write identical handles back into it.
  // Allocate/copy lazily only if a canvas handle actually resolves to a
  // different native image handle.
  sceneMetadataScratch_.clear();
  bool copied = false;
  const auto ensureCopy = [&]() {
    if (copied) return;
    sceneMetadataScratch_.assign(metadata, metadata + requiredMetadata);
    copied = true;
  };
  const auto current = [&](std::size_t offset) -> std::uint32_t {
    return copied ? sceneMetadataScratch_[offset] : metadata[offset];
  };
  const auto resolveAt = [&](std::size_t offset) -> std::optional<ImageHandle> {
    const std::uint32_t handle = current(offset);
    const auto resolved = resolveImage(handle);
    if (!resolved) return std::nullopt;
    if (*resolved != handle) {
      ensureCopy();
      sceneMetadataScratch_[offset] = *resolved;
    }
    return resolved;
  };

  for (std::size_t index = 0; index < nodeCount; ++index) {
    const std::size_t offset = index * scene_packet::metadataStride;
    const auto kindValue = current(offset);
    if (kindValue > static_cast<std::uint32_t>(scene_packet::NodeKind::effect)) {
      return false;
    }
    const auto kind = static_cast<scene_packet::NodeKind>(kindValue);
    const auto flags = current(offset + 5);
    if (flags & ~scene_packet::kAllowedNodeFlags) return false;

    if (flags & scene_packet::NodeFlags::hasAlphaMask) {
      const std::uint32_t originalMask = current(offset + 6);
      const auto mask = resolveAt(offset + 6);
      if (!mask) {
        std::cerr << "[pmjs-scene] invalid alpha-mask handle="
                  << originalMask << " node=" << index << '\n';
        return false;
      }
    }

    if (kind == scene_packet::NodeKind::filterBegin &&
        (current(offset + 4) ==
           static_cast<std::uint32_t>(scene_packet::FilterKind::displacement) ||
         current(offset + 4) ==
           static_cast<std::uint32_t>(scene_packet::FilterKind::alphaMask))) {
      const std::uint32_t originalImage = current(offset + 2);
      const auto image = resolveAt(offset + 2);
      if (!image) {
        std::cerr << "[pmjs-scene] invalid filter image handle="
                  << originalImage << " node=" << index << '\n';
        return false;
      }
    }

    if (kind != scene_packet::NodeKind::sprite &&
        kind != scene_packet::NodeKind::tilingSprite) {
      continue;
    }
    const std::uint32_t originalImage = current(offset + 2);
    const auto image = resolveAt(offset + 2);
    if (!image) {
      std::cerr << "[pmjs-scene] invalid sprite image handle="
                << originalImage << " node=" << index
                << " kind=" << static_cast<std::uint32_t>(kind) << '\n';
      return false;
    }
  }

  // A one-off canvas-heavy/huge scene should not pin a large metadata block
  // once normal direct-image packets resume.
  if (!copied && sceneMetadataScratch_.capacity() > 4096 &&
      requiredMetadata * 4 < sceneMetadataScratch_.capacity()) {
    std::vector<std::uint32_t>().swap(sceneMetadataScratch_);
  }

  const std::uint32_t* queuedMetadata =
      copied ? sceneMetadataScratch_.data() : metadata;
  const bool queued = renderer_.queueScene(
    version, nodeCount ? queuedMetadata : nullptr, requiredMetadata,
    nodeCount ? values : nullptr, valueCount, nodeCount);
  if (!queued) {
    std::cerr << "[pmjs-scene] renderer rejected packet nodes="
              << nodeCount << '\n';
  }
  return queued;
}

bool RuntimeCore::pollEvents() {
  running_ = running_ && platform_.pollEvents();
  return running_;
}

void RuntimeCore::syncDrawableSize() {
  const auto size = platform_.drawableSize();
  renderer_.setDrawableSize(size.first, size.second);
}




}  // namespace pmjs
