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

#include <stdint.h>
#include <string.h>
#include "resource.h"
#include "systemstub.h"
#include "unpack.h"
#include "video.h"

#ifdef __NDS__
// Compile out debug tracing on the DS (each call is a varargs call per sprite/string/palette).
// Remove these two lines if you want DBG_VIDEO tracing back.
#undef debug
#define debug(...) ((void)0)
#endif

// ---------------------------------------------------------------- 32-bit blit helpers
// Sprite source pixels are 4-bit palette indices (< 0x80).

typedef uint32_t u32 __attribute__((may_alias));

// 0xFF in every byte lane of s that is non-zero (lanes must be < 0x80)
static inline __attribute__((always_inline)) uint32_t nzMask(uint32_t s) {
    uint32_t nz = (s + 0x7F7F7F7Fu) & 0x80808080u;
    return (nz - (nz >> 7)) | nz;
}

// 0xFF in every byte lane of d whose bit 7 is clear (i.e. may be overwritten)
static inline __attribute__((always_inline)) uint32_t okMask(uint32_t d) {
    uint32_t ok = ~d & 0x80808080u;
    return (ok - (ok >> 7)) | ok;
}

static inline __attribute__((always_inline)) uint32_t ld32(const uint8_t *p) {
    if (((uintptr_t)p & 3) == 0) {
        return *(const u32 *)p;
    }
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline __attribute__((always_inline)) uint32_t gather32(const uint8_t *p, int stride) {
    return p[0] | (p[stride] << 8) | (p[2 * stride] << 16) | ((uint32_t)p[3 * stride] << 24);
}

template <bool CHK>
static inline __attribute__((always_inline)) uint32_t blend32(uint32_t s, uint32_t d, uint32_t cm32) {
    uint32_t m = nzMask(s);
    if (CHK) {
        m &= okMask(d);
    }
    return (d & ~m) | ((s | cm32) & m);
}

// forward row: dst[i] = src[i]
template <bool CHK>
static inline __attribute__((always_inline)) void rowFwd(const uint8_t *src, uint8_t *dst, int w, uint8_t cm) {
    const uint32_t cm32 = cm * 0x01010101u;
    while (w > 0 && ((uintptr_t)dst & 3)) {
        const uint8_t s = *src;
        if (s != 0 && !(CHK && (*dst & 0x80))) {
            *dst = s | cm;
        }
        ++src; ++dst; --w;
    }
    while (w >= 4) {
        const uint32_t s = ld32(src);
        if (s != 0) {
            const uint32_t d = *(u32 *)dst;
            *(u32 *)dst = blend32<CHK>(s, d, cm32);
        }
        src += 4; dst += 4; w -= 4;
    }
    while (w > 0) {
        const uint8_t s = *src;
        if (s != 0 && !(CHK && (*dst & 0x80))) {
            *dst = s | cm;
        }
        ++src; ++dst; --w;
    }
}

// mirrored row: dst[i] = src[-i]
template <bool CHK>
static inline __attribute__((always_inline)) void rowRev(const uint8_t *src, uint8_t *dst, int w, uint8_t cm) {
    const uint32_t cm32 = cm * 0x01010101u;
    while (w > 0 && ((uintptr_t)dst & 3)) {
        const uint8_t s = *src;
        if (s != 0 && !(CHK && (*dst & 0x80))) {
            *dst = s | cm;
        }
        --src; ++dst; --w;
    }
    while (w >= 4) {
        const uint32_t s = __builtin_bswap32(ld32(src - 3));
        if (s != 0) {
            const uint32_t d = *(u32 *)dst;
            *(u32 *)dst = blend32<CHK>(s, d, cm32);
        }
        src -= 4; dst += 4; w -= 4;
    }
    while (w > 0) {
        const uint8_t s = *src;
        if (s != 0 && !(CHK && (*dst & 0x80))) {
            *dst = s | cm;
        }
        --src; ++dst; --w;
    }
}

// strided row: dst[i] = src[i * stride] (stride = +pitch or -pitch)
template <bool CHK>
static inline __attribute__((always_inline)) void rowStride(const uint8_t *src, int stride, uint8_t *dst, int w, uint8_t cm) {
    const uint32_t cm32 = cm * 0x01010101u;
    while (w > 0 && ((uintptr_t)dst & 3)) {
        const uint8_t s = *src;
        if (s != 0 && !(CHK && (*dst & 0x80))) {
            *dst = s | cm;
        }
        src += stride; ++dst; --w;
    }
    while (w >= 4) {
        const uint32_t s = gather32(src, stride);
        if (s != 0) {
            const uint32_t d = *(u32 *)dst;
            *(u32 *)dst = blend32<CHK>(s, d, cm32);
        }
        src += 4 * stride; dst += 4; w -= 4;
    }
    while (w > 0) {
        const uint8_t s = *src;
        if (s != 0 && !(CHK && (*dst & 0x80))) {
            *dst = s | cm;
        }
        src += stride; ++dst; --w;
    }
}

// 2 source bytes -> 4 pixels in one 32-bit write
static inline __attribute__((always_inline)) uint32_t unpack2(uint32_t b0, uint32_t b1) {
    return (b0 >> 4) | ((b0 & 15) << 8) | ((b1 >> 4) << 16) | ((b1 & 15) << 24);
}

ITCM_CODE static void unpackNibbles(const uint8 *src, int bytes, uint8 *dst) {
    if (((uintptr_t)dst & 3) == 0) {
        for (; bytes >= 4; bytes -= 4, src += 4, dst += 8) {
            ((u32 *)dst)[0] = unpack2(src[0], src[1]);
            ((u32 *)dst)[1] = unpack2(src[2], src[3]);
        }
    }
    for (; bytes > 0; --bytes, ++src, dst += 2) {
        dst[0] = *src >> 4;
        dst[1] = *src & 15;
    }
}

// ----------------------------------------------------------------

Video::Video(Resource *res, SystemStub *stub)
    : _res(res), _stub(stub) {
#ifdef __NDS__
    memset(_frontLayer, 0, GAMESCREEN_W * GAMESCREEN_H);
    memset(_backLayer, 0, GAMESCREEN_W * GAMESCREEN_H);
    memset(_tempLayer, 0, GAMESCREEN_W * GAMESCREEN_H);
    memset(_tempLayer2, 0, GAMESCREEN_W * GAMESCREEN_H);
#else
    _frontLayer = (uint8 *)malloc(GAMESCREEN_W * GAMESCREEN_H);
    memset(_frontLayer, 0, GAMESCREEN_W * GAMESCREEN_H);
    _backLayer = (uint8 *)malloc(GAMESCREEN_W * GAMESCREEN_H);
    memset(_backLayer, 0, GAMESCREEN_W * GAMESCREEN_H);
    _tempLayer = (uint8 *)malloc(GAMESCREEN_W * GAMESCREEN_H);
    memset(_tempLayer, 0, GAMESCREEN_W * GAMESCREEN_H);
    _tempLayer2 = (uint8 *)malloc(GAMESCREEN_W * GAMESCREEN_H);
    memset(_tempLayer2, 0, GAMESCREEN_W * GAMESCREEN_H);
#endif
    _screenBlocks = (uint8 *)malloc((GAMESCREEN_W / SCREENBLOCK_W) * (GAMESCREEN_H / SCREENBLOCK_H));
    memset(_screenBlocks, 0, (GAMESCREEN_W / SCREENBLOCK_W) * (GAMESCREEN_H / SCREENBLOCK_H));
    _fullRefresh = true;
    _shakeOffset = 0;
    _charFrontColor = 0;
    _charTransparentColor = 0;
    _charShadowColor = 0;
}

Video::~Video() {
#ifdef __NDS__
#else
    free(_frontLayer);
    free(_backLayer);
    free(_tempLayer);
    free(_tempLayer2);
#endif
    free(_screenBlocks);
}

ITCM_CODE void Video::markBlockAsDirty(int16 x, int16 y, uint16 w, uint16 h) {
    debug(DBG_VIDEO, "Video::markBlockAsDirty(%d, %d, %d, %d)", x, y, w, h);
    assert(x >= 0 && x + w <= GAMESCREEN_W && y >= 0 && y + h <= GAMESCREEN_H);
    int bx1 = x / SCREENBLOCK_W;
    int by1 = y / SCREENBLOCK_H;
    int bx2 = (x + w - 1) / SCREENBLOCK_W;
    int by2 = (y + h - 1) / SCREENBLOCK_H;
    assert(bx2 < GAMESCREEN_W / SCREENBLOCK_W && by2 < GAMESCREEN_H / SCREENBLOCK_H);
    for (; by1 <= by2; ++by1) {
        for (int i = bx1; i <= bx2; ++i) {
            _screenBlocks[by1 * (GAMESCREEN_W / SCREENBLOCK_W) + i] = 2;
        }
    }
}

ITCM_CODE void Video::updateScreen() {
    debug(DBG_VIDEO, "Video::updateScreen()");
//  _fullRefresh = true;
    if (_fullRefresh) {
        _stub->copyRect(0, 0, Video::GAMESCREEN_W, Video::GAMESCREEN_H, _frontLayer, 256);
        _stub->updateScreen(_shakeOffset);
        _fullRefresh = false;
    } else {
        int i, j;
        int count = 0;
        uint8 *p = _screenBlocks;
        for (j = 0; j < GAMESCREEN_H / SCREENBLOCK_H; ++j) {
            uint16 nh = 0;
            for (i = 0; i < GAMESCREEN_W / SCREENBLOCK_W; ++i) {
                if (p[i] != 0) {
                    --p[i];
                    ++nh;
                } else if (nh != 0) {
                    int16 x = (i - nh) * SCREENBLOCK_W;
                    _stub->copyRect(x, j * SCREENBLOCK_H, nh * SCREENBLOCK_W, SCREENBLOCK_H, _frontLayer, 256);
                    nh = 0;
                    ++count;
                }
            }
            if (nh != 0) {
                int16 x = (i - nh) * SCREENBLOCK_W;
                _stub->copyRect(x, j * SCREENBLOCK_H, nh * SCREENBLOCK_W, SCREENBLOCK_H, _frontLayer, 256);
                ++count;
            }
            p += GAMESCREEN_W / SCREENBLOCK_W;
        }
        if (count != 0) {
            _stub->updateScreen(_shakeOffset);
        }
    }
    if (_shakeOffset != 0) {
        _shakeOffset = 0;
        _fullRefresh = true;
    }
}

ITCM_CODE void Video::fullRefresh() {
    debug(DBG_VIDEO, "Video::fullRefresh()");
    _fullRefresh = true;
    memset(_screenBlocks, 0, (GAMESCREEN_W / SCREENBLOCK_W) * (GAMESCREEN_H / SCREENBLOCK_H));
}

ITCM_CODE void Video::fadeOut() {
    debug(DBG_VIDEO, "Video::fadeOut()");
    _stub->fadeScreen();
//  fadeOutPalette();
}

ITCM_CODE void Video::fadeOutPalette() {
    for (int step = 16; step >= 0; --step) {
        for (int c = 0; c < 256; ++c) {
            Color col;
            _stub->getPaletteEntry(c, &col);
            col.r = col.r * step >> 4;
            col.g = col.g * step >> 4;
            col.b = col.b * step >> 4;
            _stub->setPaletteEntry(c, &col);
        }
        fullRefresh();
        updateScreen();
        _stub->sleep(50);
    }
}

ITCM_CODE void Video::setPaletteColorBE(int num, int offset) {
    const int color = READ_BE_UINT16(_res->_pal + offset * 2);
    Color c;
    c.r = (color & 0x00F) << 2;
    c.g = (color & 0x0F0) >> 2;
    c.b = (color & 0xF00) >> 6;
    if (color != 0) {
        c.r |= 3;
        c.g |= 3;
        c.b |= 3;
    }
    _stub->setPaletteEntry(num, &c);
}

ITCM_CODE void Video::setPaletteSlotBE(int palSlot, int palNum) {
    debug(DBG_VIDEO, "Video::setPaletteSlotBE()");
    const uint8 *p = _res->_pal + palNum * 0x20;
    for (int i = 0; i < 16; ++i) {
        const int color = READ_BE_UINT16(p); p += 2;
        Color c;
        c.r = (color & 0x00F) << 2;
        c.g = (color & 0x0F0) >> 2;
        c.b = (color & 0xF00) >> 6;
        if (color != 0) {
            c.r |= 3;
            c.g |= 3;
            c.b |= 3;
        }
        _stub->setPaletteEntry(palSlot * 0x10 + i, &c);
    }
}

ITCM_CODE void Video::setPaletteSlotLE(int palSlot, const uint8 *palData) {
    debug(DBG_VIDEO, "Video::setPaletteSlotLE()");
    for (int i = 0; i < 16; ++i) {
        uint16 color = READ_LE_UINT16(palData); palData += 2;
        Color c;
        c.b = (color & 0x00F) << 2;
        c.g = (color & 0x0F0) >> 2;
        c.r = (color & 0xF00) >> 6;
        _stub->setPaletteEntry(palSlot * 0x10 + i, &c);
    }
}

ITCM_CODE void Video::setTextPalette() {
    debug(DBG_VIDEO, "Video::setTextPalette()");
    setPaletteSlotLE(0xE, _textPal);
}

ITCM_CODE void Video::setPalette0xF() {
    debug(DBG_VIDEO, "Video::setPalette0xF()");
    const uint8 *p = _palSlot0xF;
    for (int i = 0; i < 16; ++i) {
        Color c;
        c.r = *p++ >> 2;
        c.g = *p++ >> 2;
        c.b = *p++ >> 2;
        _stub->setPaletteEntry(0xF0 + i, &c);
    }
}

// Decodes an RLE chunk straight into dst (no _memBuf bounce + second memcpy).
// A chunk is expected to expand to exactly 256 * 56 bytes.
ITCM_CODE static void PC_decodeMapHelper(int sz, const uint8 *src, uint8 *dst) {
    const uint8 *end = src + sz;
    while (src < end) {
        int16 code = (int8)*src++;
        if (code < 0) {
            const int len = 1 - code;
            memset(dst, *src++, len);
            dst += len;
        } else {
            ++code;
            memcpy(dst, src, code);
            src += code;
            dst += code;
        }
    }
}

ITCM_CODE void Video::PC_decodeMap(int level, int room) {
    debug(DBG_VIDEO, "Video::PC_decodeMap(%d)", room);
    assert(room < 0x40);
    int32 off = READ_LE_UINT32(_res->_map + room * 6);
    if (off == 0) {
        error("Invalid room %d", room);
    }
    bool packed = true;
    if (off < 0) {
        off = -off;
        packed = false;
    }
    const uint8 *p = _res->_map + off;
    _mapPalSlot1 = *p++;
    _mapPalSlot2 = *p++;
    _mapPalSlot3 = *p++;
    _mapPalSlot4 = *p++;
    if (level == 4 && room == 60) {
        // workaround for wrong palette colors (fire)
        _mapPalSlot4 = 5;
    }
    if (packed) {
        uint8 *vid = _frontLayer;
        for (int i = 0; i < 4; ++i) {
            const int sz = READ_LE_UINT16(p); p += 2;
            PC_decodeMapHelper(sz, p, vid); p += sz;
            vid += 256 * 56;
        }
    } else {
        // 32-bit interleave of the 4 planes; _frontLayer must be 4-byte aligned
        for (int y = 0; y < 224; ++y) {
            const uint8 *p0 = p + 64 * y;
            const uint8 *p1 = p0 + 256 * 56;
            const uint8 *p2 = p1 + 256 * 56;
            const uint8 *p3 = p2 + 256 * 56;
            u32 *d = (u32 *)(_frontLayer + 256 * y);
            for (int x = 0; x < 64; ++x) {
                d[x] = p0[x] | (p1[x] << 8) | (p2[x] << 16) | ((uint32_t)p3[x] << 24);
            }
        }
    }
    memcpy(_backLayer, _frontLayer, Video::GAMESCREEN_W * Video::GAMESCREEN_H);
}

ITCM_CODE void Video::PC_setLevelPalettes() {
    debug(DBG_VIDEO, "Video::PC_setLevelPalettes()");
    if (_unkPalSlot2 == 0) {
        _unkPalSlot2 = _mapPalSlot3;
    }
    if (_unkPalSlot1 == 0) {
        _unkPalSlot1 = _mapPalSlot3;
    }
    setPaletteSlotBE(0x0, _mapPalSlot1);
    setPaletteSlotBE(0x1, _mapPalSlot2);
    setPaletteSlotBE(0x2, _mapPalSlot3);
    setPaletteSlotBE(0x3, _mapPalSlot4);
    if (_unkPalSlot1 == _mapPalSlot3) {
        setPaletteSlotLE(4, _conradPal1);
    } else {
        setPaletteSlotLE(4, _conradPal2);
    }
    // slot 5 is monster palette
    setPaletteSlotBE(0x8, _mapPalSlot1);
    setPaletteSlotBE(0x9, _mapPalSlot2);
    setPaletteSlotBE(0xA, _unkPalSlot2);
    setPaletteSlotBE(0xB, _mapPalSlot4);
    // slots 0xC and 0xD are cutscene palettes
    setTextPalette();
}

ITCM_CODE void Video::PC_decodeIcn(const uint8 *src, int num, uint8 *dst) {
    const int offset = READ_LE_UINT16(src + num * 2);
    unpackNibbles(src + offset + 2, 16 * 16 / 2, dst);
}

ITCM_CODE void Video::PC_decodeSpc(const uint8 *src, int w, int h, uint8 *dst) {
    unpackNibbles(src, w * h / 2, dst);
}

ITCM_CODE static void AMIGA_blit3pNxN(uint8 *dst, int pitch, int w, int h, const uint8 *src) {
    const int planarSize = w * 2 * h;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            for (int i = 0; i < 16; ++i) {
                int color = 0;
                const int mask = 1 << (15 - i);
                for (int bit = 0; bit < 3; ++bit) {
                    if (READ_BE_UINT16(src + bit * planarSize) & mask) {
                        color |= 1 << bit;
                    }
                }
                dst[x * 16 + i] = color;
            }
            src += 2;
        }
        dst += pitch;
    }
}

ITCM_CODE static void AMIGA_blit4p16xN(uint8 *dst, int w, int h, const uint8 *src) {
    const int planarSize = w * 2 * h;
    assert(w == 1);
    for (int y = 0; y < h; ++y) {
        for (int i = 0; i < 16; ++i) {
            int color = 0;
            const int mask = 1 << (15 - i);
            for (int bit = 0; bit < 4; ++bit) {
                if (READ_BE_UINT16(src + bit * planarSize) & mask) {
                    color |= 1 << bit;
                }
            }
            dst[i] = color;
        }
        src += 2;
        dst += 16;
    }
}

ITCM_CODE static void AMIGA_blit4p8xN(uint8 *dst, int w, int h, const uint8 *src) {
    assert(w == 8);
    for (int y = 0; y < h; ++y) {
        for (int i = 0; i < 8; ++i) {
            int color = 0;
            const int mask = 1 << (7 - i);
            for (int bit = 0; bit < 4; ++bit) {
                if (src[bit] & mask) {
                    color |= 1 << bit;
                }
            }
            dst[i] = color;
        }
        src += 4;
        dst += w;
    }
}

ITCM_CODE static void AMIGA_blit4pNxN(uint8 *dst, int w, int h, const uint8 *src) {
    const int planarSize = w / 8 * h;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w / 8; ++x) {
                    for (int i = 0; i < 8; ++i) {
                int color = 0;
                const int mask = 1 << (7 - i);
                for (int bit = 0; bit < 4; ++bit) {
                    if (src[bit * planarSize] & mask) {
                        color |= 1 << bit;
                    }
                }
                dst[x * 8 + i] = color;
            }
            ++src;
        }
        dst += w;
    }
}

ITCM_CODE static void AMIGA_blit4pNxN_mask(uint8 *dst, int x0, int y0, int w, int h, uint8 *src, uint8 *mask, int size) {
    dst += y0 * 256 + x0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w * 2; ++x) {
            for (int i = 0; i < 8; ++i) {
                const int c_mask = 1 << (7 - i);
                int color = 0;
                for (int j = 0; j < 4; ++j) {
                    if (mask[j * size] & c_mask) {
                        color |= 1 << j;
                    }
                }
                if (*src & c_mask) {
                    const int px = x0 + 8 * x + i;
                    const int py = y0 + y;
                    if (px >= 0 && px < 256 && py >= 0 && py < 224) {
                        dst[8 * x + i] = color;
                    }
                }
            }
            ++src;
            ++mask;
        }
        dst += 256;
    }
}

ITCM_CODE static void AMIGA_decodeRLE(uint8 *dst, const uint8 *src) {
    int code = READ_BE_UINT16(src) & 0x7FFF; src += 2;
    const uint8 *end = src + code;
    do {
        code = *src++;
        if ((code & 0x80) == 0) {
            ++code;
            memcpy(dst, src, code);
            src += code;
        } else {
            code = 1 - ((int8)code);
            memset(dst, *src, code);
            ++src;
        }
        dst += code;
    } while (src < end);
    assert(src == end);
}

ITCM_CODE static void AMIGA_decodeSgd(uint8 *dst, const uint8 *src, const uint8 *data) {
    int num = -1;
    uint8 buf[256 * 32];
    int count = READ_BE_UINT16(src) - 1; src += 2;
    do {
        int d2 = READ_BE_UINT16(src); src += 2;
        int d0 = READ_BE_UINT16(src); src += 2;
        int d1 = READ_BE_UINT16(src); src += 2;
        if (d2 != 0xFFFF) {
            d2 &= ~(1 << 15);
            const int offset = READ_BE_UINT32(data + d2 * 4);
            if (offset < 0) {
                const uint8 *ptr = data - offset;
                const int size = READ_BE_UINT16(ptr); ptr += 2;
                if (num != d2) {
                    num = d2;
                    assert(size <= (int)sizeof(buf));
                    memcpy(buf, ptr, size);
                }
                        } else {
                if (num != d2) {
                    num = d2;
                    const int size = READ_BE_UINT16(data + offset) & 0x7FFF;
                    (void)size;
                    assert(size <= (int)sizeof(buf));
                    AMIGA_decodeRLE(buf, data + offset);
                }
            }
        }
        const int w = (buf[0] + 1) >> 1;
        const int h = buf[1] + 1;
        const int planarSize = READ_BE_UINT16(buf + 2);
        AMIGA_blit4pNxN_mask(dst, (int16)d0, (int16)d1, w, h, buf + 4, buf + 4 + planarSize, planarSize);
    } while (--count >= 0);
}

ITCM_CODE static const uint8 *AMIGA_mirrorY(const uint8 *a2) {
    static uint8 buf[32];

        a2 += 24;
    for (int j = 0; j < 4; ++j) {
        for (int i = 0; i < 8; ++i) {
            buf[31 - j * 8 - i] = *a2++;
        }
        a2 -= 16;
    }
    return buf;
}

ITCM_CODE static const uint8 *AMIGA_mirrorX(const uint8 *a2) {
    static uint8 buf[32];

    for (int i = 0; i < 32; ++i) {
        uint8 mask = 0;
        for (int bit = 0; bit < 8; ++bit) {
            if (a2[i] & (1 << bit)) {
                mask |= 1 << (7 - bit);
            }
        }
        buf[i] = mask;
    }
    return buf;
}

static void AMIGA_blit4p8x8(uint8 *dst, int pitch, const uint8 *src, int pal, int colorKey = -1) {
    for (int y = 0; y < 8; ++y) {
        for (int i = 0; i < 8; ++i) {
            const int mask = 1 << (7 - i);
            int color = 0;
            for (int bit = 0; bit < 4; ++bit) {
                if (src[8 * bit] & mask) {
                    color |= 1 << bit;
                }
            }
            if (color != colorKey) {
                dst[i] = pal + color;
            }
        }
        ++src;
        dst += pitch;
    }
}

ITCM_CODE static void AMIGA_decodeLevHelper(uint8 *dst, const uint8 *src, int offset10, int offset12, const uint8 *a5, bool sgdBuf) {
    if (offset10 != 0) {
        const uint8 *a0 = src + offset10;
        for (int y = 0; y < 224; y += 8) {
            for (int x = 0; x < 256; x += 8) {
                const int d3 = READ_BE_UINT16(a0); a0 += 2;
                const int d0 = d3 & 0x7FF;
                if (d0 != 0) {
                    const uint8 *a2 = a5 + d0 * 32;
                    if ((d3 & (1 << 12)) != 0) {
                        a2 = AMIGA_mirrorY(a2);
                    }
                    if ((d3 & (1 << 11)) != 0) {
                        a2 = AMIGA_mirrorX(a2);
                    }
                    int mask = 0;
                    if ((d3 < (1 << 15)) == 0) {
                        mask = 0x80;
                    }
                    AMIGA_blit4p8x8(dst + y * 256 + x, 256, a2, mask);
                }
            }
        }
    }
    if (offset12 != 0) {
        const uint8 *a0 = src + offset12;
        for (int y = 0; y < 224; y += 8) {
            for (int x = 0; x < 256; x += 8) {
                int d3 = READ_BE_UINT16(a0); a0 += 2;
                int d0 = d3 & 0x7FF;
                if (d0 != 0 && sgdBuf) {
                    d0 -= 896;
                }
                if (d0 != 0) {
                    const uint8 *a2 = a5 + d0 * 32;
                    if ((d3 & (1 << 12)) != 0) {
                        a2 = AMIGA_mirrorY(a2);
                    }
                    if ((d3 & (1 << 11)) != 0) {
                        a2 = AMIGA_mirrorX(a2);
                    }
                    int mask = 0;
                    if ((d3 & 0x6000) != 0 && sgdBuf) {
                        mask = 0x10;
                    } else if ((d3 < (1 << 15)) == 0) {
                        mask = 0x80;
                    }
                    AMIGA_blit4p8x8(dst + y * 256 + x, 256, a2, mask, 0);
                }
            }
        }
    }
}

ITCM_CODE void Video::AMIGA_decodeLev(int level, int room) {
    uint8 *tmp = _res->_memBuf;
    const int offset = READ_BE_UINT32(_res->_lev + room * 4);
    if (!delphine_unpack(tmp, _res->_lev, offset)) {
        error("Bad CRC for level %d room %d", level, room);
    }
    uint16 offset10 = READ_BE_UINT16(tmp + 10);
    const uint16 offset12 = READ_BE_UINT16(tmp + 12);
    const uint16 offset14 = READ_BE_UINT16(tmp + 14);
    static const int kTempMbkSize = 1024;
    uint8 *buf = (uint8 *)malloc(kTempMbkSize * 32);
    if (!buf) {
        error("Unable to allocate mbk temporary buffer");
    }
    int sz = 0;
    memset(buf, 0, 32);
    sz += 32;
    const uint8 *a1 = tmp + offset14;
    for (bool loop = true; loop;) {
        int d0 = READ_BE_UINT16(a1); a1 += 2;
        if (d0 & 0x8000) {
            d0 &= ~0x8000;
            loop = false;
        }
        const int d1 = _res->getBankDataSize(d0);
        const uint8 *a6 = _res->findBankData(d0);
        if (!a6) {
            a6 = _res->loadBankData(d0);
        }
        const int d3 = *a1++;
        if (d3 == 255) {
            assert(sz + d1 < kTempMbkSize * 32);
            memcpy(buf + sz, a6, d1);
            sz += d1;
        } else {
            for (int i = 0; i < d3 + 1; ++i) {
                const int d4 = *a1++;
                assert(sz + 32 < kTempMbkSize * 32);
                memcpy(buf + sz, a6 + d4 * 32, 32);
                sz += 32;
            }
        }
    }
    memset(_frontLayer, 0, Video::GAMESCREEN_W * Video::GAMESCREEN_H);
    if (tmp[1] != 0) {
        assert(_res->_sgd);
        AMIGA_decodeSgd(_frontLayer, tmp + offset10, _res->_sgd);
        offset10 = 0;
    }
    AMIGA_decodeLevHelper(_frontLayer, tmp, offset10, offset12, buf, tmp[1] != 0);
    memcpy(_backLayer, _frontLayer, Video::GAMESCREEN_W * Video::GAMESCREEN_H);
    uint16 num[4];
    for (int i = 0; i < 4; ++i) {
        num[i] = READ_BE_UINT16(tmp + 2 + i * 2);
    }
    _mapPalSlot1 = num[1];
    _mapPalSlot2 = num[2];
    setPaletteSlotBE(0x0, num[0]);
    for (int i = 1; i < 5; ++i) {
        setPaletteSlotBE(i, _mapPalSlot2);
    }
    setPaletteSlotBE(0x6, _mapPalSlot2);
    setPaletteSlotBE(0x8, num[0]);
    setPaletteSlotBE(0xA, _mapPalSlot2);
}

ITCM_CODE void Video::AMIGA_decodeSpm(const uint8 *src, uint8 *dst) {
    uint8 buf[256 * 32];
    const int size = READ_BE_UINT16(src + 3) & 0x7FFF;
    (void)size;
    assert(size <= (int)sizeof(buf));
    AMIGA_decodeRLE(buf, src + 3);
    const int w = (src[2] >> 7) + 1;
    const int h = src[2] & 0x7F;
    AMIGA_blit3pNxN(dst, w * 16, w, h, buf);
}

ITCM_CODE void Video::AMIGA_decodeIcn(const uint8 *src, int num, uint8 *dst) {
    for (int i = 0; i < num; ++i) {
        const int h = 1 + *src++;
        const int w = 1 + *src++;
        const int size = w * h * 8;
        src += 4 + size;
    }
    const int h = 1 + *src++;
    const int w = 1 + *src++;
    AMIGA_blit4p16xN(dst, w, h, src + 4);
}

ITCM_CODE void Video::AMIGA_decodeSpc(const uint8 *src, int w, int h, uint8 *dst) {
    switch (w) {
    case 8:
        AMIGA_blit4p8xN(dst, w, h, src);
        break;
    default:
        AMIGA_blit4pNxN(dst, w, h, src);
        break;
    }
}

void ITCM_CODE Video::drawSpriteSub1(const uint8 *src, uint8 *dst, int pitch, int h, int w, uint8 colMask) {
    while (h--) {
        rowFwd<false>(src, dst, w, colMask);
        src += pitch;
        dst += 256;
    }
}

void ITCM_CODE Video::drawSpriteSub2(const uint8 *src, uint8 *dst, int pitch, int h, int w, uint8 colMask) {
    while (h--) {
        rowRev<false>(src, dst, w, colMask);
        src += pitch;
        dst += 256;
    }
}

void ITCM_CODE Video::drawSpriteSub3(const uint8 *src, uint8 *dst, int pitch, int h, int w, uint8 colMask) {
    while (h--) {
        rowFwd<true>(src, dst, w, colMask);
        src += pitch;
        dst += 256;
    }
}

void ITCM_CODE Video::drawSpriteSub4(const uint8 *src, uint8 *dst, int pitch, int h, int w, uint8 colMask) {
    while (h--) {
        rowRev<true>(src, dst, w, colMask);
        src += pitch;
        dst += 256;
    }
}

void ITCM_CODE Video::drawSpriteSub5(const uint8 *src, uint8 *dst, int pitch, int h, int w, uint8 colMask) {
    while (h--) {
        rowStride<true>(src, pitch, dst, w, colMask);
        ++src;
        dst += 256;
    }
}

void ITCM_CODE Video::drawSpriteSub6(const uint8 *src, uint8 *dst, int pitch, int h, int w, uint8 colMask) {
    while (h--) {
        rowStride<true>(src, -pitch, dst, w, colMask);
        ++src;
        dst += 256;
    }
}

ITCM_CODE void Video::PC_drawChar(uint8 c, int16 y, int16 x) {
    debug(DBG_VIDEO, "Video::PC_drawChar(0x%X, %d, %d)", c, y, x);
    y *= 8;
    x *= 8;
    const uint8 *src = _res->_fnt + (c - 32) * 32;
    uint8 *dst = _frontLayer + x + 256 * y;
    for (int h = 0; h < 8; ++h) {
        for (int i = 0; i < 4; ++i) {
            uint8 c1 = (*src & 0xF0) >> 4;
            uint8 c2 = (*src & 0x0F) >> 0;
            ++src;

            if (c1 != 0) {
                if (c1 != 2) {
                    *dst = _charFrontColor;
                } else {
                    *dst = _charShadowColor;
                }
            } else if (_charTransparentColor != 0xFF) {
                *dst = _charTransparentColor;
            }
            ++dst;

            if (c2 != 0) {
                if (c2 != 2) {
                    *dst = _charFrontColor;
                } else {
                    *dst = _charShadowColor;
                }
            } else if (_charTransparentColor != 0xFF) {
                *dst = _charTransparentColor;
            }
            ++dst;
        }
        dst += 256 - 8;
    }
}

ITCM_CODE void Video::AMIGA_drawStringChar(uint8 *dst, int pitch, const uint8 *src, uint8 color, uint8 chr) {
    assert(chr >= 32);
    AMIGA_decodeIcn(src, chr - 32, _res->_memBuf);
    src = _res->_memBuf;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            if (src[x] != 0) {
                dst[x] = 0x1D;
            }
        }
        src += 16;
        dst += pitch;
    }
}

ITCM_CODE void Video::PC_drawStringChar(uint8 *dst, int pitch, const uint8 *src, uint8 color, uint8 chr) {
    assert(chr >= 32);
    src += (chr - 32) * 8 * 4;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 4; ++x) {
            const uint8 c1 = src[x] >> 4;
            if (c1 != 0) {
                *dst = (c1 == 15) ? color : (0xE0 + c1);
            }
            ++dst;
            const uint8 c2 = src[x] & 15;
            if (c2 != 0) {
                *dst = (c2 == 15) ? color : (0xE0 + c2);
            }
            ++dst;
        }
        src += 4;
        dst += pitch - CHAR_W;
    }
}

ITCM_CODE const char *Video::drawString(const char *str, int16 x, int16 y, uint8 col) {
    debug(DBG_VIDEO, "Video::drawString('%s', %d, %d, 0x%X)", str, x, y, col);
    void (Video::*drawCharFunc)(uint8 *, int, const uint8 *, uint8, uint8) = 0;
    switch (_res->_type) {
    case kResourceTypeAmiga:
        drawCharFunc = &Video::AMIGA_drawStringChar;
        break;
    case kResourceTypePC:
        drawCharFunc = &Video::PC_drawStringChar;
        break;
    }
    int len = 0;
    uint8 *dst = _frontLayer + y * 256 + x;
    while (1) {
        const uint8 c = *str++;
        if (c == 0 || c == 0xB || c == 0xA) {
            break;
        }
        (this->*drawCharFunc)(dst, 256, _res->_fnt, col, c);
        dst += CHAR_W;
        ++len;
    }
    markBlockAsDirty(x, y, len * 8, 8);
    return str - 1;
}
