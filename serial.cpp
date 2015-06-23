/*******************************************************************
*   serial.cpp
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

#include "serial.h"

#ifndef _WIN32
int Serial::set_interface_attribs(speed_t speed) {

memset(&tty, 0, sizeof tty);

if ( tcgetattr ( hSerial, &tty ) != 0 ) {
   std::cout << "Error " << errno << " from tcgetattr: " << strerror(errno) << std::endl;
}

// set baud
cfsetospeed(&tty, speed);
cfsetispeed(&tty, speed);

// configure 8n1
tty.c_cflag     &=  ~PARENB;
tty.c_cflag     &=  ~CSTOPB;
tty.c_cflag     &=  ~CSIZE;
tty.c_cflag     |=  CS8;

tty.c_cflag     &=  ~CRTSCTS;
tty.c_cc[VMIN]   =  1;
tty.c_cc[VTIME]  =  5;
tty.c_cflag     |=  CREAD | HUPCL | CLOCAL;

// HUPCL requests hang-up (Arduino RST)

cfmakeraw(&tty);

tcflush( hSerial, TCIOFLUSH );
if ( tcsetattr ( hSerial, TCSANOW, &tty ) != 0) {
	std::cout << "Error " << errno << " from tcsetattr: " << strerror(errno) << std::endl;
}

	return 0;
}

#endif

Serial::Serial() {

	connected = false;

#ifdef _WIN32
	// connect to port
	hSerial = CreateFile(PORT,
		GENERIC_READ | GENERIC_WRITE,		// access(read + write) mode
		0,									// share mode
		NULL,								// address of security descriptor
		OPEN_EXISTING,						// creation mode
		FILE_ATTRIBUTE_NORMAL,				// file attribs
		NULL);								// handle of file with attributes to copy (N/A)

	// if connection unsuccessful...
	if (hSerial == INVALID_HANDLE_VALUE) {
		// ...store and display an error
		DWORD error;
		// file not found (device unplugged) is by far most common error
		if ((error = GetLastError()) == ERROR_FILE_NOT_FOUND) {
			std::cout << "ERROR: Serial device not accessible." << std::endl
				<< "Is the Arduino plugged in and powered on? Is the port correct?" << std::endl;
		}
		else {
			std::cout << "ERROR: Unrecognized error. Code: " << error << std::endl;
		}
	}
	else {
		// perpare DCB struct for connection state
		DCB dcbSerialParams = { 0 };

		// get current state
		if (!GetCommState(hSerial, &dcbSerialParams)) {
			std::cout << "Unable to retrieve current serial parameters." << std::endl;
		}
		else {
			// set params as desired
			dcbSerialParams.BaudRate = BAUD_RATE;
			dcbSerialParams.ByteSize = 8;
			dcbSerialParams.StopBits = ONESTOPBIT;
			dcbSerialParams.Parity = NOPARITY;

			// DTR is shorted to RST pin on Arduino
			// This is DESIRED behavior for this software.
			// we want to recalibrate floppy drives on
			// program launch
			dcbSerialParams.fDtrControl = DTR_CONTROL_ENABLE;

			// write back params
			if (!SetCommState(hSerial, &dcbSerialParams)) {
				std::cout << "ERROR: Could not set Serial Port parameters" << std::endl;
			}
			else {
				connected = true;

				// flush I and O
				PurgeComm(hSerial, PURGE_RXCLEAR | PURGE_TXCLEAR);
			}
		}
	}
#else
	if((hSerial = open(PORT, O_RDWR | O_NOCTTY)) < 0) {
		std::cout << "ERROR: Serial device not accessible." << std::endl
			<< "Is the Arduino plugged in and powered on? Is the port correct?" << std::endl;
	}
	else {
		set_interface_attribs(BAUD_RATE);

		connected = true;
	}

#endif
}

Serial::~Serial() {

	if (connected) {

		connected = false;

#ifdef _WIN32
		CloseHandle(hSerial);
#else
		close(hSerial);
#endif

	}
}

long long Serial::readData(void *buffer, unsigned long numBytes) {
#ifdef _WIN32
	unsigned long actuallyRead;

	// get port status
	ClearCommError(hSerial, &errors, &status);

	// if available data, read as much as possible without getting more than numBytes
	if (status.cbInQue > 0 && ReadFile(hSerial, buffer, status.cbInQue > numBytes ? numBytes : status.cbInQue, &actuallyRead, NULL) && actuallyRead != 0)
		return (long long)actuallyRead;

	// if nothing read or some other error
	return -1;
#else
	return read(hSerial, buffer, (size_t)numBytes);
#endif

}

bool Serial::writeData(void *buffer, unsigned long numBytes) {
#ifdef _WIN32
	unsigned long bytesActuallySent;
	
	serialMutex.lock();

	// try to write to serial
	if (!WriteFile(hSerial, buffer, numBytes, &bytesActuallySent, 0)) {
		// if unsuccessful, eat error and return false
		ClearCommError(hSerial, &errors, &status);
		serialMutex.unlock();
		return false;
	}
	else {
		serialMutex.unlock();
		return true;
	}
#else
	serialMutex.lock();
	write(hSerial, buffer, numBytes);
	serialMutex.unlock();
	return true;
#endif
}

bool Serial::isConnected() {
	return connected;
}
