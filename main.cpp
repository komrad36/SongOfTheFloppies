/*******************************************************************
*   main.cpp
*   SongOfTheFloppies
*	Kareem Omar
*
*	6/18/2015
*   This program is entirely my own work.
*******************************************************************/

// SongOfTheFloppies parses any MIDI file into an internal structure
// and a text-readable log file called midi_log.txt. It then plays the
// MIDI simplistically using sine waves, if PLAY_SINE is defined in MIDI.h
// and/or plays it on floppy drive stepper motors if PLAY_FLOPPY is defined
// in MIDI.h.

#ifdef _DEBUG

#ifndef DBG_NEW
#define DBG_NEW new ( _NORMAL_BLOCK , __FILE__ , __LINE__ )
#define new DBG_NEW
#endif // DBG_NEW

#ifndef _CRTDBG_MAP_ALLOC
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#endif // _CRTDBG_MAP_ALLOC

#endif // _DEBUG

#include <iostream>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <Windows.h>
#define ADD_HANDLER ((BOOL)(1))
#else
#include <signal.h>
#include <unistd.h>
#endif

#include "MIDI.h"

MIDI midi;

#ifdef _WIN32
BOOL CtrlHandler(DWORD fdwCtrlType) {
	midi.isClosing = true;

	// don't let the system kill the process.
	// asynchronous handler will exit()
	// the process when it's ready.
	Sleep(INFINITE);

	return true;
}
#else
void CtrlHandler(int s) {

	// Linux handles things differently.
	// as long as you've registered the handler,
	// it won't kill the process
	midi.isClosing = true;
}
#endif

int main(int argc, char* argv[]) {
	// for memory leak testing
#ifdef _DEBUG
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

	midi.isClosing = false;

	// for graceful cleanup if user terminates process early
	// examples: stop drives from getting stuck on notes,
	// stop streams from producing loud pops
#ifdef _WIN32
	SetConsoleCtrlHandler((PHANDLER_ROUTINE)CtrlHandler, ADD_HANDLER);
#else
	struct sigaction sigIntHandler;

	sigIntHandler.sa_handler = CtrlHandler;
	sigemptyset(&sigIntHandler.sa_mask);
	sigIntHandler.sa_flags = 0;

	sigaction(SIGINT, &sigIntHandler, NULL);
	sigaction(SIGTERM, &sigIntHandler, NULL);
	sigaction(SIGHUP, &sigIntHandler, NULL);

#endif

	if (argc == 1) {
		std::cout << "No input file specified. Please specify an input file" << std::endl
			<< "or drag-and-drop a MIDI file onto this program." << std::endl << std::endl;
		return EXIT_FAILURE;
	}

	if (argc > 2) {
		std::cout << "Please specify only a single input MIDI file" << std::endl
			<< "or drag-and-drop one onto this program." << std::endl << std::endl;
		return EXIT_FAILURE;
	}

	midi.fileName = std::string(argv[1]);

	if (!midi.loadBinaryFile()) {
		std::cout << "Failed to load MIDI file." << std::endl;
		return EXIT_FAILURE;
	}

	if (midi.isClosing)
		return EXIT_FAILURE;
 
	std::cout << "Parsing MIDI file..." << std::endl;

	if (!midi.parseMIDIFile()) {
		if (midi.isClosing)
			return EXIT_FAILURE;

		std::cout << "Error parsing MIDI file." << std::endl;
		midi.stepThroughCompletedMidiStructure();
		return EXIT_FAILURE;
	}

	std::cout << "MIDI file parsed successfully." << std::endl;
	midi.rawMIDI.clear();

	if (midi.isClosing)
		return EXIT_FAILURE;

	std::cout << "Logging parsed MIDI structure to midi_log.txt..." << std::endl;
	midi.stepThroughCompletedMidiStructure();
	std::cout << "Done." << std::endl << std::endl;

	if (midi.isClosing)
		return EXIT_FAILURE;

	std::cout << "Playing parsed MIDI..." << std::endl;

#if defined(PLAY_SINE) || defined(PLAY_FLOPPY)
	midi.playMusic();
#endif

	std::cout << "Done!" << std::endl;

	return EXIT_SUCCESS;
}
