/*
 * xbased native core — the Xbox Communicator (the voice headset's adapter,
 * USB 045e:0283) in a controller's slot (#254).
 *
 * Two interfaces of class 0x78, each with one isochronous endpoint of up to
 * 48 bytes a frame: 0x04 OUT, the earpiece, and 0x85 IN, the microphone,
 * both mono 16-bit at the rate the title sets with a vendor request to the
 * interface (SET_FEATURE, wIndex 0: the rate's index, 8000, 11025, 16000,
 * 22050 or 24000 Hz; wIndex 1: the automatic gain, taken and left). The
 * title's own driver (XAPI's, in its XBE) finds it behind the controller's
 * hub and moves a packet each way every 1 ms frame (ohci.c's isochronous
 * TDs).
 *
 * Here the microphone is the system's default recording device and the
 * earpiece its default playback device (beside the game's sound), through
 * SDL's audio streams, converted to and from the title's rate. A packet
 * carries the frame's share of the rate (16 kHz: 16 samples; 11025 Hz: 11
 * or 12, as the frames add up); the microphone short of it, silence fills
 * it, and more than 100 ms waiting (the title stopped reading) is dropped.
 * The system's devices open only once the title sets a rate or moves a
 * packet (a title without voice never holds the microphone), and the
 * microphone's stream keeps 200 ms at most while nothing reads it.
 *
 * Tests (dev builds, XBASED_DEVCTL): NC_VOICE_IN_WAV=<file> plays a WAV
 * file (looped) as the microphone instead, and NC_VOICE_OUT_RAW=<file>
 * writes the earpiece's samples there (16-bit mono at the log's rate).
 */
#include "native/dev/usb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NC_DESKTOP
#include <SDL3/SDL.h>
#endif

#define EP_OUT 4
#define EP_IN  5
#define MAX_PACKET 48

static const uint8_t k_dev_desc[18] = {
    18, 1, 0x10, 0x01,      /* USB 1.1 */
    0, 0, 0, 8,             /* class in the interfaces; endpoint 0: 8 bytes */
    0x5e, 0x04, 0x83, 0x02, /* 045e:0283 */
    0x10, 0x01,             /* release 1.10 */
    0, 0, 0, 1,             /* no strings; one configuration */
};

/* the configuration, its two interfaces and their endpoints (9-byte audio
 * endpoint descriptors: refresh and sync address after the interval) */
static const uint8_t k_cfg_desc[45] = {
    9, 2, 45, 0, 2, 1, 0, 0x80, 100,
    9, 4, 0, 0, 1, 0x78, 0, 0, 0,
    9, 5, 0x00 | EP_OUT, 0x01, MAX_PACKET, 0, 1, 0, 0,
    9, 4, 1, 0, 1, 0x78, 0, 0, 0,
    9, 5, 0x80 | EP_IN, 0x01, MAX_PACKET, 0, 1, 0, 0,
};

static const unsigned k_rates[5] = {8000, 11025, 16000, 22050, 24000};

typedef struct Comm {
    NcUsbDev dev;
    unsigned rate;
    uint64_t in_frames, out_frames;     /* packets, for each one's share of the rate */
    uint64_t in_packets, out_packets, in_short;
#ifdef NC_DESKTOP
    SDL_AudioStream *mic, *ear;
    bool opened;
    SDL_AudioSpec wav_spec;             /* NC_VOICE_IN_WAV: the file, looped through mic */
    uint8_t *wav;
    uint32_t wav_len;
#endif
    FILE *out_raw;                      /* NC_VOICE_OUT_RAW */
} Comm;

/* samples in the packet of frame n at the rate */
static unsigned frame_samples(unsigned rate, uint64_t n)
{
    return (unsigned)(((n + 1) * rate) / 1000 - (n * rate) / 1000);
}

#ifdef NC_DESKTOP
static void streams_format(Comm *c)
{
    const SDL_AudioSpec spec = {SDL_AUDIO_S16LE, 1, (int)c->rate};
    if (c->mic) {
        SDL_SetAudioStreamFormat(c->mic, NULL, &spec);  /* the device's side stays its own */
        SDL_ClearAudioStream(c->mic);
    }
    if (c->ear) {
        SDL_SetAudioStreamFormat(c->ear, &spec, NULL);
        SDL_ClearAudioStream(c->ear);
    }
}

/* the microphone's stream, as the device adds to it: no more than 200 ms
 * kept for a title that doesn't read it */
static void SDLCALL mic_put(void *opaque, SDL_AudioStream *s, int additional, int total)
{
    Comm *c = opaque;
    (void)additional;
    (void)total;
    if (SDL_GetAudioStreamAvailable(s) > (int)c->rate * 2 / 5) {
        SDL_ClearAudioStream(s);
    }
}

static void streams_open(Comm *c)
{
    if (c->opened) {
        return;
    }
    c->opened = true;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        nc_log("xbased-native-usb", "communicator: no audio (%s)", SDL_GetError());
        return;
    }
    const SDL_AudioSpec spec = {SDL_AUDIO_S16LE, 1, (int)c->rate};
#ifdef XBASED_DEVCTL
    const char *wav = getenv("NC_VOICE_IN_WAV");
#else
    const char *wav = NULL;
#endif
    if (wav && wav[0]) {
        if (SDL_LoadWAV(wav, &c->wav_spec, &c->wav, &c->wav_len)) {
            c->mic = SDL_CreateAudioStream(&c->wav_spec, &spec);
            nc_log("xbased-native-usb", "communicator: the microphone is %s (%d Hz, %d channel(s))", wav,
                   c->wav_spec.freq, c->wav_spec.channels);
        } else {
            nc_log("xbased-native-usb", "communicator: %s unread (%s)", wav, SDL_GetError());
        }
    } else {
        c->mic = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, NULL, NULL);
        if (c->mic) {
            SDL_SetAudioStreamPutCallback(c->mic, mic_put, c);
            SDL_ResumeAudioStreamDevice(c->mic);
            nc_log("xbased-native-usb", "communicator: the microphone is the system's");
        } else {
            nc_log("xbased-native-usb", "communicator: no microphone (%s)", SDL_GetError());
        }
    }
    c->ear = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (c->ear) {
        SDL_ResumeAudioStreamDevice(c->ear);
    } else {
        nc_log("xbased-native-usb", "communicator: no earpiece output (%s)", SDL_GetError());
    }
}
#endif

static int comm_control(NcUsbDev *d, const uint8_t *setup, uint8_t *data, int len)
{
    Comm *c = d->opaque;
    (void)data;
    (void)len;
    /* vendor, to the interface, host to device: SET_FEATURE */
    if (setup[0] == 0x41 && setup[1] == 0x03) {
        const unsigned value = nc_rd16(setup + 2), index = nc_rd16(setup + 4);
        if (index == 0 && (value & 0xff) < 5) {
            c->rate = k_rates[value & 0xff];
            c->in_frames = c->out_frames = 0;
#ifdef NC_DESKTOP
            streams_open(c);
            streams_format(c);
#endif
            nc_log("xbased-native-usb", "communicator: %u Hz", c->rate);
            return 0;
        }
        if (index == 1) {
            return 0;                   /* the automatic gain: the system's own */
        }
    }
    nc_log("xbased-native-usb", "communicator: request %02x %02x %04x %04x refused", setup[0], setup[1],
           nc_rd16(setup + 2), nc_rd16(setup + 4));
    return NC_USB_STALL;
}

static int comm_data(NcUsbDev *d, bool in, int ep, uint8_t *buf, int len)
{
    Comm *c = d->opaque;
#ifdef NC_DESKTOP
    streams_open(c);
#endif
    if (in && ep == EP_IN) {
        int n = (int)frame_samples(c->rate, c->in_frames++) * 2;
        n = n > len ? len : n;
        int got = 0;
#ifdef NC_DESKTOP
        if (c->mic) {
            if (c->wav && SDL_GetAudioStreamAvailable(c->mic) < n) {
                SDL_PutAudioStreamData(c->mic, c->wav, (int)c->wav_len);    /* the file again */
            }
            const int max_q = (int)c->rate * 2 / 10;
            if (!c->wav && SDL_GetAudioStreamAvailable(c->mic) > max_q) {
                SDL_ClearAudioStream(c->mic);   /* not read for 100 ms: from now on */
            }
            got = SDL_GetAudioStreamData(c->mic, buf, n);
            got = got < 0 ? 0 : got;
        }
#endif
        if (got < n) {
            memset(buf + got, 0, (size_t)(n - got));
            c->in_short++;
        }
        if (++c->in_packets % 10000 == 0) {
            nc_log("xbased-native-usb", "communicator: %llu packets from the microphone (%llu short of it), %llu to "
                   "the earpiece, %u Hz", (unsigned long long)c->in_packets, (unsigned long long)c->in_short,
                   (unsigned long long)c->out_packets, c->rate);
        }
        return n;
    }
    if (!in && ep == EP_OUT) {
        c->out_packets++;
        c->out_frames++;
        if (len > 0) {
#ifdef NC_DESKTOP
            if (c->ear) {
                if (SDL_GetAudioStreamQueued(c->ear) > (int)c->rate * 2 / 5) {
                    SDL_ClearAudioStream(c->ear);   /* 200 ms behind: caught up */
                }
                SDL_PutAudioStreamData(c->ear, buf, len);
            }
#endif
            if (c->out_raw) {
                fwrite(buf, 1, (size_t)len, c->out_raw);
            }
        }
        return len;
    }
    return NC_USB_STALL;
}

static void comm_reset(NcUsbDev *d)
{
    Comm *c = d->opaque;
    c->in_frames = c->out_frames = 0;
#ifdef NC_DESKTOP
    if (c->mic && !c->wav) {
        SDL_ClearAudioStream(c->mic);
    }
    if (c->ear) {
        SDL_ClearAudioStream(c->ear);
    }
#endif
}

NcUsbDev *nc_comm_new(void)
{
    Comm *c = calloc(1, sizeof(*c));
    c->rate = 16000;
    c->dev.name = "communicator";
    c->dev.dev_desc = k_dev_desc;
    c->dev.dev_desc_len = sizeof(k_dev_desc);
    c->dev.cfg_desc = k_cfg_desc;
    c->dev.cfg_desc_len = sizeof(k_cfg_desc);
    c->dev.control = comm_control;
    c->dev.data = comm_data;
    c->dev.reset = comm_reset;
    c->dev.opaque = c;
    c->dev.pending_addr = -1;
#ifdef XBASED_DEVCTL
    const char *raw = getenv("NC_VOICE_OUT_RAW");
    if (raw && raw[0]) {
        c->out_raw = fopen(raw, "wb");
    }
#endif
    nc_log("xbased-native-usb", "communicator in player 1's controller (slot A)%s", c->out_raw ? ", earpiece to a file" : "");
    return &c->dev;
}
