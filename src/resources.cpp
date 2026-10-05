#include "resources.hpp"
#include "checked_bounds.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <setjmp.h>
#include <vector>

#include <GLES3/gl3.h>
#include <jpeglib.h>
#include <png.h>

namespace pmjs {

namespace {
constexpr std::uint32_t indexMask = 0xffffU;
constexpr std::uint16_t generationMask = 0x7fffU;

std::optional<ImagePixels> decodePngFromMemory(const void* data, std::size_t size) {
  if (!data || size < 8) return std::nullopt;
  png_image image{};
  image.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_memory(&image, data, size)) return std::nullopt;
  image.format = PNG_FORMAT_RGBA;
  const auto extent = checkedImageExtent(static_cast<int>(image.width), static_cast<int>(image.height));
  if (!extent) {
    png_image_free(&image);
    return std::nullopt;
  }
  ImagePixels result;
  result.width = extent->width;
  result.height = extent->height;
  result.rgba.resize(extent->rgbaBytes);
  if (!png_image_finish_read(&image, nullptr, result.rgba.data(), 0, nullptr)) {
    png_image_free(&image);
    return std::nullopt;
  }
  png_image_free(&image);
  return result;
}

struct JpegError {
  jpeg_error_mgr base;
  jmp_buf recovery;
};

void recoverJpegError(j_common_ptr decoder) {
  auto* error = reinterpret_cast<JpegError*>(decoder->err);
  longjmp(error->recovery, 1);
}

std::optional<ImagePixels> decodeJpegFromMemory(const void* data, std::size_t size) {
  if (!data || size < 4) return std::nullopt;
  jpeg_decompress_struct decoder{};
  JpegError error{};
  auto* result = new ImagePixels;
  std::uint8_t* rowBuffer = nullptr;
  decoder.err = jpeg_std_error(&error.base);
  error.base.error_exit = recoverJpegError;
  if (setjmp(error.recovery)) {
    std::free(rowBuffer);
    delete result;
    jpeg_destroy_decompress(&decoder);
    return std::nullopt;
  }
  jpeg_create_decompress(&decoder);
  jpeg_mem_src(&decoder, static_cast<const unsigned char*>(data), size);
  if (jpeg_read_header(&decoder, TRUE) != JPEG_HEADER_OK) {
    delete result;
    jpeg_destroy_decompress(&decoder);
    return std::nullopt;
  }
  const auto extent = checkedImageExtent(
      static_cast<int>(decoder.image_width),
      static_cast<int>(decoder.image_height));
  if (!extent) {
    delete result;
    jpeg_destroy_decompress(&decoder);
    return std::nullopt;
  }
  decoder.out_color_space = JCS_RGB;
  jpeg_start_decompress(&decoder);
  const std::size_t width = static_cast<std::size_t>(extent->width);
  result->width = extent->width;
  result->height = extent->height;
  try {
    result->rgba.resize(extent->rgbaBytes);
  } catch (...) {
    delete result;
    jpeg_destroy_decompress(&decoder);
    throw;
  }
  rowBuffer = static_cast<std::uint8_t*>(std::malloc(width * 3U));
  if (!rowBuffer) {
    delete result;
    jpeg_destroy_decompress(&decoder);
    return std::nullopt;
  }
  while (decoder.output_scanline < decoder.output_height) {
    const std::size_t rowIndex =
      static_cast<std::size_t>(decoder.output_scanline);
    JSAMPROW row = rowBuffer;
    jpeg_read_scanlines(&decoder, &row, 1);
    auto* destination = result->rgba.data() + rowIndex * width * 4U;
    for (std::size_t column = 0; column < width; ++column) {
      const std::size_t source = column * 3U;
      const std::size_t target = column * 4U;
      destination[target] = rowBuffer[source];
      destination[target + 1U] = rowBuffer[source + 1U];
      destination[target + 2U] = rowBuffer[source + 2U];
      destination[target + 3U] = 255;
    }
  }
  std::free(rowBuffer);
  rowBuffer = nullptr;
  jpeg_finish_decompress(&decoder);
  jpeg_destroy_decompress(&decoder);

  ImagePixels output = std::move(*result);
  delete result;
  return output;
}

std::optional<ImagePixels> decodeMemory(const void* data, std::size_t size) {
  if (!data || size < 4) return std::nullopt;
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  if (size >= 8 && png_sig_cmp(bytes, 0, 8) == 0) {
    return decodePngFromMemory(data, size);
  }
  if (size >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff) {
    return decodeJpegFromMemory(data, size);
  }
  return std::nullopt;
}

std::optional<ImagePixels> readTexturePixels(const ImageInfo& image) {
  const auto extent = checkedImageExtent(image.width, image.height);
  if (!extent) return std::nullopt;
  ImagePixels pixels{image.width, image.height,
                     std::vector<std::uint8_t>(extent->rgbaBytes)};
  GLint previousRead = 0, previousBuffer = 0;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);
  glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousBuffer);
  constexpr std::array<GLenum, 4> parameters = {
    GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH, GL_PACK_SKIP_PIXELS, GL_PACK_SKIP_ROWS
  };
  std::array<GLint, 4> previousPack{};
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    glGetIntegerv(parameters[index], &previousPack[index]);
    glPixelStorei(parameters[index], index == 0 ? 1 : 0);
  }
  GLuint framebuffer = 0;
  glGenFramebuffers(1, &framebuffer);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
  if (framebuffer) {
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                          GL_TEXTURE_2D, image.texture, 0);
  }
  const bool complete = framebuffer != 0 &&
    glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  // Read back the exact uploaded snapshot. Reopening sourcePath here would be
  // incorrect if the file was atomically replaced after the texture was loaded.
  if (complete) {
    glReadPixels(0, 0, image.width, image.height, GL_RGBA,
                 GL_UNSIGNED_BYTE, pixels.rgba.data());
  }
  glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(previousBuffer));
  glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousRead));
  glDeleteFramebuffers(1, &framebuffer);
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    glPixelStorei(parameters[index], previousPack[index]);
  }
  return complete ? std::optional<ImagePixels>(std::move(pixels)) : std::nullopt;
}

bool premultiplyRgba(const void* sourcePixels, int width, int height,
                     int sourceRowPixels, std::vector<std::uint8_t>* output) {
  if (!sourcePixels || width <= 0 || height <= 0 || sourceRowPixels < width) {
    return false;
  }
  const auto* source = static_cast<const std::uint8_t*>(sourcePixels);
  if (output) {
    output->resize(static_cast<std::size_t>(width) * height * 4U);
  }
  bool changed = false;
  for (int row = 0; row < height; ++row) {
    const auto* sourceRow =
      source + static_cast<std::size_t>(row) * sourceRowPixels * 4U;
    auto* destinationRow = output
      ? output->data() + static_cast<std::size_t>(row) * width * 4U
      : nullptr;
    for (int column = 0; column < width; ++column) {
      const auto* sourcePixel = sourceRow + static_cast<std::size_t>(column) * 4U;
      auto* destinationPixel = destinationRow
        ? destinationRow + static_cast<std::size_t>(column) * 4U
        : nullptr;
      const unsigned alpha = sourcePixel[3];
      if (alpha == 255U) {
        if (destinationPixel) std::memcpy(destinationPixel, sourcePixel, 4U);
        continue;
      }
      if (alpha == 0U) {
        changed |= sourcePixel[0] != 0 || sourcePixel[1] != 0 ||
                   sourcePixel[2] != 0;
        if (destinationPixel) {
          destinationPixel[0] = 0;
          destinationPixel[1] = 0;
          destinationPixel[2] = 0;
          destinationPixel[3] = 0;
        }
        continue;
      }
      for (std::size_t channel = 0; channel < 3; ++channel) {
        const auto converted = static_cast<std::uint8_t>(
          (sourcePixel[channel] * alpha + 127U) / 255U);
        changed |= converted != sourcePixel[channel];
        if (destinationPixel) destinationPixel[channel] = converted;
      }
      if (destinationPixel) destinationPixel[3] = sourcePixel[3];
    }
  }
  return changed;
}
}

ImageFileSource::~ImageFileSource() {
  if (descriptor_ >= 0) close(descriptor_);
}

std::unique_ptr<ImageFileSource> ImageStore::openFile(const std::filesystem::path& path) {
  std::error_code error;
  const auto resolved = std::filesystem::canonical(path, error);
  if (error) return nullptr;
  std::unique_ptr<ImageFileSource> source(new ImageFileSource());
  source->descriptor_ = open(resolved.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  struct stat info{};
  if (source->descriptor_ < 0 || fstat(source->descriptor_, &info) != 0 ||
      !S_ISREG(info.st_mode) || info.st_size <= 0 || info.st_size > 64 * 1024 * 1024) return nullptr;
  source->path_ = resolved;
  source->size_ = static_cast<std::size_t>(info.st_size);
  source->modifiedSeconds_ = info.st_mtim.tv_sec;
  source->modifiedNanoseconds_ = info.st_mtim.tv_nsec;
  source->key_ = resolved.generic_string() + ':' + std::to_string(info.st_dev) + ':' +
    std::to_string(info.st_ino) + ':' + std::to_string(info.st_size) + ':' +
    std::to_string(info.st_mtim.tv_sec) + ':' + std::to_string(info.st_mtim.tv_nsec) + ':' +
    std::to_string(info.st_ctim.tv_sec) + ':' + std::to_string(info.st_ctim.tv_nsec);
  return source;
}

ImageStore::~ImageStore() {
  for (auto& slot : slots_) {
    if (slot.live) {
      clearPremultipliedTexture(slot);
      glDeleteTextures(1, &slot.texture);
    }
  }
}

ImageHandle ImageStore::fallbackHandle() {
  if (fallbackHandle_ != 0 && lookup(fallbackHandle_)) return fallbackHandle_;
  std::array<std::uint32_t, 16> checkerboard{};
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      const bool magenta = ((x ^ y) & 1) != 0;
      checkerboard[static_cast<std::size_t>(y * 4 + x)] = magenta ? 0xffff00ffU : 0xff000000U;
    }
  }
  auto created = createRgba(4, 4, checkerboard.data());
  if (!created) return 0;
  pin(created->handle);
  const std::size_t index = (created->handle & indexMask) - 1U;
  slots_[index].references = 0;
  fallbackHandle_ = created->handle;
  return fallbackHandle_;
}

std::optional<ImageInfo> ImageStore::acquireFallback() {
  auto handle = fallbackHandle();
  if (handle == 0 || !retain(handle)) return std::nullopt;
  ++fallbackUses_;
  return lookup(handle);
}

std::size_t ImageStore::fallbackReferences() const {
  if (fallbackHandle_ == 0) return 0;
  const std::size_t index = (fallbackHandle_ & indexMask) - 1U;
  if (index >= slots_.size() || !slots_[index].live) return 0;
  return slots_[index].references;
}

std::optional<ImagePixels> ImageStore::decodeMemory(const void* data, std::size_t size) {
  return pmjs::decodeMemory(data, size);
}

std::optional<ImagePixels> ImageStore::decodePngFromMemory(const void* data, std::size_t size) {
  return pmjs::decodePngFromMemory(data, size);
}

std::optional<ImagePixels> ImageStore::decodeJpegFromMemory(const void* data, std::size_t size) {
  return pmjs::decodeJpegFromMemory(data, size);
}

std::optional<ImagePixels> ImageStore::decodeFile(const ImageFileSource& source) {
  std::vector<std::uint8_t> bytes(source.size_);
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto read = pread(source.descriptor_, bytes.data() + offset,
                            bytes.size() - offset, static_cast<off_t>(offset));
    if (read < 0 && errno == EINTR) continue;
    if (read <= 0) return std::nullopt;
    offset += static_cast<std::size_t>(read);
  }
  struct stat current{};
  // Atomic replacement can unlink this inode and change ctime without changing
  // its bytes. Size and mtime detect writes to the held source instead.
  if (fstat(source.descriptor_, &current) != 0 ||
      current.st_size != static_cast<off_t>(source.size_) ||
      current.st_mtim.tv_sec != source.modifiedSeconds_ ||
      current.st_mtim.tv_nsec != source.modifiedNanoseconds_) {
    return std::nullopt;
  }
  return decodeMemory(bytes.data(), bytes.size());
}

std::optional<ImageInfo> ImageStore::loadPng(const std::filesystem::path& path,
                                             bool retainCpuPixels) {
  auto source = openFile(path);
  if (!source) return std::nullopt;
  if (auto cached = acquireCached(*source)) {
    if (retainCpuPixels) this->retainCpuPixels(cached->handle);
    return cached;
  }
  auto pixels = decodeFile(*source);
  if (!pixels) return std::nullopt;
  return installDecoded(*source, std::move(*pixels), retainCpuPixels);
}

std::optional<ImageInfo> ImageStore::installDecoded(
    const ImageFileSource& source, ImagePixels pixels,
    bool retainCpuPixels) {
  if (auto cached = acquireCached(source)) {
    if (retainCpuPixels) this->retainCpuPixels(cached->handle);
    return cached;
  }
  const std::string& cacheKey = source.key();

  auto created = createRgba(pixels.width, pixels.height, pixels.rgba.data());
  if (!created) return std::nullopt;
  const std::size_t index = (created->handle & indexMask) - 1U;
  slots_[index].cacheKey = cacheKey;
  slots_[index].sourcePath = source.path();
  slots_[index].retainCpuPixels = retainCpuPixels;
  if (retainCpuPixels) {
    slots_[index].cachedPixels = std::move(pixels);
    slots_[index].cpuPixelFrames = 0;
    cpuBytes_ += slots_[index].cachedPixels->rgba.capacity();
  }
  pathCache_.emplace(cacheKey, created->handle);
  return created;
}

std::optional<ImageInfo> ImageStore::installDecodedMemory(
    ImagePixels pixels, bool /* retainCpuPixels */) {
  auto created = createRgba(pixels.width, pixels.height, pixels.rgba.data());
  if (!created) return std::nullopt;
  const std::size_t index = (created->handle & indexMask) - 1U;
  // Memory images have no path to decode again after their load bytes are gone.
  slots_[index].retainCpuPixels = true;
  slots_[index].cachedPixels = std::move(pixels);
  slots_[index].cpuPixelFrames = 0;
  cpuBytes_ += slots_[index].cachedPixels->rgba.capacity();
  return created;
}

bool ImageStore::retainCpuPixels(ImageHandle handle) {
  const auto info = lookup(handle);
  if (!info) return false;
  const std::size_t index = (handle & indexMask) - 1U;
  auto& slot = slots_[index];
  if (slot.cachedPixels) {
    slot.retainCpuPixels = true;
    slot.cpuPixelFrames = 0;
    return true;
  }
  if (slot.cacheKey.empty()) return false;
  slot.cachedPixels = readTexturePixels(*info);
  if (!slot.cachedPixels) return false;
  cpuBytes_ += slot.cachedPixels->rgba.capacity();
  slot.retainCpuPixels = true;
  slot.cpuPixelFrames = 0;
  return true;
}

std::optional<ImageInfo> ImageStore::acquireCached(
    const ImageFileSource& source) {
  const std::string& cacheKey = source.key();
  const auto cached = pathCache_.find(cacheKey);
  if (cached != pathCache_.end()) {
    const auto info = lookup(cached->second);
    if (info) {
      const std::size_t index = (cached->second & indexMask) - 1U;
      const bool wasWarm = slots_[index].references == 0 &&
        slots_[index].pins == 0 &&
        slots_[index].inFlight.load(std::memory_order_acquire) == 0;
      ++slots_[index].references;
      markUsed(slots_[index]);
      ++cacheHits_;
      if (wasWarm) ++warmHits_;
      return info;
    }
    pathCache_.erase(cached);
  }
  return std::nullopt;
}

const ImagePixels* ImageStore::readPixels(ImageHandle handle) const {
  const std::uint32_t encodedIndex = handle & indexMask;
  const auto info = lookup(handle);
  if (!info || encodedIndex == 0) return nullptr;
  const auto& slot = slots_[encodedIndex - 1U];
  if (slot.cachedPixels) {
    if (!slot.retainCpuPixels && !slot.cacheKey.empty()) {
      slot.cpuPixelFrames = transientCpuPixelFrames_;
      transientCpuPixelsActive_ = true;
    }
    return &*slot.cachedPixels;
  }
  if (slot.cacheKey.empty()) return nullptr;
  slot.cachedPixels = readTexturePixels(*info);
  if (!slot.cachedPixels) return nullptr;
  cpuBytes_ += slot.cachedPixels->rgba.capacity();
  if (!slot.retainCpuPixels) {
    slot.cpuPixelFrames = transientCpuPixelFrames_;
    transientCpuPixelsActive_ = true;
  }
  return &*slot.cachedPixels;
}

std::optional<ImageInfo> ImageStore::createRgba(int width, int height,
                                                 const void* pixels, bool premultiplied) {
  const auto extent = checkedImageExtent(width, height);
  if (!extent) return std::nullopt;
  GLuint texture = 0;
  while (glGetError() != GL_NO_ERROR) {}
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, pixels);
  if (texture == 0 || glGetError() != GL_NO_ERROR) {
    if (texture) glDeleteTextures(1, &texture);
    return std::nullopt;
  }
  ++textureCreates_;
  if (pixels) {
    textureUploadBytes_ += static_cast<std::uint64_t>(width) *
        static_cast<std::uint64_t>(height) * 4U;
  }

  std::size_t index = 0;
  if (!freeSlots_.empty()) {
    index = freeSlots_.back();
    freeSlots_.pop_back();
  } else {
    index = slots_.size();
    if (index >= indexMask) {
      glDeleteTextures(1, &texture);
      return std::nullopt;
    }
    slots_.emplace_back();
  }
  auto& slot = slots_[index];
  slot.texture = texture;
  slot.width = width;
  slot.height = height;
  slot.references = 1;
  slot.inFlight.store(0, std::memory_order_relaxed);
  slot.pins = 0;
  markUsed(slot);
  slot.cpuPixelFrames = 0;
  slot.retainCpuPixels = false;
  slot.cacheKey.clear();
  slot.sourcePath.clear();
  slot.cachedPixels.reset();
  slot.gpuOnly = pixels == nullptr;
  slot.renderTarget = false;
  slot.premultiplied = premultiplied;
  slot.premultiplyIdentity = !premultiplied && pixels != nullptr &&
    !premultiplyRgba(pixels, width, height, width, nullptr);
  slot.live = true;
  ++liveCount_;
  gpuBytes_ += extent->rgbaBytes;
  peakGpuBytes_ = std::max(peakGpuBytes_, gpuBytes_);
  return ImageInfo{makeHandle(index, slot.generation), width, height, texture, premultiplied};
}

std::optional<ImageInfo> ImageStore::createRenderTarget(int width, int height,
                                                       bool premultiplied) {
  auto image = createRgba(width, height, nullptr, premultiplied);
  if (image) slots_[(image->handle & indexMask) - 1U].renderTarget = true;
  return image;
}

std::size_t ImageStore::residentBytes(const Slot& slot) {
  return static_cast<std::size_t>(slot.width) *
      static_cast<std::size_t>(slot.height) * 4U *
      (slot.premultipliedTexture && slot.premultipliedTexture != slot.texture ? 2U : 1U) +
      (slot.cachedPixels ? slot.cachedPixels->rgba.capacity() : 0U);
}

std::size_t ImageStore::warmBytes() const {
  std::size_t result = 0;
  for (const auto& slot : slots_) {
    if (slot.live && slot.references == 0 && slot.pins == 0 &&
        slot.inFlight.load(std::memory_order_acquire) == 0 &&
        !slot.cacheKey.empty()) result += residentBytes(slot);
  }
  return result;
}

std::size_t ImageStore::warmCount() const {
  std::size_t result = 0;
  for (const auto& slot : slots_) {
    if (slot.live && slot.references == 0 && slot.pins == 0 &&
        slot.inFlight.load(std::memory_order_acquire) == 0 &&
        !slot.cacheKey.empty()) ++result;
  }
  return result;
}

std::size_t ImageStore::pinnedBytes() const {
  std::size_t result = 0;
  for (std::size_t i = 0; i < slots_.size(); ++i) {
    const auto& slot = slots_[i];
    if (!slot.live || slot.pins == 0) continue;
    const ImageHandle h = makeHandle(i, slot.generation);
    if (h == fallbackHandle_) continue;  // store-internal; exclude from user metrics
    result += residentBytes(slot);
  }
  return result;
}

std::size_t ImageStore::pinnedCount() const {
  std::size_t result = 0;
  for (std::size_t i = 0; i < slots_.size(); ++i) {
    const auto& slot = slots_[i];
    if (!slot.live || slot.pins == 0) continue;
    const ImageHandle h = makeHandle(i, slot.generation);
    if (h == fallbackHandle_) continue;  // store-internal; exclude from user metrics
    ++result;
  }
  return result;
}

std::vector<ImageMemoryEntry> ImageStore::memoryEntries() const {
  std::vector<ImageMemoryEntry> result;
  result.reserve(liveCount_);
  for (std::size_t index = 0; index < slots_.size(); ++index) {
    const auto& slot = slots_[index];
    if (!slot.live) continue;
    result.push_back({makeHandle(index, slot.generation), slot.width, slot.height,
      slot.references, slot.inFlight.load(std::memory_order_acquire), slot.pins,
      static_cast<std::size_t>(slot.width) * slot.height * 4U *
        (slot.premultipliedTexture && slot.premultipliedTexture != slot.texture ? 2U : 1U),
      slot.cachedPixels ? slot.cachedPixels->rgba.capacity() : 0U,
      slot.lastUsedSerial,
      slot.references == 0 && slot.pins == 0 &&
        slot.inFlight.load(std::memory_order_acquire) == 0 &&
        !slot.cacheKey.empty(),
      slot.sourcePath.generic_string()});
  }
  return result;
}

bool ImageStore::updateRgba(ImageHandle handle, const void* pixels) {
  const auto info = lookup(handle);
  if (!info || !pixels) return false;
  while (glGetError() != GL_NO_ERROR) {}
  glBindTexture(GL_TEXTURE_2D, info->texture);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, info->width, info->height,
                  GL_RGBA, GL_UNSIGNED_BYTE, pixels);
  const bool ok = glGetError() == GL_NO_ERROR;
  if (ok) {
    auto& slot = slots_[(handle & indexMask) - 1U];
    const std::size_t uploadBytes = static_cast<std::size_t>(info->width) *
      static_cast<std::size_t>(info->height) * 4U;
    if (slot.cachedPixels && slot.cachedPixels->rgba.size() == uploadBytes) {
      std::memcpy(slot.cachedPixels->rgba.data(), pixels, uploadBytes);
    }
    if (!slot.premultiplied) {
      bool identity = false;
      if (slot.premultipliedTexture &&
          slot.premultipliedTexture != slot.texture) {
        const std::size_t required = static_cast<std::size_t>(info->width) *
          static_cast<std::size_t>(info->height) * 4U;
        if (premultiplyScratch_.capacity() > 4U * 1024U * 1024U &&
            required * 4U < premultiplyScratch_.capacity()) {
          std::vector<std::uint8_t>().swap(premultiplyScratch_);
        }
        identity = !premultiplyRgba(
          pixels, info->width, info->height, info->width,
          &premultiplyScratch_);
        if (identity) {
          clearPremultipliedTexture(slot);
          slot.premultipliedTexture = slot.texture;
        } else {
          while (glGetError() != GL_NO_ERROR) {}
          glBindTexture(GL_TEXTURE_2D, slot.premultipliedTexture);
          glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
          glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, info->width, info->height,
                          GL_RGBA, GL_UNSIGNED_BYTE,
                          premultiplyScratch_.data());
          if (glGetError() == GL_NO_ERROR) {
            textureUploadBytes_ += uploadBytes;
          } else {
            clearPremultipliedTexture(slot);
          }
        }
      } else {
        identity = !premultiplyRgba(
          pixels, info->width, info->height, info->width, nullptr);
        if (slot.premultipliedTexture == slot.texture && !identity) {
          slot.premultipliedTexture = 0;
        } else if (identity) {
          slot.premultipliedTexture = slot.texture;
        }
      }
      slot.premultiplyIdentity = identity;
    }
    ++textureFullUpdates_;
    textureUploadBytes_ += uploadBytes;
  }
  trimPremultiplyScratch();
  return ok;
}

bool ImageStore::updateRgbaRegion(ImageHandle handle, int x, int y, int width,
                                  int height, const void* pixels,
                                  int sourceRowPixels) {
  const auto info = lookup(handle);
  if (!info || !pixels || x < 0 || y < 0 || width <= 0 || height <= 0 ||
      x + width > info->width || y + height > info->height ||
      sourceRowPixels < width) return false;
  while (glGetError() != GL_NO_ERROR) {}
  glBindTexture(GL_TEXTURE_2D, info->texture);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, sourceRowPixels);
  glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, GL_RGBA,
                  GL_UNSIGNED_BYTE, pixels);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  const bool ok = glGetError() == GL_NO_ERROR;
  if (ok) {
    auto& slot = slots_[(handle & indexMask) - 1U];
    const std::size_t uploadBytes = static_cast<std::size_t>(width) *
      static_cast<std::size_t>(height) * 4U;
    if (slot.cachedPixels &&
        slot.cachedPixels->width == info->width &&
        slot.cachedPixels->height == info->height) {
      const auto* source = static_cast<const std::uint8_t*>(pixels);
      const std::size_t sourceStride =
        static_cast<std::size_t>(sourceRowPixels) * 4U;
      const std::size_t rowBytes = static_cast<std::size_t>(width) * 4U;
      for (int row = 0; row < height; ++row) {
        std::memcpy(
          slot.cachedPixels->rgba.data() +
            (static_cast<std::size_t>(y + row) * info->width + x) * 4U,
          source + static_cast<std::size_t>(row) * sourceStride,
          rowBytes);
      }
    }
    if (!slot.premultiplied) {
      const bool wholeImage = x == 0 && y == 0 &&
        width == info->width && height == info->height;
      bool regionIdentity = false;
      if (slot.premultipliedTexture &&
          slot.premultipliedTexture != slot.texture) {
        const std::size_t required = static_cast<std::size_t>(width) *
          static_cast<std::size_t>(height) * 4U;
        if (premultiplyScratch_.capacity() > 4U * 1024U * 1024U &&
            required * 4U < premultiplyScratch_.capacity()) {
          std::vector<std::uint8_t>().swap(premultiplyScratch_);
        }
        regionIdentity = !premultiplyRgba(
          pixels, width, height, sourceRowPixels, &premultiplyScratch_);
        if (wholeImage && regionIdentity) {
          clearPremultipliedTexture(slot);
          slot.premultipliedTexture = slot.texture;
        } else {
          while (glGetError() != GL_NO_ERROR) {}
          glBindTexture(GL_TEXTURE_2D, slot.premultipliedTexture);
          glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
          glPixelStorei(GL_UNPACK_ROW_LENGTH, width);
          glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, GL_RGBA,
                          GL_UNSIGNED_BYTE, premultiplyScratch_.data());
          glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
          if (glGetError() == GL_NO_ERROR) {
            textureUploadBytes_ += uploadBytes;
          } else {
            clearPremultipliedTexture(slot);
          }
        }
      } else {
        regionIdentity = !premultiplyRgba(
          pixels, width, height, sourceRowPixels, nullptr);
        if (slot.premultipliedTexture == slot.texture && !regionIdentity) {
          slot.premultipliedTexture = 0;
        } else if (wholeImage && regionIdentity) {
          slot.premultipliedTexture = slot.texture;
        }
      }
      slot.premultiplyIdentity = wholeImage
        ? regionIdentity
        : slot.premultiplyIdentity && regionIdentity;
    }
    ++textureRegionUpdates_;
    textureUploadBytes_ += uploadBytes;
  }
  trimPremultiplyScratch();
  return ok;
}

ImageHandle ImageStore::makeHandle(std::size_t index, std::uint16_t generation) {
  return (static_cast<std::uint32_t>(generation & generationMask) << 16U) |
         static_cast<std::uint32_t>(index + 1U);
}

std::optional<ImageInfo> ImageStore::lookup(ImageHandle handle) const {
  if ((handle & canvasHandleTag) != 0) return std::nullopt;
  const std::uint32_t encodedIndex = handle & indexMask;
  if (encodedIndex == 0) return std::nullopt;
  const std::size_t index = encodedIndex - 1U;
  const auto generation = static_cast<std::uint16_t>(handle >> 16U);
  if (index >= slots_.size()) return std::nullopt;
  const auto& slot = slots_[index];
  if (!slot.live || slot.generation != generation) return std::nullopt;
  return ImageInfo{handle, slot.width, slot.height, slot.texture, slot.premultiplied};
}

bool ImageStore::isRenderTarget(ImageHandle handle) const {
  if (!lookup(handle)) return false;
  return slots_[(handle & indexMask) - 1U].renderTarget;
}

std::optional<ImageInfo> ImageStore::lookupPremultiplied(ImageHandle handle) {
  auto info = lookup(handle);
  if (!info || info->premultiplied) return info;
  auto& slot = slots_[(handle & indexMask) - 1U];
  // Render targets are already framebuffer pixels, not decoded straight images.
  if (slot.gpuOnly) return info;
  if (slot.premultiplyIdentity) {
    slot.premultipliedTexture = slot.texture;
  }
  if (!slot.premultipliedTexture) {
    std::optional<ImagePixels> readback;
    const std::uint8_t* uploadPixels = nullptr;
    std::size_t uploadBytes = 0;
    bool changed = false;
    if (slot.cachedPixels &&
        slot.cachedPixels->width == info->width &&
        slot.cachedPixels->height == info->height) {
      const std::size_t required = static_cast<std::size_t>(info->width) *
        static_cast<std::size_t>(info->height) * 4U;
      if (premultiplyScratch_.capacity() > 4U * 1024U * 1024U &&
          required * 4U < premultiplyScratch_.capacity()) {
        std::vector<std::uint8_t>().swap(premultiplyScratch_);
      }
      changed = premultiplyRgba(slot.cachedPixels->rgba.data(),
        info->width, info->height, info->width, &premultiplyScratch_);
      uploadPixels = premultiplyScratch_.data();
      uploadBytes = premultiplyScratch_.size();
    } else {
      readback = readTexturePixels(*info);
      if (!readback) return std::nullopt;
      changed = premultiplyRgba(readback->rgba.data(), info->width,
        info->height, info->width, &premultiplyScratch_);
      uploadPixels = premultiplyScratch_.data();
      uploadBytes = premultiplyScratch_.size();
    }
    slot.premultiplyIdentity = !changed;
    if (!changed) {
      slot.premultipliedTexture = slot.texture;
    } else {
      while (glGetError() != GL_NO_ERROR) {}
      GLuint texture = 0;
      glGenTextures(1, &texture);
      glBindTexture(GL_TEXTURE_2D, texture);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, info->width, info->height, 0,
        GL_RGBA, GL_UNSIGNED_BYTE, uploadPixels);
      if (!texture || glGetError() != GL_NO_ERROR) {
        if (texture) glDeleteTextures(1, &texture);
        return std::nullopt;
      }
      slot.premultipliedTexture = texture;
      ++textureCreates_;
      textureUploadBytes_ += uploadBytes;
      gpuBytes_ += uploadBytes;
      peakGpuBytes_ = std::max(peakGpuBytes_, gpuBytes_);
    }
  }
  info->texture = slot.premultipliedTexture;
  info->premultiplied = true;
  trimPremultiplyScratch();
  return info;
}

void ImageStore::clearPremultipliedTexture(Slot& slot) {
  if (slot.premultipliedTexture && slot.premultipliedTexture != slot.texture) {
    glDeleteTextures(1, &slot.premultipliedTexture);
    gpuBytes_ -= static_cast<std::size_t>(slot.width) * slot.height * 4U;
  }
  slot.premultipliedTexture = 0;
}

void ImageStore::trimPremultiplyScratch() {
  if (premultiplyScratch_.capacity() <= premultiplyScratchRetainBytes_) return;
  std::vector<std::uint8_t>().swap(premultiplyScratch_);
}

bool ImageStore::retain(ImageHandle handle) {
  if (!lookup(handle)) return false;
  const std::size_t index = (handle & indexMask) - 1U;
  ++slots_[index].references;
  markUsed(slots_[index]);
  return true;
}

void ImageStore::markUsed(Slot& slot) {
  slot.lastUsedSerial = ++useSerial_;
}

bool ImageStore::pin(ImageHandle handle) {
  if (!lookup(handle)) return false;
  auto& slot = slots_[(handle & indexMask) - 1U];
  ++slot.pins;
  markUsed(slot);
  return true;
}

bool ImageStore::unpin(ImageHandle handle) {
  if (!lookup(handle)) return false;
  const std::size_t index = (handle & indexMask) - 1U;
  auto& slot = slots_[index];
  if (slot.pins == 0) return false;
  --slot.pins;
  if (slot.pins == 0 && slot.references == 0 &&
      slot.inFlight.load(std::memory_order_acquire) == 0) {
    if (slot.cacheKey.empty()) destroySlot(index);
    else warmBudgetDirty_ = true;
  }
  return true;
}

bool ImageStore::touch(ImageHandle handle) {
  if (!lookup(handle)) return false;
  markUsed(slots_[(handle & indexMask) - 1U]);
  return true;
}

void ImageStore::destroySlot(std::size_t index) {
  auto& slot = slots_[index];
  clearPremultipliedTexture(slot);
  if (!slot.cacheKey.empty()) pathCache_.erase(slot.cacheKey);
  glDeleteTextures(1, &slot.texture);
  gpuBytes_ -= static_cast<std::size_t>(slot.width) *
               static_cast<std::size_t>(slot.height) * 4U;
  slot.texture = 0;
  slot.width = 0;
  slot.height = 0;
  slot.references = 0;
  slot.inFlight.store(0, std::memory_order_relaxed);
  slot.pins = 0;
  slot.lastUsedSerial = 0;
  slot.cpuPixelFrames = 0;
  slot.retainCpuPixels = false;
  slot.cacheKey.clear();
  slot.sourcePath.clear();
  if (slot.cachedPixels) cpuBytes_ -= slot.cachedPixels->rgba.capacity();
  slot.cachedPixels.reset();
  slot.live = false;
  slot.generation = static_cast<std::uint16_t>((slot.generation + 1U) & generationMask);
  if (slot.generation == 0) slot.generation = 1;
  freeSlots_.push_back(index);
  --liveCount_;
}

bool ImageStore::release(ImageHandle handle) {
  const auto info = lookup(handle);
  if (!info) return false;
  const std::size_t index = (handle & indexMask) - 1U;
  auto& slot = slots_[index];
  if (slot.references == 0) return false;
  --slot.references;
  if (slot.references != 0) return true;
  markUsed(slot);
  // Anonymous RGBA surfaces cannot be reacquired by path, but explicit pins
  // may still extend their lifetime.
  if (slot.pins == 0 &&
      slot.inFlight.load(std::memory_order_acquire) == 0) {
    if (slot.cacheKey.empty()) destroySlot(index);
    else warmBudgetDirty_ = true;
  }
  return true;
}

bool ImageStore::beginUse(ImageHandle handle) {
  const auto info = lookup(handle);
  return info && beginUse(*info);
}

bool ImageStore::beginUse(const ImageInfo& knownInfo) {
  const ImageHandle handle = knownInfo.handle;
  if ((handle & canvasHandleTag) != 0) return false;
  const std::uint32_t encodedIndex = handle & indexMask;
  if (encodedIndex == 0) return false;
  const std::size_t index = encodedIndex - 1U;
  if (index >= slots_.size()) return false;
  auto& slot = slots_[index];
  const auto generation = static_cast<std::uint16_t>(handle >> 16U);
  if (!slot.live || slot.generation != generation ||
      slot.texture != knownInfo.texture ||
      slot.width != knownInfo.width || slot.height != knownInfo.height) {
    return false;
  }
  slot.inFlight.fetch_add(1, std::memory_order_acq_rel);
  return true;
}

bool ImageStore::endUse(ImageHandle handle) {
  if (!lookup(handle)) return false;
  const std::size_t index = (handle & indexMask) - 1U;
  auto& slot = slots_[index];
  const auto previous = slot.inFlight.fetch_sub(1, std::memory_order_acq_rel);
  if (previous == 0) {
    slot.inFlight.store(0, std::memory_order_release);
    return false;
  }
  if (previous == 1 && slot.references == 0 && slot.pins == 0) {
    if (slot.cacheKey.empty()) destroySlot(index);
    else warmBudgetDirty_ = true;
  }
  return true;
}

void ImageStore::update() {
  // beginFrame() calls this every frame. Most frames have no transient CPU
  // pixels to age and no newly-warm image that can exceed the cache budget.
  if (!transientCpuPixelsActive_ && !warmBudgetDirty_) return;

  std::size_t currentWarmBytes = 0;
  bool transientStillActive = false;
  for (std::size_t index = 0; index < slots_.size(); ++index) {
    auto& slot = slots_[index];
    if (!slot.live) continue;
    // Explicitly retained slots keep CPU pixels for the life of the texture;
    // on-demand buffers age out here.
    if (slot.cachedPixels && !slot.retainCpuPixels) {
      if (slot.cpuPixelFrames > 0) {
        --slot.cpuPixelFrames;
        transientStillActive = true;
      } else {
        cpuBytes_ -= slot.cachedPixels->rgba.capacity();
        slot.cachedPixels.reset();
      }
    }
    if (slot.references == 0 && slot.pins == 0 && !slot.cacheKey.empty() &&
        slot.inFlight.load(std::memory_order_acquire) == 0) {
      currentWarmBytes += residentBytes(slot);
    }
  }
  transientCpuPixelsActive_ = transientStillActive;
  warmBudgetDirty_ = false;
  if (currentWarmBytes <= warmBudgetBytes_) return;

  std::vector<std::pair<std::uint64_t, std::size_t>> warmEntries;
  warmEntries.reserve(liveCount_);
  for (std::size_t index = 0; index < slots_.size(); ++index) {
    const auto& slot = slots_[index];
    if (slot.live && slot.references == 0 && slot.pins == 0 &&
        !slot.cacheKey.empty() &&
        slot.inFlight.load(std::memory_order_acquire) == 0) {
      warmEntries.emplace_back(slot.lastUsedSerial, index);
    }
  }
  std::sort(warmEntries.begin(), warmEntries.end());

  // A straight-alpha image can have a second full-size premultiplied texture
  // for Pixi-compatible linear sampling. Once the image is warm (no owners),
  // that duplicate is purely a cache: discard it before evicting the source
  // texture itself. Reacquisition can recreate it exactly, while a low-memory
  // device may keep twice as many source images resident within the same
  // budget.
  for (const auto& entry : warmEntries) {
    if (currentWarmBytes <= warmBudgetBytes_) break;
    auto& slot = slots_[entry.second];
    if (!slot.live || slot.references != 0 || slot.pins != 0 ||
        slot.cacheKey.empty() ||
        slot.inFlight.load(std::memory_order_acquire) != 0 ||
        !slot.premultipliedTexture ||
        slot.premultipliedTexture == slot.texture) {
      continue;
    }
    const std::size_t duplicateBytes =
      static_cast<std::size_t>(slot.width) *
      static_cast<std::size_t>(slot.height) * 4U;
    clearPremultipliedTexture(slot);
    currentWarmBytes -= std::min(currentWarmBytes, duplicateBytes);
  }

  for (const auto& entry : warmEntries) {
    if (currentWarmBytes <= warmBudgetBytes_) break;
    const std::size_t index = entry.second;
    auto& slot = slots_[index];
    if (!slot.live || slot.references != 0 || slot.pins != 0 ||
        slot.cacheKey.empty() ||
        slot.inFlight.load(std::memory_order_acquire) != 0) continue;
    currentWarmBytes -= residentBytes(slot);
    destroySlot(index);
    ++budgetEvictions_;
  }
}

}  // namespace pmjs
