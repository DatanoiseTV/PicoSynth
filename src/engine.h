#pragma once

#include <cstdint>
#include <cstddef>

#include "channel.h"
#include "waves.h"

class Envelope;

class Voice {

	friend class			SynthEngine;

private:
	uint8_t					chan;
	bool					free;
	bool					steal;
	uint8_t					note;
	uint8_t					vel;
	uint32_t				step;
	uint32_t				pos;
	int16_t*				wavetable;
	Envelope*				dca;

private:
	void					init();
	void					update(int16_t* samples, size_t n);
	void					note_on(uint8_t chan, uint8_t note, uint8_t vel);
	void					note_off();

public:
							Voice();

};

class SynthEngine {

private:
	static const uint8_t	nv = 64;
	Voice					voice[nv];
	Channel					channel[16];

private:
	Voice*					allocate();
	void					deallocate(Voice& v);

	void					note_on(uint8_t chan, uint8_t note, uint8_t vel);
	void					note_off(uint8_t chan, uint8_t note, uint8_t vel);

public:
	void					midi_in(uint8_t c, uint8_t d1, uint8_t d2);

public:
	void					update(int32_t* samples, size_t n);

public:
							SynthEngine();
};
