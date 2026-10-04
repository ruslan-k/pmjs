#pragma once

// Shared sample math for the real-time callback and deterministic unit tests.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include "media_decoder.hpp"

namespace pmjs {

struct PreparedAudioAsset : DecodedAudio {
  double sourceDuration = 0;
  int sourceChannels = 0;
};

// Streaming audio is consumed from the front on the real-time callback and
// appended by the decoder worker. std::deque makes those two-sample pops cheap
// asymptotically, but still pays segmented-storage and iterator overhead tens of
// thousands of times per second. Keep one contiguous allocation and advance a
// logical head instead.
class StreamSampleBuffer {
 public:
  StreamSampleBuffer() = default;
  StreamSampleBuffer(std::initializer_list<float> values) { assign(values.begin(), values.end()); }

  std::size_t size() const { return size_; }
  std::size_t capacity() const { return storage_.size(); }
  bool empty() const { return size_ == 0; }

  float& operator[](std::size_t index) {
    return storage_[physicalIndex(index)];
  }
  const float& operator[](std::size_t index) const {
    return storage_[physicalIndex(index)];
  }

  void reserve(std::size_t requested) {
    if (requested <= storage_.size()) return;
    std::vector<float> replacement(requested);
    for (std::size_t index = 0; index < size_; ++index) {
      replacement[index] = (*this)[index];
    }
    storage_.swap(replacement);
    head_ = 0;
  }

  void clear() {
    head_ = 0;
    size_ = 0;
  }

  void push_back(float value) {
    ensureCapacity(size_ + 1);
    storage_[(head_ + size_) % storage_.size()] = value;
    ++size_;
  }

  void append(const float* values, std::size_t count) {
    if (count == 0) return;
    ensureCapacity(size_ + count);
    const std::size_t capacity = storage_.size();
    std::size_t tail = head_ + size_;
    if (tail >= capacity) tail -= capacity;
    const std::size_t first = std::min(count, capacity - tail);
    std::copy_n(values, first, storage_.data() + tail);
    if (first < count) {
      std::copy_n(values + first, count - first, storage_.data());
    }
    size_ += count;
  }

  template <typename Iterator>
  void assign(Iterator first, Iterator last) {
    clear();
    for (; first != last; ++first) push_back(*first);
  }

  StreamSampleBuffer& operator=(std::initializer_list<float> values) {
    assign(values.begin(), values.end());
    return *this;
  }

  void pop_front(std::size_t count = 1) {
    count = std::min(count, size_);
    if (count == size_) {
      clear();
      return;
    }
    head_ += count;
    if (head_ >= storage_.size()) head_ -= storage_.size();
    size_ -= count;
  }

  // Read the current and next interleaved stereo frames with one wrap test.
  // mixVoiceInto() calls this once per output frame instead of mapping four
  // logical indices through the ring separately.
  void frontStereoPair(float& left, float& right,
                       float& nextLeft, float& nextRight) const {
    left = storage_[head_];
    right = storage_[head_ + 1U < storage_.size() ? head_ + 1U : 0U];
    std::size_t next = head_ + 2U;
    if (next >= storage_.size()) next -= storage_.size();
    nextLeft = storage_[next];
    nextRight = storage_[next + 1U < storage_.size() ? next + 1U : 0U];
  }

 private:
  std::size_t physicalIndex(std::size_t logicalIndex) const {
    std::size_t index = head_ + logicalIndex;
    if (index >= storage_.size()) index -= storage_.size();
    return index;
  }

  void ensureCapacity(std::size_t required) {
    if (required <= storage_.size()) return;
    const std::size_t doubled = storage_.empty() ? 16 : storage_.size() * 2;
    reserve(std::max(required, doubled));
  }

  std::vector<float> storage_;
  std::size_t head_ = 0;
  std::size_t size_ = 0;
};

struct VoiceMixState {
  std::shared_ptr<const PreparedAudioAsset> asset;
  StreamSampleBuffer samples;  // interleaved stereo frames
  double phase = 0;
  std::uint64_t positionFrame = 0;
  float volume = 1.0F, pitch = 1.0F, pan = 0.0F;
  float leftGain = 1.0F, rightGain = 1.0F;
  float gain = 1.0F, targetGain = 1.0F, gainStep = 0.0F;
  bool stopAfterFade = false, playing = false, loop = false, eof = false;
  bool suspended = false;
  std::uint64_t loopStart = 0, loopEnd = 0;
  double duration = 0;
};

inline void mixVoiceInto(VoiceMixState& voice, float* output, int frames,
                         float master) {
  if (!voice.playing || voice.suspended) return;
  const float leftPanGain =
    voice.leftGain * (voice.pan > 0 ? 1 - voice.pan : 1);
  const float rightPanGain =
    voice.rightGain * (voice.pan < 0 ? 1 + voice.pan : 1);
  for (int frame = 0; frame < frames; ++frame) {
    std::uint64_t nextSampleFrame = voice.positionFrame + 1;
    if (voice.asset) {
      const auto length = voice.asset->samples.size() / 2;
      const auto end = voice.loop && voice.loopEnd > voice.loopStart
        ? voice.loopEnd : length;
      if (voice.loop && nextSampleFrame >= end) nextSampleFrame = voice.loopStart;
      if (voice.positionFrame >= length || nextSampleFrame >= length) {
        voice.playing = false;
        break;
      }
    } else if (voice.samples.size() < 4) {
      if (voice.eof) voice.playing = false;
      break;
    }
    if (voice.gainStep != 0) {
      const float next = voice.gain + voice.gainStep;
      if ((voice.gainStep > 0 && next >= voice.targetGain) ||
          (voice.gainStep < 0 && next <= voice.targetGain)) {
        voice.gain = voice.targetGain;
        voice.gainStep = 0;
        if (voice.stopAfterFade && voice.gain <= 0) {
          voice.playing = false;
          break;
        }
      } else {
        voice.gain = next;
      }
    }
    const float fraction = static_cast<float>(voice.phase);
    float currentLeft, currentRight, nextLeft, nextRight;
    if (voice.asset) {
      const auto currentOffset = voice.positionFrame * 2;
      const auto nextOffset = nextSampleFrame * 2;
      currentLeft = voice.asset->samples[currentOffset];
      currentRight = voice.asset->samples[currentOffset + 1];
      nextLeft = voice.asset->samples[nextOffset];
      nextRight = voice.asset->samples[nextOffset + 1];
    } else {
      voice.samples.frontStereoPair(
        currentLeft, currentRight, nextLeft, nextRight);
    }
    const float left =
      currentLeft * (1 - fraction) + nextLeft * fraction;
    const float right =
      currentRight * (1 - fraction) + nextRight * fraction;
    const float leftGain = voice.volume * voice.gain * leftPanGain;
    const float rightGain = voice.volume * voice.gain * rightPanGain;
    output[frame * 2] += left * leftGain * master;
    output[frame * 2 + 1] += right * rightGain * master;
    voice.phase += voice.pitch;
    while (voice.phase >= 1 && (voice.asset
        ? (voice.loop || voice.positionFrame < voice.asset->samples.size() / 2)
        : voice.samples.size() >= 2)) {
      if (!voice.asset) {
        voice.samples.pop_front(2);
      }
      voice.phase -= 1;
      ++voice.positionFrame;
      if (voice.asset && voice.loop && voice.positionFrame >=
          (voice.loopEnd > voice.loopStart ? voice.loopEnd : voice.asset->samples.size() / 2))
        voice.positionFrame = voice.loopStart;
      else if (voice.loop && voice.loopEnd > voice.loopStart &&
          voice.positionFrame >= voice.loopEnd)
        voice.positionFrame = voice.loopStart;
      else if (voice.loop && voice.duration > 0 &&
               voice.positionFrame >= (voice.asset ? voice.asset->samples.size() / 2
                 : static_cast<std::uint64_t>(voice.duration * 48000)))
        voice.positionFrame = 0;
    }
  }
}

inline void clampStereoMix(float* output, int frames) {
  for (int index = 0; index < frames * 2; ++index)
    output[index] = std::clamp(output[index], -1.0F, 1.0F);
}

}  // namespace pmjs
