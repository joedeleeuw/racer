#include <SDL.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace {

constexpr int device_rate = 48000;
constexpr int channels = 2;
constexpr size_t bytes_per_frame = channels * sizeof(int16_t);

std::mutex audio_mutex;
SDL_AudioDeviceID audio_device = 0;
SDL_AudioStream* audio_stream = nullptr;
uint32_t guest_rate = 0;
std::vector<int16_t> swapped;
std::vector<uint8_t> converted;

[[noreturn]] void audio_fail(const char* what) {
    fprintf(stderr, "[audio] %s: %s\n", what, SDL_GetError());
    std::exit(1);
}

void open_device() {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        audio_fail("SDL audio subsystem unavailable");
    }
    SDL_AudioSpec want{};
    want.freq = device_rate;
    want.format = AUDIO_S16SYS;
    want.channels = channels;
    want.samples = 1024;
    audio_device = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
    if (audio_device == 0) {
        audio_fail("cannot open SDL audio device");
    }
    SDL_PauseAudioDevice(audio_device, 0);
    printf("[audio] SDL device on driver %s at %d Hz\n", SDL_GetCurrentAudioDriver(), device_rate);
}

}

void swe1r_set_frequency(uint32_t freq) {
    std::lock_guard lock{ audio_mutex };
    if (audio_device == 0) {
        open_device();
    }
    if (audio_stream != nullptr) {
        SDL_FreeAudioStream(audio_stream);
    }
    audio_stream = SDL_NewAudioStream(AUDIO_S16SYS, channels, freq, AUDIO_S16SYS, channels, device_rate);
    if (audio_stream == nullptr) {
        audio_fail("cannot create SDL audio stream");
    }
    guest_rate = freq;
    printf("[audio] guest rate %u Hz\n", freq);
}

void swe1r_queue_samples(int16_t* samples, size_t num_samples) {
    std::lock_guard lock{ audio_mutex };
    swapped.resize(num_samples);
    for (size_t i = 0; i + 1 < num_samples; i += 2) {
        swapped[i] = samples[i + 1];
        swapped[i + 1] = samples[i];
    }
    if (SDL_AudioStreamPut(audio_stream, swapped.data(), static_cast<int>(num_samples * sizeof(int16_t))) != 0) {
        audio_fail("SDL audio stream rejected guest samples");
    }
    converted.resize(SDL_AudioStreamAvailable(audio_stream));
    const int got = SDL_AudioStreamGet(audio_stream, converted.data(), static_cast<int>(converted.size()));
    if (got < 0) {
        audio_fail("SDL audio stream conversion failed");
    }
    if (got > 0 && SDL_QueueAudio(audio_device, converted.data(), static_cast<Uint32>(got)) != 0) {
        audio_fail("SDL audio device rejected samples");
    }
}

size_t swe1r_get_frames_remaining() {
    std::lock_guard lock{ audio_mutex };
    const size_t queued = SDL_GetQueuedAudioSize(audio_device) + SDL_AudioStreamAvailable(audio_stream);
    return queued / bytes_per_frame * guest_rate / device_rate;
}
