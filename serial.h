/*******************************************************************
*   serial.h
*   SongOfTheFloppies
*	Kareem Omar
*
*	6/18/2015
*   This program is entirely my own work.
*******************************************************************/

// This module provides a Windows- and Linux-compatible object-oriented
// serial class. It creates a single, very efficient serial connection,
// though it could easily be scaled to allow multiple simultaneous
// connections.

#ifndef SERIAL_H
#define SERIAL_H

// define without parens so
// macro below can adjust for Linux
#define BAUD 500000

#ifdef _WIN32
// wchar_t string required for Windows
#define PORT L"\\\\.\\COM6"
#else
#define PORT "/dev/ttyACM0"
#endif




#define PASTER(x,y) x ## ## y
#define EVALUATOR(x,y)  PASTER(x,y)
#define NAME(fun) EVALUATOR(fun, BAUD)
#ifdef _WIN32
#define BAUD_RATE (BAUD)
#else
#define BAUD_RATE NAME(B)
#endif

#include <iostream>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstring>
#include <fcntl.h>
#include <cerrno>
#include <termios.h>
#include <unistd.h>
#endif

class Serial {
private:
	bool connected;

#ifdef _WIN32
	// handle to serial "file"
	// type HANDLE (which is a typedef of void*) on Windows,
	// int on Linux
	HANDLE hSerial;

	// connection info struct
	// COMSTAT on Windows,
	// termios on Linux
	COMSTAT status;

	// error struct (Windows only)
	DWORD errors;
#else
	int hSerial;
	termios tty;
#endif

public:
	// handles connection setup
	Serial();

	// handles connection teardown
	~Serial();

#ifndef _WIN32
	int set_interface_attribs(speed_t speed);
#endif

	long long readData(void *buffer, unsigned long numBytes);

	bool writeData(void *buffer, unsigned long numBytes);

	bool isConnected();

	std::mutex serialMutex;

};

#endif
