#include <array>
#include <algorithm>
#include <string_view>
#include <iostream>
#include <cassert>
#include <memory>
#include <SDL.h>
#include <cstring>
#include "SDL_mixer.h"

using sample_t = int32_t;
static constexpr auto bars = 600U;
static std::array<sample_t, bars> volume_graph;
static Mix_Chunk* original_wave;
static bool looping = false;

template<typename T>
static T map(long x, long in_min, long in_max, long out_min, long out_max) {
    return static_cast<T>((x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min);
};

static int analyze_sample(Mix_Chunk *wave) {
    Uint16 format;
    int channels, incr;
    Uint8 *end = wave->abuf + wave->alen;
    int frequency{};

    Mix_QuerySpec(&frequency, &format, &channels);
    incr = (format & 0xFF) * channels;

    std::advance(end, -incr);

    assert(channels == 1);
    assert(incr == 32);

    const auto sample_frames = static_cast<double>(wave->alen / sizeof(sample_t));
    const auto samples_in_bar = sample_frames / bars;
    sample_t bar_value = (*((sample_t *) wave->abuf));
    int sample_count = 0;
    std::size_t bar_num = 0;
    for (auto *current = (sample_t *) wave->abuf;
         current != (sample_t *)end;
         std::advance(current, 1), ++sample_count) {

        bar_value = static_cast<sample_t>(std::lerp((*current), bar_value, 0.5));

        if (sample_count == static_cast<int>(samples_in_bar) && bar_num < bars) {
            volume_graph.at(bar_num++) = bar_value;
            sample_count = 0;
        }
    }

    return static_cast<int>(std::ceil((sample_frames * 1000) / frequency));
}

static void flip_sample(Mix_Chunk *wave)
{
    Uint16 format;
    int channels, incr;
    Uint8 *start = wave->abuf;
    Uint8 *end = wave->abuf + wave->alen;

    Mix_QuerySpec(NULL, &format, &channels);
    incr = (format & 0xFF) * channels;

    end -= incr;

    assert(channels == 1);
    assert(incr == 32);

    for (int i = wave->alen / 2; i >= 0; i -= 4) {
        std::swap(*((uint32_t *) start), *((uint32_t *) end));
        start += 4;
        end -= 4;
    }
}

static SDL_Rect span;
static bool is_selecting;

static bool handle_input() {
    SDL_Event event;

    while(SDL_PollEvent( &event ) != 0) {
        switch (event.type)
        {
            case SDL_QUIT:
                return true;
            case SDL_MOUSEBUTTONDOWN:
                is_selecting = true;
                break;
            case SDL_MOUSEBUTTONUP:
                is_selecting = false;
                break;
            case SDL_KEYDOWN:
                switch( event.key.keysym.sym ) {
                    case SDLK_ESCAPE:
                    case SDLK_q:
                        return true;
                    default:
                    break;
                }
                break;
            default:
                break;
        }
    }

    return false;
}

static SDL_Window *window;
static SDL_Renderer *renderer;
static constexpr auto window_width = 800; 
static constexpr auto window_height = 600; 
static constexpr auto FPS = 144;
static constexpr auto frametime = 1000.0 / FPS;

constexpr int texture_width = window_width;
constexpr int texture_height = 150;
SDL_Texture* texture = SDL_CreateTexture( renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, texture_width, texture_height );
static std::array<uint8_t, texture_width * texture_height * 4> pixels;

constexpr auto visualizer_w = window_width - 200;
constexpr auto visualizer_h = 100;

auto visualizer = SDL_Rect({
    .x = 100,
    .y = window_height / 4,
    .w = visualizer_w,
    .h = visualizer_h
});

static int sample_length_ms;
static SDL_Rect needle = {
    .x = visualizer.x + 4,
    .y = visualizer.y + 1,
    .w = 2,
    .h = visualizer_h - 2
};

std::pair<int, int> needle_area{visualizer.x, visualizer.x + visualizer_w - 4};
int needle_pos{}; // relative
bool select_handled = false;

void calc_new_x(const auto &mouse_pos) {
    static bool reverse = false;

    if (SDL_PointInRect(&mouse_pos, &visualizer)) {
        needle.x = mouse_pos.x;
        return;
    }

    needle_pos += ((visualizer_w * 100) / sample_length_ms) * (reverse && looping ? -1 : 1);
    needle.x = static_cast<int>(std::round(needle_area.first + (needle_pos * frametime) / 100));

    if (needle.x > needle_area.second || needle.x < needle_area.first) {
        if (Mix_Playing(0)) { // fixme: this is going to be inaccurate
            if (looping) {
                reverse = !reverse;
                return;
            }

            needle_pos = 0;
            needle.x = needle_area.first;
        } else {
            needle.x = needle_area.second;
        }
    }
}

void render_needle() {
    SDL_Point mouse_pos;
    SDL_GetMouseState(&mouse_pos.x, &mouse_pos.y);

    if (SDL_PointInRect(&mouse_pos, &visualizer)) {
        if (is_selecting && !select_handled) {
            span.x = std::clamp(mouse_pos.x, visualizer.x, visualizer.x + visualizer.w);
            span.y = needle.y;
            span.w = 0;
            span.h = needle.h;
            select_handled = true;
            needle_area = {visualizer.x, visualizer.x + visualizer_w - 4};
            looping = false;
        } else if (!is_selecting && select_handled) {
            auto span_w = mouse_pos.x - span.x;
            span.w = span_w;
            
            const auto span_start_pos = span.w > 0 ? span.x : span.x + span.w;

            needle_pos = 0;
            needle_area.first = span_start_pos;
            needle_area.second = span.w < 0 ? span_start_pos + (-span.w) : span.x + span.w;
        }
    }
    
    if (span.w != 0) {
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 128);
        SDL_RenderFillRect( renderer, &span );
    }

    SDL_SetRenderDrawColor(renderer, 255, 18, 18, 255);

    calc_new_x(mouse_pos);

    SDL_RenderFillRect( renderer, &needle );
}

// this creates popping sounds
// todo: stitch these together and use the "loop" parameter on Mix_PlayChannel() ?
static Mix_Chunk slice;

auto get_sample_span() {
    const auto span_start = span.w > 0 ? span.x : span.x + span.w;
    const auto span_end = span.w < 0 ? span.x : span.x + span.w;

    // fixme: there has to be a better way
    // get the relative position in the sample, round it to the nearest multiple of sample_t to avoid misalignment
    const auto sample_start = map<std::size_t>(span_start, visualizer.x, visualizer.x + visualizer.w, 0, original_wave->alen) & -sizeof(sample_t);
    const auto sample_end = map<std::size_t>(span_end, visualizer.x, visualizer.x + visualizer.w, 0, original_wave->alen) & -sizeof(sample_t);
    
    assert(sample_start >= 0 && sample_end < original_wave->alen);

    return std::pair{sample_start, sample_end - sample_start};
}

void play_slice() {
    auto create_slice = []() -> Mix_Chunk* {
        // fixme: error-prone naming
        const auto [slice_start, slice_len] = get_sample_span();
        slice.volume = original_wave->volume;
        slice.allocated = 1;

        if (slice.abuf == nullptr || slice_len * 2 > slice.alen) {
            if (slice.abuf != nullptr) {
                delete [] slice.abuf;
            } 
            slice.abuf = new uint8_t[slice_len * 2];
        }

        slice.alen = static_cast<uint32_t>(slice_len * 2);
        std::memcpy(slice.abuf, original_wave->abuf + slice_start, slice_len);

        // fixme: avoid all this by memcpy-ing the slice into both halves of the buffer and flipping only the second half
        std::unique_ptr<uint8_t[]> reversed_buffer(new uint8_t[slice_len]);

        Mix_Chunk reversed_slice {
            .allocated = 1,
            .abuf = reversed_buffer.get(),
            .alen = static_cast<uint32_t>(slice_len),
            .volume = original_wave->volume,
        };

        std::memcpy(reversed_slice.abuf, slice.abuf, slice_len);
        flip_sample(&reversed_slice);
        std::memcpy(slice.abuf + slice_len, reversed_slice.abuf, slice_len);

        return &slice;
    };

    if (span.w == 0) {
        return;
    }

    Mix_PlayChannel(0, create_slice(), -1);
    looping = true;
}

void render_frame() {
    SDL_SetRenderTarget( renderer, texture );

    SDL_SetRenderDrawColor(renderer, 18, 18, 18, 255);
    SDL_RenderClear(renderer);

    SDL_SetRenderDrawColor( renderer, 0x00, 0xFF, 0x00, 0xFF );
    SDL_RenderDrawRect( renderer, &visualizer );

    SDL_SetRenderDrawColor( renderer, 0x00, 0x00, 0xFF, 0xFF );

    // fixme: these values can be saved in analyze_sample()
    const auto [min, max] = std::minmax_element(volume_graph.begin(), volume_graph.end());

    const auto offset = static_cast<double>(visualizer_w) / bars;
    for (std::size_t i = 1; i < bars; ++i) {
        const auto x1 = static_cast<int>(visualizer.x + offset * static_cast<double>(i));
        const auto y1 = visualizer.y + visualizer_h - 2;
        const auto x2 = x1;

        // offset the graph to the bottom of the visualizer 
        auto y2 = map<int>(volume_graph[i] - static_cast<long>(*min), *min, *max, 0, visualizer.h) + visualizer.y - 2;

        if (y2 > y1) {
            // invert negative portion of wave
            y2 = y1 - (y2 - y1);
        }

        SDL_RenderDrawLine(renderer, x1, y1, x2, y2);
    }

    render_needle();

    if (!is_selecting && select_handled) {
        select_handled = false;
        play_slice();
    }

    SDL_SetRenderTarget( renderer, nullptr );
    
    SDL_UpdateTexture( texture, nullptr, pixels.data(), texture_width * 4 );
    SDL_RenderCopy( renderer, texture, nullptr, &visualizer );
    SDL_RenderPresent( renderer );
}

int main() {
    auto audio_rate = 48000;
    auto audio_format = static_cast<uint16_t>(AUDIO_S32);
    auto audio_channels = 1;
    constexpr auto loops = 0;
    constexpr auto reverse_sample = false;

	if (SDL_Init(SDL_INIT_AUDIO) < 0 || SDL_Init( SDL_INIT_VIDEO ) < 0) {
        SDL_Log("Couldn't initialize SDL: %s\n",SDL_GetError());
        return 1;
    }

    if (Mix_OpenAudio(audio_rate, audio_format, audio_channels, 4096) < 0) {
        SDL_Log("Couldn't open audio: %s\n", SDL_GetError());
        return 1;
    } else {
        Mix_QuerySpec(&audio_rate, &audio_format, &audio_channels);
        SDL_Log("Opened audio at %d Hz %d bit%s %s", audio_rate,
            (audio_format&0xFF),
            (SDL_AUDIO_ISFLOAT(audio_format) ? " (float)" : ""),
            (audio_channels > 2) ? "surround" :
            (audio_channels > 1) ? "stereo" : "mono");
        if (loops) {
            SDL_Log(" (looping)\n");
        } else {
            putchar('\n');
        }
    }

    window = SDL_CreateWindow(
        "Looper",
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        800,
        600,
        SDL_WINDOW_SHOWN);

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    std::string_view filename = "test.wav";
    original_wave = Mix_LoadWAV(filename.data());

    if (original_wave == nullptr) {
        SDL_Log("Couldn't load %s: %s\n", filename.data(), SDL_GetError());
        return 1;
    }

    if (reverse_sample) {
        flip_sample(original_wave);
    }

    Mix_PlayChannel(0, original_wave, loops);

    std::cout << "Playing: " << filename << '\n';
    bool exit = false;

    sample_length_ms = analyze_sample(original_wave);
    while (!exit) {
        render_frame();
        exit = handle_input();
        SDL_Delay(static_cast<int>(frametime));
    }

    std::cout << "Finished.\n";
}
