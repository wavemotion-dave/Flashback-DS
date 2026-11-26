/* REminiscence - Flashback interpreter
 * Copyright (C) 2005-2011 Gregory Montoir
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef __NDS__
#include <nds.h>
#include <maxmod9.h>
#else
#include <SDL.h>
#endif

#include "scaler.h"
#include "systemstub.h"

#ifdef __NDS__

typedef enum {
  EMUARM7_INIT_SND = 0x123C,
  EMUARM7_STOP_SND = 0x123D,
  EMUARM7_PLAY_SND = 0x123E,
} FifoMesType;


#define  cxBG (0<<8)
#define  cyBG (0<<8)
#define  xdxBG (((256 / 256) << 8) | (256 % 256))
#define  ydyBG ((((224+8) / 192) << 8) | ((224+8) % 192))

#define  cyBG_Zoomed (16<<8)
#define  ydyBG_Zoomed (((192 / 192) << 8) | (192 % 192))

// VRAM bitmap stride in pixels (BgSize_B16_256x256)
#define DS_FB_PITCH 256

// 0: keep the original DS behaviour on shake (top rows are blanked, picture not moved)
// 1: shift the picture down by the shake offset like the SDL build does
#define DS_SHAKE_SHIFT 0

uint8_t bZoomScreen = 0;

extern int bg0, bg1, bg0b, bg1b;

volatile u32 emuFps;                     // Fps to display
volatile u32 emuActFrames;               // Actual number of frames 
volatile u16 g_framePending = 0;         // To manage Vcount and VBL Interrupt

extern bool gameToLoad;

mm_ds_system sys;
mm_stream mystream;

#include "mixer.h"
Mixer *theMixer;

typedef struct {
  int x,y,w,h;
} SDL_Rect;

int dampen_unzoom = 0;
ITCM_CODE void vblankIntr() 
{
    if(g_framePending == 2) {
        g_framePending = 0;
        emuActFrames++;
    }

    // Antialias tile layer - just a bit of blurring to help vertical compression
    if (bZoomScreen)
    {
        dampen_unzoom = 60;
        REG_BG2PA = xdxBG ; REG_BG2PB = 0; REG_BG2PC =0; REG_BG2PD = ydyBG_Zoomed; 
        REG_BG3PA = xdxBG;  REG_BG3PB = 0; REG_BG3PC =0; REG_BG3PD = ydyBG_Zoomed; 

        REG_BG2X = cxBG;    REG_BG2Y = cyBG_Zoomed;
        REG_BG3X = cxBG;    REG_BG3Y = cyBG_Zoomed;
    }
    else
    {
        if (dampen_unzoom)
        {
            dampen_unzoom--;
        }
        else
        {
            REG_BG2PA = xdxBG ; REG_BG2PB = 0; REG_BG2PC =0; REG_BG2PD = ydyBG; 
            REG_BG3PA = xdxBG;  REG_BG3PB = 0; REG_BG3PC =0; REG_BG3PD = ydyBG; 
            
            REG_BG2X = cxBG;    REG_BG2Y = 0x40;
            REG_BG3X = cxBG;    REG_BG3Y = 0xC0;
        }
    }

  
    static u16 sTime = 0;
    sTime++;
    if(sTime >= 60) {
        sTime = 0;
        emuFps = emuActFrames;
        emuActFrames = 0;
    }
}

// VBL Interrupt
ITCM_CODE static void vcountIntr() {
    if (g_framePending == 1 && REG_VCOUNT < 192) {
        g_framePending = 2;
    }
}

// a millisecond counter
volatile int milliseconds=0;

void timer0_function( void ) {
  // increment milliseconds
  milliseconds++;
} 

// ---------------------------------------------------------------- fast 16/32-bit blit helpers

typedef uint32 u32a __attribute__((may_alias));

static inline __attribute__((always_inline)) uint32 ld32(const uint8 *p) {
    if (((uintptr_t)p & 3) == 0) {
        return *(const u32a *)p;
    }
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32)p[3] << 24);
}

// 8-bit palettised rect -> 16-bit pixels, two pixels per 32-bit store.
// dst must be a 16-bit pixel pointer (VRAM or a main RAM buffer) with DS_FB_PITCH stride.
static void ITCM_CODE convertRect(uint16 *dst, const uint8 *src, int srcPitch, int w, int h, const uint16 *pal) {
    do {
        uint16 *d = dst;
        const uint8 *s = src;
        int n = w;
        if (((uintptr_t)d & 2) != 0 && n > 0) {
            *d++ = pal[*s++];
            --n;
        }
        while (n >= 4) {
            const uint32 v = ld32(s);
            ((u32a *)d)[0] = pal[v & 0xFF] | ((uint32)pal[(v >> 8) & 0xFF] << 16);
            ((u32a *)d)[1] = pal[(v >> 16) & 0xFF] | ((uint32)pal[v >> 24] << 16);
            s += 4;
            d += 4;
            n -= 4;
        }
        while (n-- > 0) {
            *d++ = pal[*s++];
        }
        dst += DS_FB_PITCH;
        src += srcPitch;
    } while (--h);
}

// 32-bit word copy (both pointers 4-byte aligned, bytes multiple of 4)
static void ITCM_CODE copyWords(void *dst, const void *src, int bytes) {
    u32a *d = (u32a *)dst;
    const u32a *s = (const u32a *)src;
    for (int n = bytes >> 2; n > 0; --n) {
        *d++ = *s++;
    }
}

// dst = (old * (32 - a) + new * a) / 32 per RGB555 channel, 2 pixels per iteration.
// a in [0, 32]; bit 15 is forced on (opaque BMP16 pixel).
static void ITCM_CODE blendFrame(uint16 *dst, const uint16 *oldF, const uint16 *newF, int pixels, uint32 a) {
    const uint32 M = 0x03E07C1Fu;
    const uint32 wo = 32 - a;
    for (int i = 0; i < pixels; i += 2) {
        const uint32 o = *(const u32a *)(oldF + i);
        const uint32 n = *(const u32a *)(newF + i);
        const uint32 o0 = o & 0xFFFF, o1 = o >> 16;
        const uint32 n0 = n & 0xFFFF, n1 = n >> 16;
        const uint32 vo0 = (o0 | (o0 << 16)) & M;
        const uint32 vn0 = (n0 | (n0 << 16)) & M;
        const uint32 vo1 = (o1 | (o1 << 16)) & M;
        const uint32 vn1 = (n1 | (n1 << 16)) & M;
        const uint32 r0 = ((vo0 * wo + vn0 * a) >> 5) & M;
        const uint32 r1 = ((vo1 * wo + vn1 * a) >> 5) & M;
        const uint32 p0 = ((r0 | (r0 >> 16)) & 0x7FFF) | 0x8000;
        const uint32 p1 = ((r1 | (r1 >> 16)) & 0x7FFF) | 0x8000;
        *(u32a *)(dst + i) = p0 | (p1 << 16);
    }
}

#endif

struct SystemStub_SDL : SystemStub {
    enum {
        MAX_BLIT_RECTS = 200,
        SOUND_SAMPLE_RATE = 22050,
        JOYSTICK_COMMIT_VALUE = 3200
    };
#ifdef __NDS__
    uint16 *_screenBuffer;       // unused on DS (copyRect writes straight to VRAM)
    uint16 *_fadeScreenBuffer;   // old frame during a fade
#else
    uint16 *_screenBuffer;
    uint16 *_fadeScreenBuffer;
    SDL_Surface *_screenSurface;
#endif
    uint16 *_fadeNewBuffer;      // new frame during a fade (DS only)
    bool _fullscreen;
    int _currentScaler;
    uint8 _overscanColor;
    uint16 _pal[256];
    int _screenW, _screenH;
#ifndef __NDS__
    SDL_Joystick *_joystick;
#endif  
    SDL_Rect _blitRects[MAX_BLIT_RECTS];
    int _numBlitRects;
    bool _fadeOnUpdateScreen;

    virtual ~SystemStub_SDL() {}
    virtual void init(const char *title, int w, int h);
    virtual void destroy();
    virtual void setPalette(const uint8 *pal, int n);
    virtual void setPaletteEntry(int i, const Color *c);
    virtual void getPaletteEntry(int i, Color *c);
    virtual void setOverscanColor(int i);
    virtual void copyRect(int x, int y, int w, int h, const uint8 *buf, int pitch);
    virtual void fadeScreen();
    virtual void updateScreen(int shakeOffset);
    virtual void processEvents();
    virtual void sleep(int duration);
    virtual uint32 getTimeStamp();
    virtual void startAudio(AudioCallback callback, void *param);
    virtual void stopAudio();
    virtual uint32 getOutputSampleRate();
    virtual void lockAudio();
    virtual void unlockAudio();

    void prepareGfxMode();
    void cleanupGfxMode();
    void switchGfxMode(bool fullscreen, uint8 scaler);
    void flipGfx();
    void forceGfxRedraw();
    void drawRect(SDL_Rect *rect, uint8 color, uint16 *dst, uint16 dstPitch);
};

SystemStub *SystemStub_SDL_create() {
    return new SystemStub_SDL();
}

void SystemStub_SDL::init(const char *title, int w, int h) {
    // Prepare DS graphic mode
    videoSetMode(MODE_5_2D | DISPLAY_BG2_ACTIVE | DISPLAY_BG3_ACTIVE);
    vramSetBankA(VRAM_A_MAIN_BG_0x06000000);
    vramSetBankB(VRAM_B_MAIN_BG_0x06020000 );
    bg0 = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0,0);
    bg1 = bgInit(2, BgType_Bmp16, BgSize_B16_256x256, 0,0);

    // copyRect() now writes straight into VRAM, so start from a clean bitmap
    dmaFillWords(0, bgGetGfxPtr(bg0), DS_FB_PITCH * 256 * 2);

    REG_BLDCNT = BLEND_ALPHA | BLEND_SRC_BG2 | BLEND_DST_BG3;
    REG_BLDALPHA = (8 << 8) | 8; // 50% / 50% 

    REG_BG2PB = 0;
    REG_BG2PC = 0;
    REG_BG3PB = 0;
    REG_BG3PC = 0;

    REG_BG2X = cxBG; 
    REG_BG2Y = cyBG; 
    REG_BG3X = cxBG; 
    REG_BG3Y = cyBG; 
    REG_BG2PA = xdxBG ; 
    REG_BG2PD = ydyBG; 
    REG_BG3PA = xdxBG; 
    REG_BG3PD = ydyBG; 

    // Prepare timer for tick 
    milliseconds=0;
    irqSet( IRQ_TIMER0, timer0_function ); 
    TIMER0_DATA = (u16) TIMER_FREQ(1000);
    TIMER0_CR = TIMER_ENABLE | TIMER_IRQ_REQ; 
    irqEnable(IRQ_TIMER0); 

    // Init vbl and hbl func
    SetYtrigger(190); //trigger 2 lines before vsync
    irqSet(IRQ_VBLANK, vblankIntr);
    irqSet(IRQ_VCOUNT, vcountIntr);
    irqEnable(IRQ_VBLANK | IRQ_VCOUNT);

    memset(&_pi, 0, sizeof(_pi));
    if (gameToLoad) { _pi.load =true; gameToLoad = false; }
    _screenW = w;
    _screenH = h;

    assert(_screenW == DS_FB_PITCH && _screenH <= 256);
    _screenBuffer = 0;

    _fadeScreenBuffer = 0;
    _fadeNewBuffer = 0;
    _fadeOnUpdateScreen = false;
    _fullscreen = false;
    _currentScaler = 2;
    memset(_pal, 0, sizeof(_pal));
    prepareGfxMode();
}

void SystemStub_SDL::destroy() {
    cleanupGfxMode();
#ifndef __NDS__
    if (SDL_JoystickOpened(0)) {
        SDL_JoystickClose(_joystick);
    }
    SDL_Quit();
#endif
}

ITCM_CODE void SystemStub_SDL::setPalette(const uint8 *pal, int n) {
    assert(n <= 256);
    for (int i = 0; i < n; ++i) {
        uint8 r = pal[i * 3 + 0];
        uint8 g = pal[i * 3 + 1];
        uint8 b = pal[i * 3 + 2];

        BG_PALETTE[i] = RGB15( (r>>3), (g>>3), (b>>3) );
        _pal[i] = RGB15( (r>>3), (g>>3), (b>>3) ) | 0x8000;
    }
}

ITCM_CODE void SystemStub_SDL::setPaletteEntry(int i, const Color *c) {
    uint8 r = (c->r << 2) | (c->r & 3);
    uint8 g = (c->g << 2) | (c->g & 3);
    uint8 b = (c->b << 2) | (c->b & 3);

    BG_PALETTE[i] = RGB15( (r>>3), (g>>3), (b>>3) );
    _pal[i] = RGB15( (r>>3), (g>>3), (b>>3) ) | 0x8000;
}

ITCM_CODE void SystemStub_SDL::getPaletteEntry(int i, Color *c) {
    c->r = ((_pal[i]    ) & 0x1F)<<3;
    c->g = ((_pal[i]>>5 ) & 0x1F)<<3;
    c->b = ((_pal[i]>>10) & 0x1F)<<3;

    c->r >>= 2;
    c->g >>= 2;
    c->b >>= 2;
}

void SystemStub_SDL::setOverscanColor(int i) {
    _overscanColor = i;
}

ITCM_CODE void SystemStub_SDL::copyRect(int x, int y, int w, int h, const uint8 *buf, int pitch) {
    // Convert the 8-bit game layer straight into VRAM (or into the "new frame" buffer while a
    // fade is pending). No 1-pixel border, no intermediate screen buffer.
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > _screenW) {
        w = _screenW - x;
    }
    if (y + h > _screenH) {
        h = _screenH - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }
    uint16 *base = _fadeOnUpdateScreen ? _fadeNewBuffer : (uint16 *)bgGetGfxPtr(bg0);
    convertRect(base + y * DS_FB_PITCH + x, buf + y * pitch + x, pitch, w, h, _pal);
}

ITCM_CODE void SystemStub_SDL::fadeScreen() {
#ifdef __NDS__
    // Snapshot what is on screen now; copyRect() will build the new frame in _fadeNewBuffer
    // (starting from the old one) until the next updateScreen() runs the fade.
    const int fadeScreenBufferSize = _screenH * DS_FB_PITCH * sizeof(uint16);
    if (!_fadeScreenBuffer) {
        _fadeScreenBuffer = (uint16 *)malloc(fadeScreenBufferSize);
        assert(_fadeScreenBuffer);
    }
    if (!_fadeNewBuffer) {
        _fadeNewBuffer = (uint16 *)malloc(fadeScreenBufferSize);
        assert(_fadeNewBuffer);
    }
    copyWords(_fadeScreenBuffer, bgGetGfxPtr(bg0), fadeScreenBufferSize);
    memcpy(_fadeNewBuffer, _fadeScreenBuffer, fadeScreenBufferSize);
    _fadeOnUpdateScreen = true;
#else
    const int fadeScreenBufferSize = _screenH * _screenW * sizeof(uint16);
    if (!_fadeScreenBuffer) {
        _fadeScreenBuffer = (uint16 *)malloc(fadeScreenBufferSize);
        assert(_fadeScreenBuffer);
    }
    _fadeOnUpdateScreen = true;
    memcpy(_fadeScreenBuffer, _screenBuffer + _screenW + 1, fadeScreenBufferSize);
#endif
}

#ifndef __NDS__
ITCM_CODE static uint16 blendPixel16(uint16 colorSrc, uint16 colorDst, uint32 mask, int step) {
    const uint32 pSrc = (colorSrc | (colorSrc << 16)) & mask;
    const uint32 pDst = (colorDst | (colorDst << 16)) & mask;
    const uint32 pRes = ((pDst - pSrc) * step / 16 + pSrc) & mask;
    return pRes | (pRes >> 16);
}
#endif

ITCM_CODE void SystemStub_SDL::updateScreen(int shakeOffset) {
    uint16 *vram = (uint16 *)bgGetGfxPtr(bg0);
    if (_fadeOnUpdateScreen) {
        const int pixels = _screenH * DS_FB_PITCH;
        for (int i = 1; i <= 16; ++i) {
            blendFrame(vram, _fadeScreenBuffer, _fadeNewBuffer, pixels, i * 2);
            swiWaitForVBlank();
        }
        g_framePending = 1;
        _fadeOnUpdateScreen = false;
        return;
    }
    if (shakeOffset != 0) {
        if (shakeOffset > _screenH) {
            shakeOffset = _screenH;
        }
#if DS_SHAKE_SHIFT
        for (int j = _screenH - 1; j >= shakeOffset; --j) {
            copyWords(vram + j * DS_FB_PITCH, vram + (j - shakeOffset) * DS_FB_PITCH, DS_FB_PITCH * 2);
        }
#endif
        const uint32 c = _pal[_overscanColor];
        dmaFillWords(c | (c << 16), vram, shakeOffset * DS_FB_PITCH * 2);
    }
    // everything else was already written to VRAM by copyRect()
    g_framePending = 1;
}


ITCM_CODE void SystemStub_SDL::processEvents() {
  scanKeys();  
  uint32 keys_down = keysDown();
  uint32 keys_up = keysUp();
  
  if ( (keys_up & KEY_LEFT) ) {
    _pi.dirMask &=~PlayerInput::DIR_LEFT;
  }
  if ( (keys_up & KEY_RIGHT) ) {
    _pi.dirMask &=~PlayerInput::DIR_RIGHT;
  }
  if ( (keys_up & (KEY_UP | KEY_X) ) ) {
    _pi.dirMask &=~PlayerInput::DIR_UP;
  }
  if ( (keys_up & KEY_DOWN) ) {
    _pi.dirMask &=~PlayerInput::DIR_DOWN;
  }
  if ( (keys_up & KEY_Y) ) {
    _pi.enter = false;
  }
  if ( (keys_up & KEY_A) ) {
    _pi.shift = false;
  }
  if ( (keys_up & KEY_B) ) {
    _pi.space = false;
  }
  if ( (keys_up & (KEY_R | KEY_START)) ) {
    _pi.backspace = false;
  }
  if (keys_up & (KEY_L | KEY_SELECT)) {
    _pi.escape = false;
  }

  if ( (keys_down & KEY_LEFT) ) {
    _pi.dirMask |= PlayerInput::DIR_LEFT;
  }
  if ( (keys_down & KEY_RIGHT) ) {
    _pi.dirMask |= PlayerInput::DIR_RIGHT;
  }
  if ( (keys_down & (KEY_UP | KEY_X) ) ) {
    _pi.dirMask |= PlayerInput::DIR_UP;
  }
  if ( (keys_down & KEY_DOWN) ) {
    _pi.dirMask |= PlayerInput::DIR_DOWN;
  }
  if ( (keys_down & KEY_Y) ) {
    _pi.enter = true;
  }
  if ( (keys_down & KEY_A) ) {
    _pi.shift = true;
  }
  if ( (keys_down & KEY_B) ) {
    _pi.space = true;
  }
  if ( (keys_down & (KEY_R | KEY_START)) ) {
    _pi.backspace = true;
  }
  if (keys_down & (KEY_L | KEY_SELECT)) {
    _pi.escape = true;
  }
  
  // The X jump supersedes left/right
  if (keys_down & KEY_X)
  {
      _pi.dirMask &=~PlayerInput::DIR_LEFT;
      _pi.dirMask &=~PlayerInput::DIR_RIGHT;
  }
}

ITCM_CODE void SystemStub_SDL::sleep(int duration) {
  uint32 debtime=milliseconds;
  while ((int)(milliseconds-debtime)<duration);
}

ITCM_CODE uint32 SystemStub_SDL::getTimeStamp() {
  return milliseconds; 
}

ITCM_CODE mm_word  TimerMaxMod( mm_word length, mm_addr dest, mm_stream_formats format ) {
  int8* theDest = (int8 *) dest;
  theMixer->mix((int8 *) theDest,length);
    
  return length;
}

void SystemStub_SDL::startAudio(AudioCallback callback, void *param) {
    theMixer =  (Mixer *) param;   
    sys.mod_count           = 0;
    sys.samp_count          = 0;
    sys.mem_bank            = 0;
    sys.fifo_channel        = FIFO_MAXMOD;
    mmInit( &sys );
    mystream.sampling_rate  = SOUND_SAMPLE_RATE;        // sampling rate =
    mystream.buffer_length  = 1024;                     // buffer length =
    mystream.callback       = TimerMaxMod;              // set callback function
    mystream.format         = MM_STREAM_8BIT_MONO;      // format = mono 8-bit
    mystream.timer          = MM_TIMER3;                // use hardware timer 3
    mystream.manual         = false;                    // use automatic filling
    mmStreamOpen( &mystream );
}

void SystemStub_SDL::stopAudio() {
  mmStreamClose();
}

uint32 SystemStub_SDL::getOutputSampleRate() {
    return SOUND_SAMPLE_RATE;
}

void SystemStub_SDL::lockAudio() {
#ifdef __NDS__
#else
    SDL_LockAudio();
#endif
}

void SystemStub_SDL::unlockAudio() {
#ifdef __NDS__
#else
    SDL_UnlockAudio();
#endif
}

ITCM_CODE void SystemStub_SDL::prepareGfxMode() {
#ifdef __NDS__
#else
    int w = _screenW * _scalers[_currentScaler].factor;
    int h = _screenH * _scalers[_currentScaler].factor;
    _screenSurface = SDL_SetVideoMode(w, h, 16, _fullscreen ? (SDL_FULLSCREEN | SDL_HWSURFACE) : SDL_HWSURFACE);
    if (!_screenSurface) {
        error("SystemStub_SDL::prepareGfxMode() Unable to allocate _screen buffer");
    }
#endif
    forceGfxRedraw();
}

void SystemStub_SDL::cleanupGfxMode() {
    if (_screenBuffer) {
        free(_screenBuffer);
        _screenBuffer = 0;
    }
    if (_fadeScreenBuffer) {
        free(_fadeScreenBuffer);
        _fadeScreenBuffer = 0;
    }
    if (_fadeNewBuffer) {
        free(_fadeNewBuffer);
        _fadeNewBuffer = 0;
    }
#ifndef __NDS__
    if (_screenSurface) {
        // freed by SDL_Quit()
        _screenSurface = 0;
    }
#endif  
}

void SystemStub_SDL::switchGfxMode(bool fullscreen, uint8 scaler) {
#ifdef __NDS__
#else
    SDL_FreeSurface(_screenSurface);
#endif
    _fullscreen = fullscreen;
    _currentScaler = scaler;
    prepareGfxMode();
    forceGfxRedraw();
}

ITCM_CODE void SystemStub_SDL::flipGfx() {
    uint16 scanline[256];
    assert(_screenW <= 256);
    uint16 *p = _screenBuffer + _screenW + 1;
    for (int y = 0; y < _screenH; ++y) {
        p += _screenW;
        for (int x = 0; x < _screenW; ++x) {
            scanline[x] = *--p;
        }
        memcpy(p, scanline, _screenW * sizeof(uint16));
        p += _screenW;
    }
    forceGfxRedraw();
}

ITCM_CODE void SystemStub_SDL::forceGfxRedraw() {
    _numBlitRects = 1;
    _blitRects[0].x = 0;
    _blitRects[0].y = 0;
    _blitRects[0].w = _screenW;
    _blitRects[0].h = _screenH;
}

ITCM_CODE void SystemStub_SDL::drawRect(SDL_Rect *rect, uint8 color, uint16 *dst, uint16 dstPitch) {
    dstPitch >>= 1;
    int x1 = rect->x;
    int y1 = rect->y;
    int x2 = rect->x + rect->w - 1;
    int y2 = rect->y + rect->h - 1;
    assert(x1 >= 0 && x2 < _screenW && y1 >= 0 && y2 < _screenH);
    for (int i = x1; i <= x2; ++i) {
        *(dst + y1 * dstPitch + i) = *(dst + y2 * dstPitch + i) = _pal[color];
    }
    for (int j = y1; j <= y2; ++j) {
        *(dst + j * dstPitch + x1) = *(dst + j * dstPitch + x2) = _pal[color];
    }
}
