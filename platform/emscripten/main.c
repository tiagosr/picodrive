#include <emscripten.h>
#include <SDL2/SDL.h>

#include <stdint.h>
#include <stdbool.h>
#include <pico/pico_int.h>
#include <pico/state.h>
#include <pico/patch.h>
#include "../common/emu.h"
#include "../common/input_pico.h"
#include "../common/version.h"

SDL_Window *window = NULL;
SDL_Renderer *renderer = NULL;
SDL_Surface *surface = NULL;
SDL_Texture *texture = NULL;
bool simulate = true;
bool render_graphics = true;
bool render_sound = true;
bool loaded = false;
SDL_AudioDeviceID sound_dev = 0;

static int em_sound_init();

EMSCRIPTEN_KEEPALIVE
int cart_insert(unsigned char *buf, unsigned int size, char *config)
{
    return PicoCartInsert(buf, size, config);
}
EMSCRIPTEN_KEEPALIVE
void cart_unload(void) { PicoCartUnload(); }
EMSCRIPTEN_KEEPALIVE
void set_sprite_limit(bool limit)
{
    if (limit)
    {
        PicoIn.opt &= ~POPT_DIS_SPRITE_LIM;
    }
    else
    {
        PicoIn.opt |= POPT_DIS_SPRITE_LIM;
    }
}

EMSCRIPTEN_KEEPALIVE
void set_input_device_nothing(int index) { PicoSetInputDevice(index, PICO_INPUT_NOTHING); }
EMSCRIPTEN_KEEPALIVE
void set_input_device_3btn(int index) { PicoSetInputDevice(index, PICO_INPUT_PAD_3BTN); }
EMSCRIPTEN_KEEPALIVE
void set_input_device_6btn(int index) { PicoSetInputDevice(index, PICO_INPUT_PAD_6BTN); }
EMSCRIPTEN_KEEPALIVE
void send_input(int pad, int buttons) {
    switch (pad)
    {
        case 0:
        case 1:
            PicoIn.pad[pad] = buttons;
            break;

        default:
            break;
    }
    
}
EMSCRIPTEN_KEEPALIVE
void set_loop(bool _simulate, bool _render_graphics, bool _render_sound)
{
    simulate = _simulate;
    render_graphics = _render_graphics;
    if (render_sound != _render_sound)
    {
        if (_render_sound)
        {
            PsndRerate(1);
            if (em_sound_init() >= 0)
            {
                render_sound = true;
            }
        }
        else
        {
            if (sound_dev)
            {
                SDL_PauseAudioDevice(sound_dev, 1);
            }
            render_sound = false;
        }
    }
}

void em_main_loop(void *something)
{
    if (render_graphics)
    {
        SDL_RenderClear(renderer);
        SDL_LockSurface(surface);
        if (simulate)
        {
            if (loaded)
            {
                PicoPatchApply();
                PicoFrame();
            }
        }
        else
        {
            PicoFrameDrawOnly();
        }
        SDL_UnlockSurface(surface);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
    }
    else
    {
        if (loaded)
        {
            PicoPatchApply();
            SDL_LockSurface(surface);
            PicoFrame();
            SDL_UnlockSurface(surface);
        }
    }
}

static void em_audio_mixer_s16(void *user, Uint8 *stream, int len)
{
    int samples = len / 2; // stereo
    short *buf = (short *)stream;
    for (int i = 0; i < samples; i++)
    {
        buf[i] = PicoIn.sndOut[i];
    }
}
static int em_sound_init()
{
    if (sound_dev != 0)
    {
        SDL_CloseAudioDevice(sound_dev);
        sound_dev = 0;
    }
    SDL_AudioSpec as;
    as.freq = PicoIn.sndRate;
    as.format = AUDIO_S16;
    as.channels = 2;
    as.samples = Pico.snd.len;
    as.callback = em_audio_mixer_s16;

    SDL_AudioSpec as_got;
    int dev = SDL_OpenAudioDevice(NULL, 0, &as, &as_got, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (dev == 0)
    {
        fprintf(stderr, "Error opening audio device: %s\n", SDL_GetError());
        return -1;
    }
    if (as.freq != as_got.freq)
    {
        PicoIn.sndRate = as_got.freq;
        PsndRerate(1);
        if (as_got.samples != Pico.snd.len)
        {
            SDL_CloseAudioDevice(dev);
            as_got.samples = Pico.snd.len;
            dev = SDL_OpenAudioDevice(NULL, 0, &as_got, NULL, 0);
            if (dev == 0)
            {
                fprintf(stderr, "Error opening audio device: %s\n", SDL_GetError());
                return -1;
            }
        }
    }
    SDL_PauseAudioDevice(dev, 0);
    sound_dev = dev;
    return 0;
}

void emu_video_mode_change(int start_line, int line_count, int is_32_cols)
{
    printf("video mode changed!");
    SDL_SetWindowSize(window, is_32_cols ? 256 : 320, line_count);
    emscripten_cancel_main_loop();
    emscripten_set_main_loop_arg(em_main_loop, NULL, Pico.m.pal ? 50 : 60, false);
}

void emu_32x_startup(void)
{
    // stub method
}

void lprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
}

void *plat_mmap(unsigned long addr, size_t size, int need_exec, int is_fixed)
{
    return malloc(size);
}

void plat_munmap(void *base_addr, size_t len)
{
    free(base_addr);
}

void *plat_mremap(void *ptr, size_t oldsize, size_t newsize)
{
    void *tmp;
    size_t preserve_size = oldsize;
    if (preserve_size > newsize)
        preserve_size = newsize;
    tmp = malloc(preserve_size);
    if (tmp == NULL)
        return NULL;
    memcpy(tmp, ptr, preserve_size);
    plat_munmap(ptr, oldsize);
    return tmp;
}

/*******
 * INIT
 *******/

int main(int argc, char const *argv[])
{
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
    SDL_CreateWindowAndRenderer(320, 224, 0, &window, &renderer);
    surface = SDL_CreateRGBSurfaceWithFormat(0, 320, 240, 16, SDL_PIXELFORMAT_RGB555);
    texture = SDL_CreateTextureFromSurface(renderer, surface);
    PicoIn.opt = POPT_EN_STEREO | POPT_EN_FM | POPT_EN_PSG | POPT_EN_Z80 | POPT_EN_MCD_PCM | POPT_EN_MCD_CDDA | POPT_EN_MCD_GFX | POPT_EN_32X | POPT_EN_PWM | POPT_ACC_SPRITES | POPT_DIS_32C_BORDER;
    PicoIn.sndRate = 44100;
    PicoIn.autoRgnOrder = 0x184; // TODO: use parameters here
    PicoInit();
    PicoDrawSetOutFormat(PDF_RGB555, 0);
    PicoDrawSetOutBuf(surface->pixels, surface->pitch);

    PicoCartInsert(NULL, 0, "");
    loaded = true;
    emscripten_set_main_loop_arg(em_main_loop, NULL, 60, false);
    //emscripten_set_main_loop_timing(EM_TIMING_SETTIMEOUT, 33);

    return 0;
}
