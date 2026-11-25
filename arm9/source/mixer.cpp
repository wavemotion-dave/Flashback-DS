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

#include <nds.h>

#include "mixer.h"
#include "systemstub.h"


Mixer::Mixer(SystemStub *stub)
    : _stub(stub) {
}

void Mixer::init() {
    for (auto &ch : _channels) {
        ch = {};
    }
    _premixHook = 0;
    _stub->startAudio(Mixer::mixCallback, this);
}

void Mixer::free() {
    stopAll();
    _stub->stopAudio();
}

void Mixer::setPremixHook(PremixHook premixHook, void *userData) {
    debug(DBG_SND, "Mixer::setPremixHook()");
    LockAudioStack las(_stub);
    _premixHook = premixHook;
    _premixHookData = userData;
}

ITCM_CODE void Mixer::play(const MixerChunk *mc, uint16 freq, uint8 volume) {
    debug(DBG_SND, "Mixer::play(%d, %d)", freq, volume);
    LockAudioStack las(_stub);
    MixerChannel *ch = 0;
    for (int i = 0; i < NUM_CHANNELS; ++i) {
        MixerChannel *cur = &_channels[i];
        if (cur->active) {
            if (cur->chunk.data == mc->data) {
                cur->chunkPos = 0;
                return;
            }
        } else {
            ch = cur;
            break;
        }
    }
    if (ch) {
        ch->active = true;
        ch->volume = volume;
        ch->chunk = *mc;
        ch->chunkPos = 0;
        ch->chunkInc = (freq << FRAC_BITS) / _stub->getOutputSampleRate();
    }
}

uint32 Mixer::getSampleRate() const {
    return _stub->getOutputSampleRate();
}

void Mixer::stopAll() {
    debug(DBG_SND, "Mixer::stopAll()");
    LockAudioStack las(_stub);
    for (uint8 i = 0; i < NUM_CHANNELS; ++i) {
        _channels[i].active = false;
    }
}

ITCM_CODE void Mixer::mix(int8 *buf, int len) {
    static int32 acc[1024];            // >= stream buffer_length
    if (len > 1024) len = 1024;

    // Volume is applied with a shift below, so this must stay a power of two (64 -> >> 6).
    static_assert(Mixer::MAX_VOLUME == 64, "update VOL_SHIFT in Mixer::mix");
    const int VOL_SHIFT = 6;
    const uint32 fracMask = (1u << FRAC_BITS) - 1;

    memset(buf, 0, len);
    if (_premixHook) {
        if (!_premixHook(_premixHookData, buf, len)) {
            _premixHook = 0;
            _premixHookData = 0;
        }
    }
    for (int i = 0; i < len; ++i) acc[i] = buf[i];

    for (uint8 c = 0; c < NUM_CHANNELS; ++c) {
        MixerChannel *ch = &_channels[c];
        if (!ch->active) continue;

        const int8 *data = (const int8 *)ch->chunk.data;
        const uint32 limit = (ch->chunk.len - 1) << FRAC_BITS;   // stop when pos reaches this
        uint32 pos = ch->chunkPos;
        const uint32 inc = ch->chunkInc;
        const int vol = ch->volume;

        // Work out up front how many samples are left in the chunk,
        // so the inner loop has no end-of-chunk test.
        int n = len;
        if (pos >= limit) {
            n = 0;
        } else if (inc != 0) {
            uint32 left = (limit - pos + inc - 1) / inc;          // one division per channel
            if (left < (uint32)n) n = (int)left;
        }

        int32 *out = acc;
        for (int i = 0; i < n; ++i) {
            const int8 *p = data + (pos >> FRAC_BITS);
            const int s0 = p[0];
            const int s1 = p[1];
            const int s = s0 + (((s1 - s0) * (int)(pos & fracMask)) >> FRAC_BITS);
            *out++ += (s * vol) >> VOL_SHIFT;
            pos += inc;
        }

        ch->chunkPos = pos;
        if (n < len) ch->active = false;
    }

    for (int i = 0; i < len; ++i) {
        int v = acc[i];
        buf[i] = (int8)(v < -128 ? -128 : (v > 127 ? 127 : v));
    }
}

void Mixer::addclamp(int8& a, int b) {
    int add = a + b;
    if (add < -128) {
        add = -128;
    } else if (add > 127) {
        add = 127;
    }
    a = add;
}

void Mixer::mixCallback(void *param, uint8 *buf, int len) {
    ((Mixer *)param)->mix((int8 *)buf, len);
}
