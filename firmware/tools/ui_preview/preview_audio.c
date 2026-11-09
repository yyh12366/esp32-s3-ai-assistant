#include "preview_audio.h"

#include <pthread.h>
#include <string.h>

#include <SDL.h>

enum { AUDIO_RATE = 16000, AUDIO_QUEUE_MAX = AUDIO_RATE * 2 * 3 / 2, AUDIO_CHUNK_MAX = 4096 };

static SDL_AudioDeviceID s_device;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t s_generation;
static bool s_active;
static bool s_paused;

static bool queue_ready(uint64_t generation, uint64_t active_generation, bool active, bool paused)
{
    return active && !paused && generation == active_generation;
}

bool preview_audio_init(void)
{
    SDL_AudioSpec wanted = {0};
    wanted.freq = AUDIO_RATE;
    wanted.format = AUDIO_S16LSB;
    wanted.channels = 1;
    wanted.samples = 1024;
    s_device = SDL_OpenAudioDevice(NULL, 0, &wanted, NULL, 0);
    if (!s_device) return false;
    SDL_PauseAudioDevice(s_device, 0);
    return true;
}

void preview_audio_shutdown(void)
{
    pthread_mutex_lock(&s_lock);
    if (s_device) {
        SDL_ClearQueuedAudio(s_device);
        SDL_CloseAudioDevice(s_device);
        s_device = 0;
    }
    s_active = false;
    s_paused = false;
    pthread_mutex_unlock(&s_lock);
}

void preview_audio_begin(uint64_t generation)
{
    pthread_mutex_lock(&s_lock);
    if (generation >= s_generation) {
        s_generation = generation;
        s_active = true;
        s_paused = false;
        if (s_device) {
            SDL_ClearQueuedAudio(s_device);
            SDL_PauseAudioDevice(s_device, 0);
        }
    }
    pthread_mutex_unlock(&s_lock);
}

void preview_audio_cancel(uint64_t generation)
{
    pthread_mutex_lock(&s_lock);
    if (generation >= s_generation) {
        s_generation = generation;
        s_active = false;
        s_paused = false;
        if (s_device) {
            SDL_ClearQueuedAudio(s_device);
            SDL_PauseAudioDevice(s_device, 0);
        }
    }
    pthread_mutex_unlock(&s_lock);
}

bool preview_audio_available(void)
{
    return s_device != 0;
}

uint32_t preview_audio_queued_bytes(void)
{
    pthread_mutex_lock(&s_lock);
    const uint32_t queued = s_device ? SDL_GetQueuedAudioSize(s_device) : 0;
    pthread_mutex_unlock(&s_lock);
    return queued;
}

bool preview_audio_set_paused(uint64_t generation, bool paused)
{
    pthread_mutex_lock(&s_lock);
    const bool current = s_active && generation == s_generation && s_device;
    if (current) {
        s_paused = paused;
        SDL_PauseAudioDevice(s_device, paused ? 1 : 0);
    }
    pthread_mutex_unlock(&s_lock);
    return current;
}

static bool queue_bytes(uint64_t generation, const uint8_t *bytes, size_t length)
{
    // Backpressure bounds queued PCM and lets pause or cancel stop the producer promptly.
    for (;;) {
        pthread_mutex_lock(&s_lock);
        if (!s_device || !s_active || generation != s_generation) {
            pthread_mutex_unlock(&s_lock);
            return false;
        }
        if (!queue_ready(generation, s_generation, s_active, s_paused) ||
            SDL_GetQueuedAudioSize(s_device) + length > AUDIO_QUEUE_MAX) {
            pthread_mutex_unlock(&s_lock);
            SDL_Delay(5);
            continue;
        }
        const bool queued = SDL_QueueAudio(s_device, bytes, (Uint32)length) == 0;
        pthread_mutex_unlock(&s_lock);
        return queued;
    }
}

static size_t scale_pcm16le(preview_audio_stream_t *stream, const uint8_t *bytes, size_t length,
                            uint8_t *output, size_t output_size, size_t *consumed)
{
    size_t input = 0;
    size_t used = 0;
    while (used + 2 <= output_size && (stream->has_low_byte || input + 1 < length)) {
        uint8_t low;
        uint8_t high;
        if (stream->has_low_byte) {
            low = stream->low_byte;
            high = bytes[input++];
            stream->has_low_byte = false;
        } else {
            low = bytes[input++];
            high = bytes[input++];
        }
        const int16_t sample = (int16_t)((uint16_t)low | ((uint16_t)high << 8));
        const int16_t scaled = (int16_t)((int32_t)sample / 5);
        output[used++] = (uint8_t)scaled;
        output[used++] = (uint8_t)((uint16_t)scaled >> 8);
    }
    if (input < length && used + 2 > output_size) {
        *consumed = input;
        return used;
    }
    if (input < length) {
        stream->low_byte = bytes[input];
        stream->has_low_byte = true;
        input++;
    }
    *consumed = input;
    return used;
}

bool preview_audio_write_pcm16le(uint64_t generation, preview_audio_stream_t *stream,
                                 const uint8_t *bytes, size_t length)
{
    uint8_t output[AUDIO_CHUNK_MAX];
    size_t input = 0;
    while (input < length) {
        size_t consumed = 0;
        const size_t written = scale_pcm16le(stream, bytes + input, length - input,
                                             output, sizeof(output), &consumed);
        if (written && !queue_bytes(generation, output, written)) return false;
        if (!consumed) return false;
        input += consumed;
    }
    return true;
}

void preview_audio_wait_drained(uint64_t generation)
{
    for (;;) {
        pthread_mutex_lock(&s_lock);
        const bool current = s_active && generation == s_generation && s_device;
        const bool empty = current && SDL_GetQueuedAudioSize(s_device) == 0;
        pthread_mutex_unlock(&s_lock);
        if (!current || empty) return;
        SDL_Delay(10);
    }
}

bool preview_audio_self_check(void)
{
    const uint8_t pcm[] = {0x10, 0x27, 0xF0, 0xD8};
    uint8_t output[4] = {0};
    preview_audio_stream_t stream = {0};
    size_t consumed = 0;
    if (scale_pcm16le(&stream, pcm, 1, output, sizeof(output), &consumed) != 0 ||
        consumed != 1 || !stream.has_low_byte) return false;
    if (scale_pcm16le(&stream, pcm + 1, 3, output, sizeof(output), &consumed) != sizeof(output) ||
        consumed != 3) return false;
    if (stream.has_low_byte || output[0] != 0xD0 || output[1] != 0x07 ||
        output[2] != 0x30 || output[3] != 0xF8) return false;
    return queue_ready(1, 1, true, false) && !queue_ready(1, 1, true, true) &&
           !queue_ready(1, 2, true, false) && !queue_ready(1, 1, false, false);
}
