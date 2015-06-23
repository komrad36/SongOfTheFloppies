/*******************************************************************
*   myPortAudio.cpp
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

#include "myPortAudio.h"

static int patestCallback(const void *inputBuffer, void *outputBuffer,
	unsigned long framesPerBuffer,
	const PaStreamCallbackTimeInfo* timeInfo,
	PaStreamCallbackFlags statusFlags,
	void *userData)
{
	paData *data = (paData*)userData;
	float *out = (float*)outputBuffer;
	float cumulativeOut;

	// silence compiler warnings about unused vars
	(void)timeInfo;
	(void)statusFlags;
	(void)inputBuffer;

	// for each frame to construct
	for (unsigned long i = 0; i < framesPerBuffer; ++i) {
		cumulativeOut = 0.0f;
		// for each possibly playing stream
		for (unsigned long j = 0; j < MAX_SIMUL; ++j) {
			// if the stream is active
			if (data->currentDecayState[j] > MIN_DECAY_STATE) {
				// update its decay state
				data->currentDecayState[j] *= data->decayFactor[j];
				// cap the decay state at MAX if it's larger than that
				if (data->currentDecayState[j] > MAX_DECAY_STATE) data->currentDecayState[j] = MAX_DECAY_STATE;

				// add this stream's current amplitude to the total current wave position
				cumulativeOut += data->currentDecayState[j] * data->normalizedVel[j] * data->sine[data->phase[j]];

				// increment this stream's phase
				data->phase[j] += data->phaseIncrement[j];

				// wrap the table (single period of sine wave)
				if (data->phase[j] >= TABLE_SIZE) data->phase[j] -= TABLE_SIZE;
			}
		}

		// one for each channel (ear) - interleaved output
		*out++ = cumulativeOut;
		*out++ = cumulativeOut;

	}
	return paContinue;
}

Stream::~Stream() {
	Pa_StopStream(stream);
	Pa_CloseStream(stream);
	Pa_Terminate();
}

void Stream::startAudio(uint16_t idx) {
	data.phase[idx] = 0;
	data.currentDecayState[idx] = INITIAL_DECAY_STATE;
	data.decayFactor[idx] = GROWTH_FACTOR;
}

// don't actually stop audio (would pop)
// instead let the stream start shrinking in
// amplitude naturally
void Stream::stopAudio(uint16_t idx) {
	data.decayFactor[idx] = SHRINK_FACTOR;
}

void Stream::setPitchBend(uint16_t idx, double pitchBend) {
	this->pitchBend[idx] = pitchBend;
	data.phaseIncrement[idx] = (unsigned long)(noteFreq[idx] * pitchBend * TABLE_SIZE / ((double)SAMPLE_RATE) + 0.5);
}

void Stream::setFreqs(uint16_t idx, double freq, double pitchBend) {
	this->noteFreq[idx]			= freq;
	this->pitchBend[idx]		= pitchBend;
	data.phaseIncrement[idx]	= (unsigned long)(freq * pitchBend * TABLE_SIZE / ((double)SAMPLE_RATE) + 0.5);
}

void Stream::setChannelExpression(uint16_t idx, uint8_t channelExpression) {
	this->channelExpression[idx] = channelExpression;
	data.normalizedVel[idx] = (float)noteVel[idx] / OVERHEAD_MAX * (float)channelVel[idx] / MAX_VEL * (float)channelExpression / MAX_VEL;
}

void Stream::setChannelVel(uint16_t idx, uint8_t channelVel) {
	this->channelVel[idx]	= channelVel;
	data.normalizedVel[idx] = (float)noteVel[idx] / OVERHEAD_MAX * (float)channelVel / MAX_VEL * (float)channelExpression[idx] / MAX_VEL;
}

void Stream::setVels(uint16_t idx, uint8_t channelExpression, uint8_t channelVel, uint8_t noteVel) {
	this->channelExpression[idx]	= channelExpression;
	this->channelVel[idx]			= channelVel;
	this->noteVel[idx]				= noteVel;
	data.normalizedVel[idx]			= (float)noteVel / OVERHEAD_MAX * (float)channelVel / MAX_VEL * (float)channelExpression / MAX_VEL;
}

// high-resolution table of single, complete sine period
void Stream::initSineTable() {
	for (size_t i = 0; i < TABLE_SIZE; ++i) {
		data.sine[i] = (float)sin(((double)i / (double)TABLE_SIZE) * M_PI * 2.0);
	}
}

Stream::Stream() {
	err = Pa_Initialize();
	if (err != paNoError) goto error;

#ifdef WIN32
	outputParameters.device = Pa_GetHostApiInfo(Pa_HostApiTypeIdToHostApiIndex(PaHostApiTypeId::paASIO))->defaultOutputDevice;
#else
	outputParameters.device = Pa_GetDefaultOutputDevice();
#endif

	if (outputParameters.device == paNoDevice)
		goto error;

	outputParameters.channelCount = 2;
	outputParameters.sampleFormat = paFloat32;
	outputParameters.suggestedLatency = Pa_GetDeviceInfo(outputParameters.device)->defaultLowOutputLatency;
	outputParameters.hostApiSpecificStreamInfo = NULL;

	err = Pa_OpenStream(
		&stream,
		NULL,
		&outputParameters,
		SAMPLE_RATE,
		AUTO_FRAMES_PER_BUFFER,
		paClipOff,
		patestCallback,
		&data);
	if (err != paNoError) goto error;

	// decrease pops on linux
#ifdef __linux__
	PaAlsa_EnableRealtimeScheduling(stream, ENABLE_REALTIME_SCHEDULING);
#endif

	Pa_StartStream(stream);
	if (err != paNoError) goto error;

	streamInitialized = true;

	// sleep to let asio/alsa initialize
	std::this_thread::sleep_for(std::chrono::milliseconds(MS_TO_WAIT_AFTER_STREAM_LAUNCH));

	return;

error:
	streamInitialized = false;
	Pa_Terminate();
	std::cout << "PortAudio error:" << std::endl;
	std::cout << "Error code: " << err << std::endl;
	std::cout << "Error message: " << Pa_GetErrorText(err) << std::endl;

	if ((err = paUnanticipatedHostError)) {
		const PaHostErrorInfo* hostErrorInfo = Pa_GetLastHostErrorInfo();
		std::cout << "Error code: " << hostErrorInfo->errorCode << std::endl;
		std::cout << "Error message: " << hostErrorInfo->errorText << std::endl;
	}
}

PaStream* Stream::getStream() {
	return stream;
}
