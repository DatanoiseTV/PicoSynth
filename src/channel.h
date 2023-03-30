#pragma once

#include <cstdint>

class Channel {

	friend class			SynthEngine;

private:
	static bool				init;
	static uint8_t			pan_table[128];

private:
	void					update_cc(uint8_t cc, uint8_t value);
	void					update_bend(uint8_t lsb, uint8_t msb);

private:					// state mirroring MIDI values
	const uint8_t			bend_range = 2;
	int16_t					bend = 0;
	uint8_t					control[128];
	uint8_t					pressure;
	uint8_t					prognum;

private:					// calculated state
	float					bend_f;
	uint8_t					pan_l;
	uint8_t					pan_r;

public:
							Channel();

public:
	void					midi_in(uint8_t cmd, uint8_t d1, uint8_t d2);

};
