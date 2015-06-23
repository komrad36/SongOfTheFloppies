/*******************************************************************
*   MIDI.cpp
*   SongOfTheFloppies
*	Kareem Omar
*
*	6/18/2015
*   This program is entirely my own work.
*******************************************************************/

// This module contains code necessary for parsing and playing MIDI files
// on sine waves and floppy drive stepper motors (via serial->Arduino).

#include "MIDI.h"

// would put in MIDI struct but would require passing
// separately anyway for volatile keyword
// (which I can't apply to whole struct due to vectors)
volatile double ticksPerSecond;

// same for this. used to keep track of available indices in portAudio optimized
// note array
std::queue<uint16_t> availablePlayIndices;

std::mutex mtx, track0mtx;
std::condition_variable cv;
volatile bool ready;

// load entire binary contents of MIDI file into RAM (they're small)
bool MIDI::loadBinaryFile() {

	std::ifstream in(fileName.c_str(), std::ios::binary);
	if (!in) {
		std::cout << "Failed to open file. Is the file name correct?" << std::endl;
		return false;
	}

	in.seekg(0, std::ios::end);
	fileSize = (size_t)(in.tellg());
	if (fileSize > MAX_MIDI_FILE_SIZE_IN_BYTES) {
		std::cout << "File is too large! Are you sure that's a MIDI?" << std::endl;
		return false;
	}

	// slightly evil trick
	// write data directly into string buffer
	// not pretty, but guaranteed safe since
	// C++11 mandates contiguous string storage
	rawMIDI.clear();
	rawMIDI.resize(fileSize);
	in.seekg(0, std::ios::beg);
	in.read(&rawMIDI[0], fileSize);
	in.close();

	return true;
}

// MIDIs contain some quantities stored as
// 'variable-length quantities', where 7 bits
// per byte are used for storage and the 8th bit
// is used as a flag - 1 means there are more
// bytes, 0 means this is the last byte
// (see the MIDI standard)
size_t MIDI::readVariableLengthQuantity() {
	size_t value;
	uint8_t c;
	// store byte in value
	// if no flag, done immediately
	if ((value = rawMIDI[pos++]) & 0x80) {
		// if flag, strip flag...
		value &= 0x7f;
		do {
			// ...and while there are more bytes,
			// bitshift left 7, strip flag, repeat to add more bytes
			value = (value << 7) + ((c = rawMIDI[pos++]) & 0x7f);
		} while (c & 0x80);
	}
	return value;
}

// read and endian-swap 4 bytes of raw data
size_t MIDI::readFourBinaryBytes() {
	size_t startPos = pos;
	pos += 4;
	return swapi4(*(uint32_t*)(rawMIDI.substr(startPos, 4).c_str()));
}

// read and endian-swap 4 bytes of raw data
uint16_t MIDI::readTwoBinaryBytes() {
	size_t startPos = pos;
	pos += 2;
	return swapi2(*(uint16_t*)(rawMIDI.substr(startPos, 2).c_str()));
}

// read and endian-swap 3 bytes of a string
size_t ThreeBinaryBytesDirectToInt(std::string bytes) {
	// pad out with 0 bits to make it 4 bytes, then use int32 ptr for swap
	return swapi4(*(uint32_t*)((std::string("\0", 1) + bytes).c_str()));
}

// only 1 byte so no need for endian-swap
uint8_t MIDI::readOneBinaryByte() {
	return *(uint8_t*)(rawMIDI.substr(pos++, 2).c_str());
}

bool MIDI::parseBaseEvent(TrackChunk& chunk, MTrkEvent& mte) {
	// determine type of event
	// possibilities are MIDI event, sysex event, or meta event

	// first byte will help us determine this.
	// if it's F0 or F7, it's a sysex
	// if it's FF, it's a meta
	// all others are midi

	uint8_t firstByte = readOneBinaryByte();

	BaseEvent* evt;
	size_t length;
	uint8_t status;

	switch (firstByte) {
	case 0xF0:
	case 0xF7:
		// sysex event
		evt = new SysExEvent();
		evt->type = EventType::SysEx;
		length = static_cast<SysExEvent*>(evt)->length = readVariableLengthQuantity();
		static_cast<SysExEvent*>(evt)->bytes = rawMIDI.substr(pos, length);
		pos += length;
		break;

	case 0xFF:
		// meta event
		evt = new MetaEvent();
		evt->type = EventType::Meta;
		static_cast<MetaEvent*>(evt)->metaType = readOneBinaryByte();
		length = static_cast<MetaEvent*>(evt)->length = readVariableLengthQuantity();
		static_cast<MetaEvent*>(evt)->bytes = rawMIDI.substr(pos, length);
		pos += length;
		break;

	default:
		// MIDI event
		evt = new MidiEvent();
		evt->type = EventType::MIDI;

		// Tricky thing: "running status." If we get an invalid status byte (< 0x80),
		// RE-USE the status of the previous byte, and jump right into data bytes 1 and/or 2.

		// Another tricky thing, sometimes there are no further bytes,
		// sometimes there is one, sometimes there are two.
		// The status byte determines this. See http://www.org/techspecs/midimessages.php.

		if (firstByte < 0x80) {
			status = static_cast<MidiEvent*>(evt)->status = chunk.runningStatus;
			static_cast<MidiEvent*>(evt)->byte1 = firstByte;
		}
		else {
			status = chunk.runningStatus = static_cast<MidiEvent*>(evt)->status = firstByte;
			static_cast<MidiEvent*>(evt)->byte1 = (status >= 244) ? 0 : readOneBinaryByte();
		}

		static_cast<MidiEvent*>(evt)->byte2 = ((status >= 192 && status <= 223) || status == 243) ? 0 : readOneBinaryByte();
	}

	mte.evt = evt;
	return true;
}

bool MIDI::parseMTrkEvent(TrackChunk& chunk) {
	MTrkEvent mte = MTrkEvent();
	mte.deltaTime = readVariableLengthQuantity();

	if (!parseBaseEvent(chunk, mte)) {
		std::cout << "Failed to parse base MIDI event." << std::endl;
		return false;
	}

	chunk.mtrkEvents.push_back(mte);

	return true;
}

bool MIDI::parseHeader() {
	if (fileSize < 5)
		return false;

	// check for correct MIDI header tag
	if (rawMIDI.substr(0, 4) != "MThd")
		return false;

	// get length of header chunk (should be 6)
	pos = 4;

	header.length = readFourBinaryBytes();

	header.format = readTwoBinaryBytes();
	header.ntrks = readTwoBinaryBytes();
	header.division = readTwoBinaryBytes();

	// there could be more to the header, which we should IGNORE,
	// so reset position past MThd tag (4), past length field (4),
	// and past ACTUAL header length as determined by length field
	pos = 4 + 4 + header.length;

	return true;
}

bool MIDI::parseChunk() {

	// check for correct MIDI track chunk tag
	if (rawMIDI.substr(pos, 4) != "MTrk")
		return false;

	// instantiate new chunk
	TrackChunk chunk = TrackChunk();

	// get length of chunk
	pos += 4;
	chunk.length = readFourBinaryBytes();

	size_t chunkEnd = pos + chunk.length;

	while (pos < chunkEnd) {
		if (!parseMTrkEvent(chunk)) {
			std::cout << "Failed to parse MIDI MTrkEvent." << std::endl;
			return false;
		}
	}
	chunks.push_back(chunk);
	return true;
}

bool MIDI::parseMIDIFile() {

	pos = 0;

	if (!parseHeader()) {
		std::cout << "Failed to parse MIDI header." << std::endl;
		return false;
	}

	while (pos < fileSize) {
		if (!parseChunk()) {
			std::cout << "Failed to parse MIDI chunk." << std::endl;
			return false;
		}
		if (isClosing) {
			cleanUpMemory();
			return false;
		}
	}

	return true;
}

void extractTimeSignature(std::string bytes, std::ofstream& log) {
	// numerator
	std::string num = (bytes.size() <= 0) ? "??" : std::to_string(bytes[0]);

	// denominator (as 2^n)
	std::string den = (bytes.size() <= 1) ? "??" : std::to_string((size_t)(pow(2, bytes[1])));

	// MIDI clocks per metronome click
	std::string cc = (bytes.size() <= 2) ? "??" : std::to_string(bytes[2]);

	// clocks per quarter-note
	std::string bb = (bytes.size() <= 3) ? "??" : std::to_string(bytes[3]);

	log << num << "/" << den
		<< ", " << cc << " clocks/metronome tick, "
		<< bb
		<< " clocks/qtr-note." << std::endl;
}

void extractSMPTE(std::string bytes, std::ofstream& log) {
	std::string hr = (bytes.size() <= 0) ? "??" : std::to_string(bytes[0]);
	std::string min = (bytes.size() <= 1) ? "??" : std::to_string(bytes[1]);
	std::string sec = (bytes.size() <= 2) ? "??" : std::to_string(bytes[2]);

	std::string frames;
	if (bytes.size() == 4) {
		frames = std::to_string(bytes[3]);
	}
	else if (bytes.size() == 5) {
		frames = std::to_string(bytes[3] + 0.01*bytes[4]);
	}
	else {
		frames = "??";
	}

	log << std::setw(2) << std::setfill('0') << hr << ":" << std::setw(2) << std::setfill('0') << min
		<< ":" << std::setw(2) << std::setfill('0') << sec << " and " << frames << " frames" << std::endl;
}

void extractKeySignature(std::string bytes, std::ofstream& log) {

	if (bytes.size() != 2) {
		log << "<invalid>" << std::endl;
		return;
	}

	// sharps and flats
	int8_t sf = bytes[0];

	// major/minor
	int8_t mi = bytes[1];

	if (sf < 0) {
		size_t numFlats = (size_t)(-sf);
		log << numFlats << " flat" << ((numFlats == 1) ? "" : "s") << ", ";
	}
	else if (sf > 0) {
		size_t numSharps = (size_t)(sf);
		log << numSharps << " sharp" << ((numSharps == 1) ? "" : "s") << ", ";
	}
	else {
		log << "Key of C ";
	}

	log << ((mi == 1) ? "minor" : "major") << std::endl;
}

bool invalidProg(uint8_t prog) {
	return (prog > 112 || (prog >= 97 && prog <= 104));
}

std::string MIDI::extractNote(size_t chan, MidiEvent* evt, bool logDrives) {
	if (logDrives && chan != 10 && !channels[chan - 1].channelHasBeenUsed && !invalidProg(channels[chan - 1].prog)) {
		++maxTotalChannels;
		channels[chan - 1].channelHasBeenUsed = true;

#ifndef ASSIGN_CHANNELS_TO_DRIVES_SEQUENTIALLY
		if (freeDrive < MAX_DRIVES) channels[chan - 1].chanToDrive = freeDrive++;
#endif

	}

	// get octave (see MIDI standard)
	unsigned short octave = evt->byte1 / 12 - 1;

	// get note
	std::string noteSelect[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
	std::string note = noteSelect[evt->byte1 % 12];
	return note + std::to_string(octave) + ", Velocity (0 - 127): " + std::to_string(evt->byte2);
}

uint16_t pitchBendBytes(MidiEvent* evt) {
	return ((uint16_t)evt->byte2 << 7) + (uint16_t)evt->byte1;
}

std::string extractPitchBendChange(MidiEvent* evt) {
	return std::to_string(pitchBendBytes(evt));
}

std::string MIDI::extractProgramChange(uint8_t chan, MidiEvent* evt) {
	return std::to_string(channels[chan - 1].prog = evt->byte1);
}

std::string extractModeChange(MidiEvent* evt) {
	switch (evt->byte1) {
	case 0x00:
		return "Bank Select (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x01:
		return "Modulation Wheel (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x05:
		return "Portamento Time (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x06:
		return "Data Entry, MSB (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x07:
		return "Channel Volume (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x0A:
		return "Channel Pan (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x0B:
		return "Expression Control (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x20:
		return "LSB for Control 0 (Bank Select) (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x21:
		return "LSB for Control 1 (Modulation Wheel) (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x22:
		return "LSB for Control 2 (Breath Controller) (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x23:
		return "LSB for Control 3 (undef) (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x24:
		return "LSB for Control 4 (Foot Controller) (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x25:
		return "LSB for Control 5 (Portamento Time) (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x26:
		return "LSB for Control 6 (Data Entry) (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x27:
		return "LSB for Control 7 (Channel Volume) (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x28:
		return "LSB for Control 8 (Balance) (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x29:
		return "LSB for Control 9 (undef) (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x2A:
		return "LSB for Control 10 (Pan) (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x2B:
		return "LSB for Control 11 (Expression Controller) (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x2C:
		return "LSB for Control 12 (Effect control 1) (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x2D:
		return "LSB for Control 13 (Effect control 2) (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x40:
		return "Damper/sustain " + std::string((evt->byte2 >= 64) ? "ON" : "OFF");
		break;
	case 0x41:
		return "Portamento " + std::string((evt->byte2 >= 64) ? "ON" : "OFF");
		break;
	case 0x46:
		return "Sound Controller 1 (default: Sound Variation) (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x47:
		return "Sound Controller 2 (default: Timbre/Harmonic Intens.) (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x48:
		return "Sound Controller 3 (default: Release Time) (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x49:
		return "Sound Controller 4 (default: Attack Time) (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x4A:
		return "Sound Controller 5 (default: Brightness) (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x4B:
		return "Sound Controller 6 (default: Decay Time) (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x4C:
		return "Sound Controller 7 (default: Vibrato Rate) (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x4D:
		return "Sound Controller 8 (default: Vibrato Depth) (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x4E:
		return "Sound Controller 9 (default: Vibrato Delay) (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x4F:
		return "Sound Controller 10 (default: undef) (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x5B:
		return "Effects 1 (Default==Reverb) Depth (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x5C:
		return "Effects 2 (Default==Tremolo) Depth (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x5D:
		return "Effects 3 (Default==Chorus) Depth (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x5E:
		return "Effects 4 (Default==Celeste/Detune) Depth (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x5F:
		return "Effects 5 (Default==Phaser) Depth (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x62:
		return "NRPN LSB (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x63:
		return "NRPN MSB (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x64:
		return "RPN LSB (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x65:
		return "RPN MSB (0-127): " + std::to_string(evt->byte2);
		break;
	case 0x78:
		return "All Sound OFF";
		break;
	case 0x79:
		return "Reset All Controllers";
		break;
	case 0x7A:
		return "Local Control: " + std::string((evt->byte2 == 0) ? "OFF" : "ON");
		break;
	case 0x7B:
		return "All Notes OFF";
		break;
	case 0x7C:
		return "Omni Mode OFF";
		break;
	case 0x7D:
		return "Omni Mode ON";
		break;
	case 0x7E:
		return "Mono Mode ON";
		break;
	case 0x7F:
		return "Poly Mode ON";
		break;
	default:
		return "Unknown mode (Code " + std::to_string(evt->byte1) + ")";
	}
}

double noteToFreq(uint8_t noteID) {
	return pow(2.0, (double)((int)noteID - 69) / 12.0)*440.0;
}

void MIDI::noteOff(size_t chan, MidiEvent* evt) {
#ifdef PLAY_SINE
	uint16_t idx = channels[chan - 1].activeNotes[evt->byte1];
	if (idx != NOT_ACTIVE) {
		stream->stopAudio(idx);
		channels[chan - 1].activeNotes[evt->byte1] = NOT_ACTIVE;

		mtx.lock();
		availablePlayIndices.push(idx);
		mtx.unlock();
	}
#endif

#ifdef PLAY_FLOPPY
	if (serial->isConnected()) {
		channels[chan - 1].isPlayingOnFloppy = false;
		uint32_t outBytes = (uint32_t)channels[chan - 1].chanToDrive;
		serial->writeData((void*)&outBytes, PACKET_SIZE_BYTES);
	}
#endif


#ifdef LOG_NOTES
#ifdef VERBOSE_1
#ifdef VERBOSE_2
		// printf for speed and for atomicity
		// (intentionally outside mtx for performance)
	printf("Channel %u Note OFF: %s\n", chan, extractNote(chan, evt, false).c_str());
#endif
#endif
#endif
}

// pack drive select and note frequency into single 4 byte
// unsigned int for sending efficiently over serial
// This is done by letting first byte store drive select
// and next 3 bytes store (freq*10000.0), truncated to an
// int. This way the Arduino can cast that int to float
// and divide by 10000 to get a decimal freq with up to
// 4 decimal place accuracy, while still allowing
// up to ~1677 Hz (floppies typically can only play up
// to ~400 Hz before the stepper motors slip)
#ifdef PLAY_FLOPPY
void MIDI::sendNoteToFloppy(size_t chan) {
	// floppies can't do note velocities, but
	// don't play super quiet notes
	if ((float)channels[chan - 1].expression * (float)channels[chan - 1].volume >= MIN_FLOPPY_VOLUME) {
		uint32_t convertedFreq = (uint32_t)(channels[chan - 1].floppyFreq*channels[chan - 1].pitchBendFactor*FREQ_MULTIPLIER);
		uint32_t outBytes = ((uint32_t)channels[chan - 1].chanToDrive) + (convertedFreq << 8);
		serial->writeData((void*)&outBytes, PACKET_SIZE_BYTES);
	}
}
#endif

void MIDI::updatePlayingNotes(size_t chan) {
#ifdef PLAY_SINE
	uint16_t idx;
	for (size_t i = 0; i < MAX_NOTES; ++i) {
		idx = channels[chan - 1].activeNotes[i];
		if (idx != NOT_ACTIVE) {
			stream->setPitchBend(idx, channels[chan - 1].pitchBendFactor);
			stream->setChannelVel(idx, channels[chan - 1].volume);
			stream->setChannelExpression(idx, channels[chan - 1].expression);
		}
	}
#endif

#ifdef PLAY_FLOPPY
	if (channels[chan - 1].isPlayingOnFloppy)
		sendNoteToFloppy(chan);
#endif
}

void MIDI::noteOn(size_t chan, MidiEvent* evt) {
#ifdef ASSIGN_CHANNELS_TO_DRIVES_SEQUENTIALLY
	if (chan != 10 && !channels[chan - 1].channelHasBeenUsed && !invalidProg(channels[chan - 1].prog)) {
		channels[chan - 1].channelHasBeenUsed = true;

		mtx.lock();
		if (freeDrive < MAX_DRIVES) channels[chan - 1].chanToDrive = freeDrive++;
		mtx.unlock();
	}
#endif

	uint8_t velocity = evt->byte2;

	// if this note ON is being used as a note OFF,
	// or it's a sound effect program rather than
	// a normal instrument that can be approximated
	// with a sine wave or floppy drive,
	// treat as note off instead
	if (velocity <= 1 || invalidProg(channels[chan - 1].prog)) {
		noteOff(chan, evt);
		return;
	}

#ifdef PLAY_SINE
	uint16_t idx = channels[chan - 1].activeNotes[evt->byte1];
	if (idx == NOT_ACTIVE) {

		// new note
		mtx.lock();
		if (availablePlayIndices.size() == 0) {
			std::cout << "ERROR: too many simultaneous voices!" << std::endl;
			mtx.unlock();
		}
		else {
			idx = channels[chan - 1].activeNotes[evt->byte1] = availablePlayIndices.front();
			availablePlayIndices.pop();
			mtx.unlock();
			stream->setFreqs(idx, noteToFreq(evt->byte1), channels[chan - 1].pitchBendFactor);
			stream->setVels(idx, channels[chan - 1].expression, channels[chan - 1].volume, velocity);
			stream->startAudio(idx);
		}
	}
	else {

		// already playing note, just updated freq and/or velocity
		stream->setFreqs(idx, noteToFreq(evt->byte1), channels[chan - 1].pitchBendFactor);
		stream->setVels(idx, channels[chan - 1].expression, channels[chan - 1].volume, velocity);
	}
#endif

#ifdef PLAY_FLOPPY
	if (serial->isConnected() && channels[chan - 1].chanToDrive != CHANNEL_NOT_ASSIGNED) {
		// shift all notes down to sound better on floppies...
		uint8_t note = evt->byte1 - NOTE_DOWN_SHIFT_SEMITONES;

		// ... and if note is still too high for floppy drives,
		// drop octaves until it's in range...
		while (note > MAX_FLOPPY_NOTE)
			note -= NUM_SEMITONES_IN_OCTAVE;

		// ...or if note is too low for floppy drives,
		// climb octaves until it's in range.
		while (note < MIN_FLOPPY_NOTE)
			note += NUM_SEMITONES_IN_OCTAVE;

		channels[chan - 1].floppyFreq = noteToFreq(note);
		channels[chan - 1].isPlayingOnFloppy = true;
		sendNoteToFloppy(chan);
	}
#endif

#ifdef LOG_NOTES
#ifdef VERBOSE_1
		// printf for speed and for atomicity
		// (intentionally outside mtx for performance)
	printf("Channel %llu Note ON: %s\n", (unsigned long long)chan, extractNote(chan, evt, false).c_str());
#endif
#endif
}

void MIDI::setChannelExpression(size_t chan, MidiEvent* evt) {
	channels[chan - 1].expression = evt->byte2;
	updatePlayingNotes(chan);

#ifdef LOG_NOTES
#ifdef VERBOSE_1
	// printf for speed and for atomicity
	// (intentionally outside mtx for performance)
	printf("Channel %llu Expression Change: %u\n", (unsigned long long) chan, channels[chan - 1].expression);
#endif
#endif
}

void MIDI::setChannelVolume(size_t chan, MidiEvent* evt) {
	channels[chan - 1].volume = evt->byte2;
	updatePlayingNotes(chan);

#ifdef LOG_NOTES
	// printf for speed and for atomicity
	// (intentionally outside mtx for performance)
	printf("Channel %llu Master Volume: %u\n", (unsigned long long) chan, channels[chan - 1].volume);
#endif
}

double pitchBendBytesToFactor(uint16_t bytes) {
	return pow(2.0, MAX_PITCH_BEND_SEMITONES*((double)bytes - 8192.0) / 8192.0 / fNUM_SEMITONES_IN_OCTAVE);
}

void MIDI::setPitchBend(size_t chan, MidiEvent* evt) {
	channels[chan - 1].pitchBendFactor = pitchBendBytesToFactor(pitchBendBytes(evt));
	updatePlayingNotes(chan);

#ifdef LOG_NOTES
	// printf for speed and for atomicity
	// (intentionally outside mtx for performance)
	printf("Channel %llu Pitch BEND! x%g\n", (unsigned long long) chan, channels[chan - 1].pitchBendFactor);
#endif
}

// output hex string for SysEx events
// ignored (not handled, just printed directly to log)
// by this program
void generateVariableLengthMessage(std::string bytes, std::ofstream& log) {
	log << "0x";
	for (size_t i = 0; i < bytes.size(); ++i) {
		log << std::hex << (uint16_t)bytes[i];
	}
	log << std::dec << std::endl;
}

void MIDI::decodeDivision() {
	// if bit 15 is 0, remaining bits give delta-time ticks per quarter note.
	// if bit 15 is 1, bits 14->8 give SMPTE fps and bits 7->0 give delta-time ticks/frame

	// if bit 15 is 0
	if (header.division < 0x8000) {
		header.TicksPerQtrNoteMode = true;
		ticksPerQtrNote = header.division;
		ticksPerSecond = MICROSECONDS_PER_SECOND * (double)ticksPerQtrNote / (double)usecPerQtrNote;
	}
	else { //bit 15 is 1
		header.TicksPerQtrNoteMode = false;
		uint8_t fps = -(uint8_t)((header.division - 0x8000) >> 8);
		uint8_t ticksPerFrame = (uint8_t)(header.division & 0xFF);

		FPS = (fps == 29) ? 29.97 : (double)fps;
		ticksPerSecond = ticksPerFrame * FPS;
	}
}

void MIDI::stepThroughCompletedMidiStructure() {

	maxTotalChannels = 0;

	freeDrive = 0;

	for (size_t i = 0; i < NUM_CHANNELS; ++i) {
		// default to piano
		channels[i].prog = DEFAULT_INSTRUMENT;

		// default to 100/127 volume
		channels[i].volume = DEFAULT_VOLUME;

		// default to 127/127 expression
		channels[i].expression = DEFAULT_EXPRESSION;

		channels[i].chanToDrive = CHANNEL_NOT_ASSIGNED;
		channels[i].channelHasBeenUsed = false;
		channels[i].isPlayingOnFloppy = false;
	}

	std::ofstream log("midi_log.txt");
	if (!log) {
		std::cout << "Failed to open midi_log.txt for logging. Aborting." << std::endl;
		return;
	}

	log << "Stepping through parsed MIDI structure:" << std::endl;
	log << "File Name: " << fileName << std::endl;
	log << "File Size (bytes): " << fileSize << std::endl << std::endl;
	log << "> Delta-times appear before each event." << std::endl << std::endl;
	log << ">>> MIDI Header:" << std::endl;
	log << "File format: " << header.format << std::endl;
	log << "Division: " << header.division << std::endl;
	log << "# of tracks: " << header.ntrks << std::endl;

	uint8_t status;

	for (size_t i = 0; i < chunks.size(); ++i) {
		log << ">>> Track " << i << ":" << std::endl;

		log << "# of MTrkEvents: " << chunks[i].mtrkEvents.size() << std::endl;
		for (auto it = chunks[i].mtrkEvents.begin(), end = chunks[i].mtrkEvents.end(); it != end; ++it) {
			if (isClosing) {
				log.close();
				cleanUpMemory();
				return;
			}
			log << it->deltaTime << " | ";
			switch (it->evt->type) {
			case EventType::MIDI:
				log << "MIDI Event: ";

				status = static_cast<MidiEvent*>(it->evt)->status;

				if (status >= 0x80 && status <= 0x8F) {
					log << "Chan " << status - 0x7F << " Note OFF: " << extractNote(status - 0x7F, static_cast<MidiEvent*>(it->evt), false) << std::endl;
				}
				else if (status >= 0x90 && status <= 0x9F) {
					log << "Chan " << status - 0x8F << " Note ON: " << extractNote(status - 0x8F, static_cast<MidiEvent*>(it->evt), true) << std::endl;
				}
				else if (status >= 0xB0 && status <= 0xBF) {
					log << "Chan " << status - 0xAF << " Control/Mode Change: " << extractModeChange(static_cast<MidiEvent*>(it->evt)) << std::endl;
				}
				else if (status >= 0xC0 && status <= 0xCF) {
					log << "Chan " << status - 0xBF << " Program Change: Select Program (0-127): " << extractProgramChange(status - 0xBF, static_cast<MidiEvent*>(it->evt)) << std::endl;
				}
				else if (status >= 0xE0 && status <= 0xEF) {
					log << "Chan " << status - 0xDF << " Pitch Bend Change (0-16383): " << extractPitchBendChange(static_cast<MidiEvent*>(it->evt)) << " (factor==" << pitchBendBytesToFactor(pitchBendBytes(static_cast<MidiEvent*>(it->evt))) << ')' << std::endl;
				}
				else {
					log << "Unknown (Code 0x" << std::hex << (uint16_t)(static_cast<MidiEvent*>(it->evt)->status) << std::dec << ")" << std::endl;
				}
			break;
			case EventType::Meta:
				log << "Meta Event: ";
				switch (static_cast<MetaEvent*>(it->evt)->metaType) {
				case 0x01:
				case 0x0A:
				case 0x0B:
					log << "Text: " << static_cast<MetaEvent*>(it->evt)->bytes.c_str() << std::endl;
					break;
				case 0x02:
					log << "Copyright Notice: " << static_cast<MetaEvent*>(it->evt)->bytes.c_str() << std::endl;
					break;
				case 0x03:
					log << "Track Name: " << static_cast<MetaEvent*>(it->evt)->bytes.c_str() << std::endl;
					break;
				case 0x04:
					log << "Instrument Name: " << static_cast<MetaEvent*>(it->evt)->bytes.c_str() << std::endl;
					break;
				case 0x05:
					log << "Lyric: " << static_cast<MetaEvent*>(it->evt)->bytes.c_str() << std::endl;
					break;
				case 0x06:
					log << "Marker: " << static_cast<MetaEvent*>(it->evt)->bytes.c_str() << std::endl;
					break;
				case 0x07:
					log << "Cue Point: " << static_cast<MetaEvent*>(it->evt)->bytes.c_str() << std::endl;
					break;
				case 0x08:
					log << "Program Name: " << static_cast<MetaEvent*>(it->evt)->bytes.c_str() << std::endl;
					break;
				case 0x09:
					log << "Device Name: " << static_cast<MetaEvent*>(it->evt)->bytes.c_str() << std::endl;
					break;
				case 0x20:
					log << "MIDI Channel: " << atoll(static_cast<MetaEvent*>(it->evt)->bytes.c_str()) << std::endl;
					break;
				case 0x21:
					log << "MIDI Port: " << atoll(static_cast<MetaEvent*>(it->evt)->bytes.c_str()) << std::endl;
					break;
				case 0x2F:
					log << "End of Track" << std::endl;
					break;
				case 0x51:
					usecPerQtrNote = ThreeBinaryBytesDirectToInt(static_cast<MetaEvent*>(it->evt)->bytes);
					log << "Set Tempo: " << usecPerQtrNote << " microsec per quarter note" << std::endl;
					log << "New Division Decode: ";
					decodeDivision();
					log << (header.TicksPerQtrNoteMode ? "Ticks/QtrNote Method: " : "FPS Method: ") << ticksPerSecond << " delta-time ticks per second." << std::endl;
					break;
				case 0x54:
					log << "SMPTE Offset: ";
					extractSMPTE(static_cast<MetaEvent*>(it->evt)->bytes, log);
					break;
				case 0x58:
					log << "Time Signature: ";
					extractTimeSignature(static_cast<MetaEvent*>(it->evt)->bytes, log);
					break;
				case 0x59:
					log << "Key Signature: ";
					extractKeySignature(static_cast<MetaEvent*>(it->evt)->bytes, log);
					break;
				case 0x7F:
					log << "Sequencer Specific Data" << std::endl;
					break;
				default:
					log << "Unknown (Code 0x" << std::hex << (uint16_t)(static_cast<MetaEvent*>(it->evt)->metaType) << std::dec << ")" << std::endl;
				}
				break;
			default: // SysEx:
				log << "SysEx Event: " << static_cast<SysExEvent*>(it->evt)->bytes.size() << " byte message: ";
				generateVariableLengthMessage(static_cast<SysExEvent*>(it->evt)->bytes, log);
			}
		}
	}

	log << "Total channels used: " << maxTotalChannels << std::endl;
	std::cout << "Total channels used: " << maxTotalChannels << std::endl;
	log.close();
}

void MIDI::playTrack(size_t track) {

	printf("Thread %lu launched for track playback.\n", track);

	uint8_t status;

	chunks[track].elapsedMS = 0.0;
	for (auto it = chunks[track].mtrkEvents.begin(), end = chunks[track].mtrkEvents.end(); it != end; ++it) {

		if (it->deltaTime != 0) {
			if (track == 0) {
				ready = true;
				
				cv.notify_one();
			}
			mtx.lock();
			chunks[track].elapsedMS += (double)it->deltaTime / ticksPerSecond * MILLISECONDS_PER_SECOND;
			mtx.unlock();
			std::this_thread::sleep_until(startTime + std::chrono::nanoseconds((long long)(NANOSECONDS_PER_MILLISECOND*chunks[track].elapsedMS)));
		}
		
		if (isClosing) {
			// don't let other threads try to clean up more than once
			mtx.lock();

			cleanUpAudio();
			cleanUpMemory();
			exit(EXIT_FAILURE);
		}

		switch (it->evt->type) {
		case EventType::MIDI:
			status = static_cast<MidiEvent*>(it->evt)->status;

			if (status >= 0x80 && status <= 0x8F) {
				noteOff(status - 0x7F, static_cast<MidiEvent*>(it->evt));
			}
			// don't use 0x99 (channel 10) as it is an effects/percussion channel
			else if (status >= 0x90 && status <= 0x9F && status != 0x99) {
				noteOn(status - 0x8F, static_cast<MidiEvent*>(it->evt));
			}
			else if (status >= 0xC0 && status <= 0xCF) {
				channels[status - 0xC0].prog = static_cast<MidiEvent*>(it->evt)->byte1;
			}
			else if (status >= 0xB0 && status <= 0xBF) {
				if (static_cast<MidiEvent*>(it->evt)->byte1 == 0x07) setChannelVolume(status - 0xAF, static_cast<MidiEvent*>(it->evt));
				if (static_cast<MidiEvent*>(it->evt)->byte1 == 0x0B) setChannelExpression(status - 0xAF, static_cast<MidiEvent*>(it->evt));
			}
			else if (status >= 0xE0 && status <= 0xEF) {
				setPitchBend(status - 0xDF, static_cast<MidiEvent*>(it->evt));
			}
			break;
		case EventType::Meta:
			switch (static_cast<MetaEvent*>(it->evt)->metaType) {
			case 0x01:
			case 0x0A:
			case 0x0B:
#ifdef LOG_NOTES
				printf("Text: %s\n", static_cast<MetaEvent*>(it->evt)->bytes.c_str());
#endif
				break;
			case 0x05:
#ifdef LOG_NOTES
				printf("Lyric: %s\n", static_cast<MetaEvent*>(it->evt)->bytes.c_str());
#endif
				break;
			case 0x2F:
				if (track == 0) {
					ready = true;
					cv.notify_one();
				}
#ifdef LOG_NOTES
				printf("End of Track %llu\n", (unsigned long long) track);
				printf("Elapsed time: %g sec\n", chunks[track].elapsedMS / 1000.0);
#endif
				break;
			case 0x51:
				mtx.lock();
				usecPerQtrNote = ThreeBinaryBytesDirectToInt(static_cast<MetaEvent*>(it->evt)->bytes);
				decodeDivision();
				mtx.unlock();
#ifdef LOG_NOTES
				printf("New Tempo: %g ticks per second\n", ticksPerSecond);
#endif
			}
			break;
		case EventType::SysEx:
			break;
		}
	}

	printf("Thread %lu terminating.\n", track);
}

void MIDI::playMusic() {

#ifdef ASSIGN_CHANNELS_TO_DRIVES_SEQUENTIALLY
	for (size_t i = 0; i < NUM_CHANNELS; ++i)
		channels[i].channelHasBeenUsed = false;
#endif

#ifdef LOG_NOTES
	std::cout << "Logging notes." << std::endl;
#endif
#ifdef VERBOSE_1
	std::cout << "Verbosity Level 1 enabled." << std::endl;
#endif
#ifdef VERBOSE_2
	std::cout << "Verbosity Level 2 enabled." << std::endl;
#endif

	// MIDI default, will probably be replaced by a 
	// MIDI 0x51 "Set Tempo" event, but maybe not
	usecPerQtrNote = MIDI_STANDARD_DEFAULT_USEC_PER_QTR_NOTE;
	decodeDivision();

#ifdef PLAY_FLOPPY
	serial = new Serial();
#endif

#ifdef PLAY_SINE

	std::cout << "Launching audio stream..." << std::endl;
	stream = new Stream();
	if (!stream->streamInitialized) {
		std::cout << "Aborting." << std::endl;
		return;
	}

	stream->initSineTable();

	std::cout << "Done." << std::endl;
#endif

	std::vector<std::thread> threads;

	for (int16_t i = MAX_SIMUL - 1; i >= 0; --i) {
		availablePlayIndices.push(i);
	}

	for (size_t i = 0; i < NUM_CHANNELS; ++i) {
		channels[i].pitchBendFactor = 1.0;
		for (size_t j = 0; j < MAX_NOTES; ++j) {
			channels[i].activeNotes[j] = NOT_ACTIVE;
		}
	}

	// wait for Arduino ready signal...
#ifdef PLAY_FLOPPY
	int buffer;
	std::cout << std::endl << "Waiting for Arduino to signal READY..." << std::endl;
	while (serial->readData((void*)&buffer, sizeof buffer) <= 0) {
		if (isClosing) {
			cleanUpAudio();
			cleanUpMemory();
			return;
		}
		std::this_thread::sleep_for(std::chrono::microseconds(US_TO_WAIT_BETWEEN_ARDUINO_READINESS_CHECKS));
	}

	std::cout << "Arduino ready!" << std::endl << std::endl;
#endif

	// ...and a bit more for aesthetics (pause between calibration and music)
	std::this_thread::sleep_for(std::chrono::milliseconds(MS_TO_WAIT_AFTER_CALIBRATION));

	std::cout << "Launching playback..." << std::endl;

	// mark start of playback to sync all future events to
	startTime = std::chrono::high_resolution_clock::now();

	// format 1 files must play multiple tracks simultaneously.
	if (header.format == 1) {

		// launch thread 0 first and lock until track 0
		// either ends or hits a non-zero delta time
		// so that tempo (which is first signaled at delta-time 0
		// in thread 0) can be established
		// before others start playing notes
		ready = false;
		threads.push_back(std::thread(&MIDI::playTrack, this, 0));
		std::unique_lock<std::mutex> lk(track0mtx);
		cv.wait(lk, []{ return ready; });

		for (size_t i = 1; i < chunks.size(); ++i) {
			threads.push_back(std::thread(&MIDI::playTrack, this, i));
		}

		// wait for song to finish playing
		for (auto it = threads.begin(), end = threads.end(); it != end; ++it){
			it->join();
		}
	}
	else {
		for (size_t i = 0; i < chunks.size(); ++i) {
			playTrack(i);
		}
	}

	cleanUpAudio();
	cleanUpMemory();
}

void MIDI::cleanUpMemory() {
	for (size_t i = 0; i < chunks.size(); ++i) {
		for (auto it = chunks[i].mtrkEvents.begin(), end = chunks[i].mtrkEvents.end(); it != end; ++it) {
			switch (it->evt->type) {
			case EventType::Meta:
				delete (static_cast<MetaEvent*>(it->evt));
				break;
			case EventType::MIDI:
				delete (static_cast<MidiEvent*>(it->evt));
				break;
			case EventType::SysEx:
				delete (static_cast<SysExEvent*>(it->evt));
			}
		}
	}
}

void MIDI::cleanUpAudio() {
#ifdef PLAY_SINE
	for (uint16_t i = 0; i < MAX_SIMUL; ++i)
		stream->stopAudio(i);
	
#endif


#ifdef PLAY_FLOPPY
	if (serial->isConnected()) {
		// tell each drive in turn to stop playing
		// by writing a 0 frequency to it
		for (uint32_t i = 0; i < freeDrive; ++i) {
			serial->writeData((void*)&i, PACKET_SIZE_BYTES);
		}
	}

#endif

	// wait for drives and/or streams to stop
	std::this_thread::sleep_for(std::chrono::milliseconds(MS_TO_WAIT_AFTER_PLAYING));

#ifdef PLAY_FLOPPY
	delete serial;
#endif

#ifdef PLAY_SINE
	delete stream;
#endif
}
