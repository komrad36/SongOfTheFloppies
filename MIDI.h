/*******************************************************************
*   MIDI.h
*   SongOfTheFloppies
*	Kareem Omar
*
*	6/18/2015
*   This program is entirely my own work.
*******************************************************************/

// This module contains code necessary for parsing and playing MIDI files
// on sine waves and floppy drive stepper motors (via serial->Arduino).

#ifndef MIDI_H
#define MIDI_H

// USER-ADJUSTABLE PARAMETERS //

#define PLAY_SINE
//#define PLAY_FLOPPY

#define LOG_NOTES
#define VERBOSE_1
#define VERBOSE_2



// assign drives by order of note appearance in time
// during playback?
#define ASSIGN_CHANNELS_TO_DRIVES_SEQUENTIALLY

#define MAX_DRIVES										(15)

#define MIN_FLOPPY_NOTE									(25)
#define MAX_FLOPPY_NOTE									(57)

#define MAX_PITCH_BEND_SEMITONES						(2.3)

#define NOTE_DOWN_SHIFT_SEMITONES						(12)

#define MS_TO_WAIT_AFTER_CALIBRATION					(2000)
#define MS_TO_WAIT_AFTER_PLAYING						(300)
#define US_TO_WAIT_BETWEEN_ARDUINO_READINESS_CHECKS		(1000)
////////////////////////////////




// BELOW ARE NOT TO BE MODIFIED //
#define CHANNEL_NOT_ASSIGNED							(255)
#define DEFAULT_EXPRESSION								(127)
#define DEFAULT_INSTRUMENT								(1)
#define DEFAULT_VOLUME									(100)
#define DELTA_TIME_WIDTH								(5)
#define EFFECTS_CHANNEL									(10)
#define FREQ_MULTIPLIER									(10000.0)
#define LENGTH_FIELD_LENGTH								(4)
#define MAX_MIDI_FILE_SIZE_IN_BYTES						(2000000)
#define MAX_NOTES										(128)
#define MICROSECONDS_PER_SECOND							(1000000.0)
#define MIDI_STANDARD_DEFAULT_USEC_PER_QTR_NOTE			(500000)
#define MILLISECONDS_PER_SECOND							(1000.0)
#define MIN_FLOPPY_VOLUME								(1000.0f)
#define NANOSECONDS_PER_MILLISECOND						(1000000.0)
#define	NOT_ACTIVE										(65535)
#define NUM_CHANNELS									(16)
#define NUM_SEMITONES_IN_OCTAVE							(12)
#define fNUM_SEMITONES_IN_OCTAVE						(12.0)
#define PACKET_SIZE_BYTES								(4)
#define TAG_LENGTH										(4)
#define VOLUME_NORM										(127.0)

#include <cstdint>
#include <condition_variable>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "myPortAudio.h"
#include "serial.h"

// endian swap a 4-byte unsigned int
#define swapi4(i4) (((i4) >> 24) + (((i4) >> 8) & 65280) + (((i4) & 65280) << 8) + ((i4) << 24))

// endian swap a 2-byte unsigned int
#define swapi2(i2) (((i2) >> 8) + ((i2) << 8))

class BaseMTrkEvent;

// base chunk, derived into either header or track chunk
struct Chunk {
	size_t length;
};

struct HeaderChunk : public Chunk {
	//length is typically 6 bytes exactly (excluding type and length fields)
	uint16_t format;
	uint16_t ntrks;
	uint16_t division;
	bool TicksPerQtrNoteMode;
};

struct TrackChunk : public Chunk {
	double elapsedMS;

	uint8_t runningStatus;
	std::vector<BaseMTrkEvent*> mtrkEvents;
};

// for each channel (16 total),
// keep track of program (i.e. instrument),
// pitch bend state, floppy drive mapping,
// and whether the channel has been used
// in the current midi
struct Channel {
	uint8_t prog, volume, chanToDrive, expression;
	double pitchBendFactor;
	bool channelHasBeenUsed;

	// one for each of 128 possible notes
	uint16_t activeNotes[MAX_NOTES];
	
	// floppies can only play one note at a time (of course!)
	// so we'll compromise by keeping track of it here
	// instead of either not keeping track (we want to have it for
	// in-place pitch bend modification)
	// or having a full separate stream class like for sine audio
	bool isPlayingOnFloppy;
	double floppyFreq;
};

void generateVariableLengthMessage(const std::string& bytes, std::ofstream& log);

// a midi file consists of a header chunk and a variable
// number of track chunks
class MIDI {
	friend class MidiEvent;
	friend class SysExEvent;
	friend class MetaEvent;
public:
	std::string fileName, rawMIDI;
	bool isClosing;

	bool loadBinaryFile();
	void playMusic();
	void stepThroughCompletedMidiStructure();
	bool parseMIDIFile();
	void cleanUpAudio();
	void cleanUpMemory();

private:
	size_t pos, fileSize;
	size_t usecPerQtrNote;
	size_t ticksPerQtrNote;
	size_t ticksPerFrame;
	double FPS;
	HeaderChunk header;
	std::vector<TrackChunk> chunks;
	std::chrono::high_resolution_clock::time_point startTime;
	Channel channels[NUM_CHANNELS];
	size_t maxTotalChannels;
	uint8_t freeDrive;
#ifdef PLAY_FLOPPY
	Serial* serial;
#endif
#ifdef PLAY_SINE
	Stream* stream;
#endif


	size_t readVariableLengthQuantity();
	size_t readFourBinaryBytes();
	uint16_t readTwoBinaryBytes();
	uint8_t readOneBinaryByte();
	bool parseBaseMTrkEvent(TrackChunk& chunk);
	bool parseHeader();
	bool parseChunk();
	std::string extractNote(const size_t chan, const MidiEvent& evt, const bool logDrives);
	std::string extractProgramChange(const uint8_t chan, const MidiEvent& evt);
	void noteOff(const size_t chan, const MidiEvent& evt);
	void sendNoteToFloppy(const size_t chan);
	void noteOn(const size_t chan, const MidiEvent& evt);
	void setChannelVolume(const size_t chan, const MidiEvent& evt);
	void setChannelExpression(const size_t chan, const MidiEvent& evt);
	void updatePlayingNotes(const size_t chan);
	void setPitchBend(const size_t chan, const MidiEvent& evt);
	void decodeDivision();
	void playTrack(const size_t track);
};

// chunks are composed of a variable number of MTrkEvents
// which consist of a delta-time and one of the three
// types of events
//
// base class for all types of supported events
// derived into Midi, SysEx, Meta
class BaseMTrkEvent {
public:
	size_t deltaTime;
	virtual void loadEvent(MIDI& midi, TrackChunk& chunk, const uint8_t firstByte) = 0;
	virtual void processEvent(MIDI& midi, std::ofstream& log) = 0;
	virtual void playEvent(MIDI& midi, const size_t track) = 0;
};

class MidiEvent : public BaseMTrkEvent {
public:
	uint8_t status;
	uint8_t byte1;
	uint8_t byte2;

	void loadEvent(MIDI& midi, TrackChunk& chunk, const uint8_t firstByte);
	void processEvent(MIDI& midi, std::ofstream& log);
	void playEvent(MIDI& midi, const size_t track);
};

class SysExEvent : public BaseMTrkEvent {
public:
	size_t length;
	std::string bytes;

	void loadEvent(MIDI& midi, TrackChunk& chunk, const uint8_t firstByte);
	void processEvent(MIDI& midi, std::ofstream& log);
	void playEvent(MIDI& midi, const size_t track) {};
};

class MetaEvent : public BaseMTrkEvent {
public:
	uint8_t metaType;
	size_t length;
	std::string bytes;

	void loadEvent(MIDI& midi, TrackChunk& chunk, const uint8_t firstByte);
	void processEvent(MIDI& midi, std::ofstream& log);
	void playEvent(MIDI& midi, const size_t track);
};

#endif