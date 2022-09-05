#include <array>
#include <algorithm>
#include <string_view>
#include <iostream>
#include <cassert>
#include <SDL.h>
#include "SDL_mixer.h"

using sample_t = int32_t;
static constexpr auto bars = 600;
static std::array<sample_t, bars> volume_graph;

static int analyze_sample(Mix_Chunk *wave) {
    Uint16 format;
    int channels, incr;
    Uint8 *start = wave->abuf;
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
    int bar_num = 0;
    for (auto *current = (sample_t *) wave->abuf;
         current != (sample_t *)end;
         std::advance(current, 1), ++sample_count) {

        bar_value = std::lerp((*current), bar_value, 0.5);

        if (sample_count == static_cast<int>(samples_in_bar) && bar_num < bars) {
            volume_graph[bar_num++] = bar_value;
            sample_count = 0;
        }
    }

    return static_cast<int>((sample_frames * 1000) / frequency);
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

int needle_pos{};
bool select_handled = false;

void calc_new_x(const auto &mouse_pos) {
    if (SDL_PointInRect(&mouse_pos, &visualizer)) {
        needle.x = mouse_pos.x;
        return;
    }

    needle_pos += (visualizer_w * 10) / sample_length_ms;
    needle.x = visualizer.x + (needle_pos / 10) * frametime;

    if ((needle.x) >= visualizer.x + visualizer_w - 4) {
        if (Mix_Playing(0)) { // fixme: this is going to be inaccurate
            needle.x = visualizer.x + 4 * frametime;
            needle_pos = 0;
        } else {
            needle.x = visualizer.x + visualizer_w - 4;
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
        } else if (!is_selecting && select_handled) {
            auto span_w = mouse_pos.x - span.x;
            span.w = span_w;
            select_handled = false;
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

void render_frame() {
    SDL_SetRenderTarget( renderer, texture );

    SDL_SetRenderDrawColor(renderer, 18, 18, 18, 255);
    SDL_RenderClear(renderer);

    SDL_SetRenderDrawColor( renderer, 0x00, 0xFF, 0x00, 0xFF );
    SDL_RenderDrawRect( renderer, &visualizer );

    SDL_SetRenderDrawColor( renderer, 0x00, 0x00, 0xFF, 0xFF );

    auto map = [](long x, long in_min, long in_max, long out_min, long out_max) {
          return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
    };

    const auto [min, max] = std::minmax_element(volume_graph.begin(), volume_graph.end());

    const auto offset = static_cast<double>(visualizer_w) / static_cast<double>(bars);
    for (int i = 1; i < bars; ++i) {
        const auto x1 = static_cast<int>(visualizer.x + offset * i);
        const auto y1 = visualizer.y + visualizer_h - 2;
        const auto x2 = x1;
        auto y2 = map(volume_graph[i] - static_cast<long>(*min), *min, *max, 0, visualizer.h) + visualizer.y - 2;

        if (y2 > y1) {
            y2 = y1 - (y2 - y1);
        }

        SDL_RenderDrawLine(renderer, x1, y1, x2, y2);
    }

    render_needle();

    SDL_SetRenderTarget( renderer, nullptr );
    
    SDL_UpdateTexture( texture, nullptr, pixels.data(), texture_width * 4 );
    SDL_RenderCopy( renderer, texture, nullptr, &visualizer );
    SDL_RenderPresent( renderer );
}

// int dx{1};
// int dy{1};
// double speed{0.35};

// void render_bounce_frame() {
//     auto update_visualizer = []() {
//         if (visualizer.x + visualizer.w > window_width || visualizer.x < 0) {
//             dx = -dx;
//         }

//         if (visualizer.y + visualizer.h > window_height || visualizer.y < 0) {
//             dy = -dy;
//         }

//         visualizer.x += static_cast<int>(dx * speed * frametime);
//         visualizer.y += static_cast<int>(dy * speed * frametime);
//     };

//     SDL_SetRenderDrawColor(renderer, 18, 18, 18, 255);
//     SDL_RenderClear(renderer);

//     update_visualizer();
    
//     SDL_SetRenderDrawColor(renderer, 255, 0, 255, 255);
//     SDL_RenderFillRect(renderer, &visualizer);

//     SDL_RenderPresent(renderer);
// }

int main() {
    auto audio_rate = 48000;
    auto audio_format = static_cast<uint16_t>(AUDIO_S32);
    auto audio_channels = 1;
    constexpr auto loops = 1;
    constexpr auto reverse_sample = true;

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

    std::string_view filename = "test2.wav";
    auto *wave = Mix_LoadWAV(filename.data());

    if (wave == nullptr) {
        SDL_Log("Couldn't load %s: %s\n", filename.data(), SDL_GetError());
        return 1;
    }


    if (reverse_sample) {
        flip_sample(wave);
    }

    Mix_PlayChannel(0, wave, loops);

    std::cout << "Playing: " << filename << '\n';
    bool exit = false;

    sample_length_ms = analyze_sample(wave);
    while (!exit) {
        render_frame();
        exit = handle_input();
        SDL_Delay(static_cast<int>(frametime));
    }

    std::cout << "Finished.\n";
}
