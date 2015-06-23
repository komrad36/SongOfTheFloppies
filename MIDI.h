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

#define MAX_DRIVES										(13)
#define MIN_FLOPPY_NOTE									(25)
#define MAX_FLOPPY_NOTE									(57)
#define MAX_PITCH_BEND_SEMITONES						(2.0)
#define NOTE_DOWN_SHIFT_SEMITONES						(12)

#define US_TO_WAIT_BETWEEN_ARDUINO_READINESS_CHECKS		(1000)
#define MS_TO_WAIT_AFTER_CALIBRATION					(500)
#define MS_TO_WAIT_AFTER_PLAYING						(300)
////////////////////////////////



// BELOW ARE NOT TO BE MODIFIED //
#define MIDI_STANDARD_DEFAULT_USEC_PER_QTR_NOTE			(500000)

// 2 MB
#define DEFAULT_EXPRESSION								(127)
#define DEFAULT_VOLUME									(100)
#define DEFAULT_INSTRUMENT								(1)
#define VOLUME_NORM										(127.0)
#define MIN_FLOPPY_VOLUME								(1000.0f)
#define MAX_MIDI_FILE_SIZE_IN_BYTES						(2000000)
#define NUM_CHANNELS									(16)
#define	NOT_ACTIVE										(65535)
#define MILLISECONDS_PER_SECOND							(1000.0)
#define NANOSECONDS_PER_MILLISECOND						(1000000.0)
#define MICROSECONDS_PER_SECOND							(1000000.0)
#define PACKET_SIZE_BYTES								(4)
#define NUM_SEMITONES_IN_OCTAVE							(12)
#define fNUM_SEMITONES_IN_OCTAVE						(12.0)
#define MAX_NOTES										(128)
#define FREQ_MULTIPLIER									(10000.0)
#define CHANNEL_NOT_ASSIGNED							(255)
#define EFFECTS_CHANNEL									(10)

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

// keep track of what derived class
// a BaseEvent is
enum class EventType { MIDI, SysEx, Meta };

// base class for all types of supported events
// derived into Midi, SysEx, Meta
class BaseEvent {
public:
	EventType type;
};

class MidiEvent : public BaseEvent {
public:
	uint8_t status;
	uint8_t byte1;
	uint8_t byte2;
};

class SysExEvent : public BaseEvent {
public:
	size_t length;
	std::string bytes;
};

class MetaEvent : public BaseEvent {
public:
	uint8_t metaType;
	size_t length;
	std::string bytes;
};

// chunks are composed of a variable number of MTrkEvents
// which consist of a delta-time and one of the three
// types of events
class MTrkEvent {
public:
	size_t deltaTime;
	BaseEvent* evt;
};

// base chunk, derived into either header or track chunk
class Chunk {
public:
	size_t length;
};

class HeaderChunk : public Chunk {
public:
	
	//length is typically 6 bytes exactly (excluding type and length fields)
	uint16_t format;
	uint16_t ntrks;
	uint16_t division;
	bool TicksPerQtrNoteMode;
};

class TrackChunk : public Chunk {
public:
	double elapsedMS;

	uint8_t runningStatus;
	std::vector<MTrkEvent> mtrkEvents;
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

// a midi file consists of a header chunk and a variable
// number of track chunks
class MIDI {
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
	std::vector < TrackChunk > chunks;

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
	bool parseBaseEvent(TrackChunk& chunk, MTrkEvent& mte);
	bool parseMTrkEvent(TrackChunk& chunk);
	bool parseHeader();
	bool parseChunk();
	std::string extractNote(size_t chan, MidiEvent* evt, bool logDrives);
	std::string extractProgramChange(uint8_t chan, MidiEvent* evt);
	void noteOff(size_t chan, MidiEvent* evt);
	void sendNoteToFloppy(size_t chan);
	void noteOn(size_t chan, MidiEvent* evt);
	void setChannelVolume(size_t chan, MidiEvent* evt);
	void setChannelExpression(size_t chan, MidiEvent* evt);
	void updatePlayingNotes(size_t chan);
	void setPitchBend(size_t chan, MidiEvent* evt);
	void decodeDivision();
	void playTrack(size_t track);
};

#endif
