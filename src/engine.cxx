#include <cstdio>

#include "hardware/interp.h"
#include "hardware/divider.h"

#include "engine.h"
#include "audio.h"
#include "envelope.h"
#include "midi.h"
#include "waves.h"

extern int16_t power_table[];

//--------------------------------------------------------------------+
// Per-voice state
//--------------------------------------------------------------------+

void Voice::init()
{
	free = true;
	steal = false;
	channel = nullptr;
	patch = nullptr;
	dca_env = nullptr;
	dco_env = nullptr;
}

Voice::Voice()
{
	init();
}

void Voice::update(int16_t* samples, size_t n)
{
	// copy voice state to the interpolator
	interp0->base[0] = step;
	interp0->base[2] = (uint32_t)waves[patch->wavenum];
	interp0->accum[0] = pos;

	// generate the samples
	for (uint i = 0; i < n; ++i) {
		samples[i] = *(int16_t*)interp0->pop[2];
	}

	// update voice state
	pos = interp0->accum[0] & (wave_max - 1);
}

void Voice::note_on(uint8_t _chan, uint8_t _note, uint8_t _vel)
{
	extern uint32_t note_table[];

	// remember note parameters
	note = _note;
	vel = _vel;

	// load the current patch parameters
	auto& p = *patch;

	// set up the DCA envelope
	dca_env = new ADSR(p.dca_env_a, p.dca_env_d, p.dca_env_s, p.dca_env_r);
	dca_env->gate_on();

	// set up the DCO envelope
	if (p.dco_env_level) {
		dco_env = new ADSR(p.dco_env_a, p.dco_env_d, p.dco_env_s, p.dco_env_r);
		dco_env->gate_on();
	}

	// setup DCO
	step_base = note_table[note];
	pos = 0;
}

void Voice::note_off()
{
	dca_env->gate_off();

	if (dco_env) {
		dco_env->gate_off();
	}

	steal = true;		// voice may now be stolen
}

//--------------------------------------------------------------------+
// Core synth engine
//--------------------------------------------------------------------+

SynthEngine::SynthEngine()
{
	// all voices start out unused
	for (auto& v : voice) {
		v.init();
	}

	// set all channels to a default preset
	for (uint8_t c = 0; c < 16; ++c) {
		midi_in(0xc0 + c, c, 0);
	}
}

void SynthEngine::deallocate(Voice& v)
{
	delete v.dca_env;
	delete v.dco_env;
	v.init();
}

Voice* SynthEngine::allocate()
{
	// look for a spare voice
	for (auto& v: voice) {
		if (v.free) {
			v.free = false;
			v.steal = false;
			return &v;
		}
	}

	// none-found, we need to steal
	for (auto& v: voice) {
		if (v.steal) {
			deallocate(v);
			return &v;
		}
	}

	return nullptr;
}

// temporary buffer of mono samples
static int16_t mono[BUFFER_SIZE];

uint8_t __not_in_flash_func(SynthEngine::update)(int32_t* samples, size_t n)
{
	uint8_t active = 0;

	// update all envelopes and release any voice
	// that now has an inactive DCA
	for (auto& v: voice) {
		if (v.free) continue;

		v.dca_env->update();
		if (!v.dca_env->active()) {
			deallocate(v);
			continue;
		}

		// get a reference to the current note's patch
		auto& p = *v.patch;

		// update DCO envelope
		if (p.dco_env_level && v.dco_env) {
			v.dco_env->update();
		}
	}

	// set up the interpolator
	interp_config cfg = interp_default_config();
	interp_config_set_shift(&cfg, 15);
	interp_config_set_mask(&cfg, 1, wave_shift);
	interp_config_set_add_raw(&cfg, true);
	interp_set_config(interp0, 0, &cfg);

	for (auto& v : voice) {

		// voice not in use
		if (v.free) continue;

		// get a reference to the channel parameters
		assert(v.chan < 0x10);
		auto& chan = *v.channel;

		// and a reference to the current note's patch
		auto& p = *v.patch;

		// get the 15-bit DCA current envelope level
		uint32_t dca = v.dca_env->level();		// 15 bits

		// scale the DCA by the patch's 7-bit DCA master level
		dca *= p.dca_env_level;					// 22 bits

		// scale the DCA by the 7-bit note velocity
		dca *= v.vel;							// 29 bits

		// scale the DCA by the 7-bit channel volume
		dca >>= 7;								// 22 bits
		dca *= chan.control[volume];			// 29 bits
		dca >>= 4;								// 25 bits

		// apply 7-bit pan and scale back to 16 bits
		uint16_t level_l = (dca * chan.pan_l) >> 16;
		uint16_t level_r = (dca * chan.pan_r) >> 16;

		// scale the DCO step by the current pitchbend amount
		v.step = v.step_base;
		if (chan.bend) {
			v.step = ((uint64_t)v.step * chan.bend_f) >> 15;
		}

		// apply the DCO envelope
		if (v.dco_env && p.dco_env_level) {
			int32_t env = v.dco_env->level();	// 16 bits
			if (true || env) {
				env = env * p.dco_env_level;	// 24 bits
				env = (env >> 10) + 8192;		// 14 bits
				v.step = ((uint64_t)v.step * power_table[env]) >> 15;
			}
		}

		// generate a buffer full of (mono) samples
		v.update(mono, n);

		// TODO: apply filters here

		// count active voices
		++active;

		// don't bother accumulating silent channels
		if (!dca) continue;

		// accumulate the samples into the supplied output buffer
		for (size_t i = 0, j = 0; i < n; ++i) {
			samples[j++] += (level_l * mono[i]) >> 16;
			samples[j++] += (level_r * mono[i]) >> 16;
		}
	}

	return active;
}

void SynthEngine::note_on(uint8_t chan, uint8_t note, uint8_t vel)
{
	auto* vp = allocate();
	if (vp) {
		auto& v = *vp;
		v.channel = &channel[chan];
		v.patch = &presets[v.channel->program % 4];
		v.note_on(chan, note, vel);
	}
}

void SynthEngine::note_off(uint8_t chan, uint8_t note, uint8_t vel)
{
	Channel* c = &channel[chan];
	for (auto& v: voice) {
		if (v.free) continue;
		if (v.channel == c && v.note == note) {
			v.note_off();
		}
	}
}

void SynthEngine::midi_in(uint8_t c, uint8_t d1, uint8_t d2)
{
	uint8_t cmd = (c & 0xf0) >> 4;
	uint8_t chan = (c & 0x0f);

	switch (cmd) {
		case 0x8:
			note_off(chan, d1, d2);
			break;
		case 0x9:
			if (d2) {
				note_on(chan, d1, d2);
			} else {
				note_off(chan, d1, d2);
			}
			break;
		case 0xb:
		case 0xc:
		case 0xd:
		case 0xe:
			channel[chan].midi_in(c, d1, d2);
			break;
		case 0xf:
			break;
	}
}
