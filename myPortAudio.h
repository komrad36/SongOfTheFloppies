/*******************************************************************
*   myPortAudio.h
*   SongOfTheFloppies
*	Kareem Omar
*
*	6/18/2015
*   This program is entirely my own work.
*******************************************************************/

// This module provides a Windows- and Linux-compatible interface
// between simple "note on" and "note off" commands of the MIDI variety
// into low-level handling of the resultant sine waves
// using the portAudio library

#ifndef MYPORTAUDIO_H
#define MYPORTAUDIO_H

#define _USE_MATH_DEFINES

#define TABLE_SIZE						(100000)
#define SAMPLE_RATE						(44100.0)
#define AUTO_FRAMES_PER_BUFFER			(0)

// growth and decay constants
// are to allow fade-in and -out
// of sine waves to prevent pops
#define GROWTH_FACTOR					(1.01f)
#define SHRINK_FACTOR					(0.997f)
#define INITIAL_DECAY_STATE				(0.006f)

#define MAX_DECAY_STATE					(1.0f)
#define MIN_DECAY_STATE					(0.001f)

#define MAX_SIMUL						(200)
#define ENABLE_REALTIME_SCHEDULING		(1)
#define MS_TO_WAIT_AFTER_STREAM_LAUNCH	(500)

#define MAX_VEL (127.0f)

// can be set higher than actual max (127.0)
// to produce quieter output
// and prevent pops/clipping
#define OVERHEAD_MAX (1500.0f)

#include <cstdint>
#include <iostream>
#include <math.h>
#include <thread>
#include <vector>

#include "portaudio.h"

#ifdef __linux__
#include "pa_linux_alsa.h"
#endif

// data passed to audio callback
struct paData {

	float sine[TABLE_SIZE];

	float currentDecayState[MAX_SIMUL];
	float decayFactor[MAX_SIMUL];
	unsigned long phase[MAX_SIMUL];

	float normalizedVel[MAX_SIMUL];
	unsigned long phaseIncrement[MAX_SIMUL];

};

class Stream {
private:
	PaStreamParameters outputParameters;
	PaStream *stream;
	PaError err;
	paData data;

	uint8_t noteVel[MAX_SIMUL];
	uint8_t channelVel[MAX_SIMUL];
	uint8_t channelExpression[MAX_SIMUL];

	double noteFreq[MAX_SIMUL];
	double pitchBend[MAX_SIMUL];

public:

	bool streamInitialized;

	Stream();
	~Stream();


	void initSineTable();
	PaStream* getStream();
	void setFreqs(uint16_t idx, double freq, double pitchBend);
	void setChannelVel(uint16_t idx, uint8_t channelVel);
	void setChannelExpression(uint16_t idx, uint8_t channelExpression);
	void setVels(uint16_t idx, uint8_t channelExpression, uint8_t channelVel, uint8_t noteVel);
	void setPitchBend(uint16_t idx, double pitchBend);
	void startAudio(uint16_t idx);
	void stopAudio(uint16_t idx);

};

#endif
