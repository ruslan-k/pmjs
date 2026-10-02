#include "media_service.hpp"

#include <SDL.h>
#include <sys/resource.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

double cpuMs() {
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  return usage.ru_utime.tv_sec * 1000.0 + usage.ru_utime.tv_usec / 1000.0 +
         usage.ru_stime.tv_sec * 1000.0 + usage.ru_stime.tv_usec / 1000.0;
}

std::vector<std::uint8_t> wav(int frames) {
  std::vector<std::uint8_t> bytes(44 + frames * 4);
  auto word = [&](int offset, std::uint32_t value, int count) {
    for (int i = 0; i < count; ++i) bytes[offset + i] =
      static_cast<std::uint8_t>(value >> (i * 8));
  };
  auto text = [&](int offset, const char* value) {
    for (int i = 0; i < 4; ++i) bytes[offset + i] =
      static_cast<std::uint8_t>(value[i]);
  };
  text(0, "RIFF"); word(4, static_cast<std::uint32_t>(bytes.size() - 8), 4);
  text(8, "WAVE"); text(12, "fmt "); word(16, 16, 4); word(20, 1, 2);
  word(22, 2, 2); word(24, 48000, 4); word(28, 192000, 4);
  word(32, 4, 2); word(34, 16, 2); text(36, "data");
  word(40, static_cast<std::uint32_t>(frames * 4), 4);
  for (int frame = 0; frame < frames; ++frame) {
    const std::int16_t value = static_cast<std::int16_t>(
      ((frame % 400) - 200) * 80);
    word(44 + frame * 4, static_cast<std::uint16_t>(value), 2);
    word(46 + frame * 4, static_cast<std::uint16_t>(value), 2);
  }
  return bytes;
}

void writeFile(const std::filesystem::path& path,
               const std::vector<std::uint8_t>& bytes) {
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
}

double measureCpu(int milliseconds) {
  const double started = cpuMs();
  std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
  return cpuMs() - started;
}

}  // namespace

int main() {
  SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
  SDL_setenv("PMJS_AUDIO_DIAGNOSTICS", "0", 1);
  if (SDL_Init(SDL_INIT_AUDIO) != 0) {
    std::cerr << SDL_GetError() << '\n';
    return 2;
  }

  const auto root = std::filesystem::temp_directory_path() /
    ("pmjs-audio-perf-" + std::to_string(SDL_GetPerformanceCounter()));
  std::filesystem::create_directories(root);
  writeFile(root / "stream.wav", wav(48000 * 8));

  try {
    pmjs::MediaService media(root);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    const double idleCpu = measureCpu(1500);

    std::string error;
    const auto stream = media.loadAudio("stream.wav", &error,
      {pmjs::AudioIntent::music, {}, {}});
    if (!stream || !media.play(stream, true, 0)) {
      std::cerr << "cannot start stream: " << error << '\n';
      return 3;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const double streamingCpu = measureCpu(1500);
    const auto buffered = media.bufferedFrames(stream);
    media.stop(stream);
    media.release(stream);

    std::cout << "{\"benchmark\":\"audio-worker\","
              << "\"idle_cpu_ms\":" << idleCpu << ","
              << "\"streaming_cpu_ms\":" << streamingCpu << ","
              << "\"buffered_frames\":" << buffered << "}\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    std::filesystem::remove_all(root);
    SDL_Quit();
    return 1;
  }

  std::filesystem::remove_all(root);
  SDL_Quit();
  return 0;
}
