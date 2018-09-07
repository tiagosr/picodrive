#include <emscripten.h>
#include <SDL2/SDL.h>

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <pico/pico_int.h>
#include <pico/state.h>
#include <pico/patch.h>
#include "../common/emu.h"
#include "../common/input_pico.h"
#include "../common/version.h"

SDL_Window *window = NULL;
SDL_Renderer *renderer = NULL;
SDL_Texture *texture = NULL;
bool simulate = true;
bool render_graphics = true;
bool render_sound = true;
bool loaded = false;
SDL_AudioDeviceID sound_dev = 0;
void* loaded_rom = NULL;
int vdp_start_line = 0;
int vdp_line_count = 240;

static unsigned short vout_buf[320*240];
static short snd_buffer[2*48000/50];
#define SND_QUEUE_SAMPLES 65536
static short snd_queue[SND_QUEUE_SAMPLES];
static int snd_queue_insert_idx = 0;
static int snd_queue_read_idx = 0;

static int em_sound_init();
static void snd_write(int len) {
    if (sound_dev > 0) {
        SDL_QueueAudio(sound_dev, PicoIn.sndOut, len);
        //printf("Enqueuing %d bytes to sound\n", len);
    }
    //len = len * 2;
    //for (int i = 0; i < len; i++) {
    //    if (snd_queue_insert_idx >= SND_QUEUE_SAMPLES)
    //        snd_queue_insert_idx -= SND_QUEUE_SAMPLES;
    //    snd_queue[snd_queue_insert_idx] = snd_buffer[i];
    //    snd_queue_insert_idx++;
    //}
}

static void em_byteswap(void *dst, const void *src, int len)
{
    const unsigned int *ps = src;
    unsigned int *pd = dst;
    int i, m;

    if (len < 2)
        return;

    m = 0x00ff00ff;
    for (i = 0; i < len / 4; i++)
    {
        unsigned int t = ps[i];
        pd[i] = ((t & m) << 8) | ((t & ~m) >> 8);
    }
}

EMSCRIPTEN_KEEPALIVE
int cart_insert(unsigned char *buf, unsigned int size, char *config)
{
    if (loaded_rom) {
        PicoCartUnload();
    }
    void* new_rom = malloc(size);
    if (new_rom) {
        PicoIn.AHW = 0;
        PicoIn.quirks = 0;
        em_byteswap(new_rom, buf, size);
        int ret = PicoCartInsert(new_rom, size, NULL);
        PicoDetectRegion();
        PicoLoopPrepare();
        snd_queue_insert_idx = 0;
        snd_queue_read_idx = 0;
        memset(snd_buffer, 0, sizeof(snd_buffer));
        memset(snd_queue, 0, sizeof(snd_queue));
        PicoIn.sndOut = snd_buffer;
        PsndRerate(0);
        if (loaded_rom)
            free(loaded_rom);
        loaded_rom = new_rom;
        printf("Loaded ROM from cart_insert - %u bytes\n", size);
        return ret;
    } else {
        return -1;
    }
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
    void *pixels;
    int pitch;
    SDL_Rect dst_rect;
    dst_rect.x = 0;
    dst_rect.y = -vdp_start_line;
    dst_rect.w = 320;
    dst_rect.h = 240;
    PicoIn.pad[0] = PicoIn.pad[1] = 0;
    if (render_graphics)
    {
        SDL_RenderClear(renderer);
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
        SDL_LockTexture(texture, NULL, &pixels, &pitch);
        for (int y = 0; y < 240; y++) {
            for (int x = 0; x < 320; x++) {
                uint16_t rgb16 = vout_buf[x + 320*y];
                uint32_t rgb = 
                    (((rgb16&0x1f)*33)>>2) + // [4:0] * [9:0] >> [7:0]
                    ((((rgb16&0x7e0)*65)>>1) & 0xff00) + // [10:5] * [16:5] >> [15:4] & [15:8]
                    ((((rgb16&0xf800)*33)<<3) & 0xff0000) + // [15:11] * [20:11] << [23:14] & [23:16]
                    0xff000000;
                ((uint32_t*)pixels)[x + 320*y] = rgb;
            }
        }
        SDL_UnlockTexture(texture);
        SDL_RenderCopy(renderer, texture, NULL, &dst_rect);
        SDL_RenderPresent(renderer);
    }
    else
    {
        if (loaded)
        {
            PicoPatchApply();
            PicoFrame();
        }
    }
    //printf("Frame count: %d\n", Pico.m.frame_count);
}

static void em_audio_mixer_s16(void *user, Uint8 *stream, int len)
{
    int samples = len / 2; // 2 bytes per sample
    short *buf = (short *)stream;
    for (int i = 0; i < samples; i++)
    {
        if (snd_queue_insert_idx == snd_queue_read_idx)
            return;
        if (snd_queue_read_idx >= SND_QUEUE_SAMPLES)
            snd_queue_read_idx -= SND_QUEUE_SAMPLES;
        buf[i] = snd_queue[snd_queue_read_idx];
        snd_queue_read_idx++;
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
    as.samples = 2048;
    //as.callback = em_audio_mixer_s16;
    as.callback = NULL;

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
    }
    sound_dev = dev;
    SDL_PauseAudioDevice(dev, 0);
    return 0;
}

void emu_video_mode_change(int start_line, int line_count, int is_32_cols)
{
    printf("video mode changed!\n");
    SDL_SetWindowSize(window, is_32_cols ? 256 : 320, line_count);
    vdp_start_line = start_line;
    vdp_line_count = line_count;
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
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 320, 240);
    PicoIn.opt = POPT_EN_STEREO | POPT_EN_FM | POPT_EN_PSG | POPT_EN_Z80
        | POPT_EN_MCD_PCM | POPT_EN_MCD_CDDA | POPT_EN_MCD_GFX
        | POPT_EN_32X | POPT_EN_PWM
        | POPT_ACC_SPRITES | POPT_DIS_32C_BORDER;
    PicoIn.sndRate = 44100;
    PicoIn.autoRgnOrder = 0x184; // TODO: use parameters here
    PicoIn.writeSound = snd_write;
    memset(snd_buffer, 0, sizeof(snd_buffer));
    memset(snd_queue, 0, sizeof(snd_queue));
    PicoIn.sndOut = snd_buffer;
    PicoIn.overclockM68k = 0;
    PicoIn.regionOverride = 0;
    PicoInit();
    PicoDrawSetOutFormat(PDF_RGB555, 0);
    PicoDrawSetOutBuf(vout_buf, 320*2);
    PicoCartInsert(NULL, 0, NULL);
    PicoLoopPrepare();
    PsndRerate(0);
    loaded = true;
    em_sound_init();
    emscripten_set_main_loop_arg(em_main_loop, NULL, 60, false);
    //emscripten_set_main_loop_timing(EM_TIMING_SETTIMEOUT, 33);
    return 0;
}
