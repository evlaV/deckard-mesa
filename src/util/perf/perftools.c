//===================== Copyright (c) Valve Corporation. All Rights Reserved. ======================

//note this DEFAULT_SOURCE thing is important -- without it, you get clock_gettime not defined type errors
#define _DEFAULT_SOURCE /* for major() */ 

#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>

#include <sys/types.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "perftools.h"


char m_buffer[ BUFFER_SIZE ];
uint64_t m_unMessageID;

static int systemTimeInitialized = 0;
static uint64_t systemTicksPerSecond = 0;
static double systemSecondsPerTick = 0;
static uint64_t systemBaseTicks = 0;

static uint64_t ulMessageCounter = 30000000;

static int s_nTraceFileFD = -1;  

uint64_t GetSystemTimeInTicks(void);
double GetSystemTime(void);

// Get current system time in ticks
uint64_t GetSystemTimeInTicks()
{
	uint64_t ticks;
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	ticks = now.tv_sec;
	ticks *= 1000000000;
	ticks += now.tv_nsec;
	return ticks;
}

static void InitSystemTime()
{
    uint64_t ulBaseTicks = 0;

	systemTimeInitialized = 1;

	systemTicksPerSecond = 1000000000;
	systemSecondsPerTick = 1.0 / systemTicksPerSecond;
	if( ulBaseTicks )
	{
		systemBaseTicks = ulBaseTicks;
	}
	else
	{
		systemBaseTicks = GetSystemTimeInTicks();
	}
}

// Get current system in seconds since first call to a timing function
double GetSystemTime()
{
	if (!systemTimeInitialized)
		InitSystemTime();

	return (GetSystemTimeInTicks() - systemBaseTicks) * systemSecondsPerTick;
}


unsigned int unMessageID = 0;

static int GetTraceMarkerFd()
{
	if ( s_nTraceFileFD >= 0 ) {
		//printf("valid fd\n");
		return s_nTraceFileFD;
	}


    // Could be in either of these two places
    s_nTraceFileFD = open( "/sys/kernel/debug/tracing/trace_marker", O_WRONLY );
    if ( s_nTraceFileFD < 0 )
    {
        s_nTraceFileFD = open( "/sys/kernel/tracing/trace_marker", O_WRONLY );
    }

    if ( s_nTraceFileFD >= 0 )
    {
        printf("Found trace_marker, enabling tracing\n");
    }
    else
    {
        printf("Failed to open trace_marker, disabling tracing\n");
    }

    return s_nTraceFileFD;
}

void EventWriteStringF( const char *pchFormat, ... )
{
	va_list args;
	char pchBuffer[ BUFFER_SIZE ];

	va_start( args, pchFormat );
	vsnprintf( pchBuffer, sizeof( pchBuffer ), pchFormat, args );
	va_end( args );

	EventWriteString( pchBuffer );
}


void EventWriteString( const char *pchEvent)
{
	int traceFileFD = GetTraceMarkerFd();

	if ( traceFileFD >= 0 )
	{
		size_t nBytesToWrite = strlen( pchEvent );
		do {
			ssize_t nBytesWritten = write( traceFileFD, pchEvent, nBytesToWrite );
			if ( nBytesWritten == -1 )
			{
				return;
			}
			nBytesToWrite -= nBytesWritten;

		} while ( nBytesToWrite );
	}

}


void EventWriteStringBegin( const char *pchEvent, uint64_t ulEventId )
{
	char buffer[ BUFFER_SIZE ];
	snprintf( buffer, BUFFER_SIZE, "%s begin_ctx=%" PRIu64, pchEvent, ulEventId );
	EventWriteString( buffer );
}

void EventWriteStringEnd( const char *pchEvent, uint64_t ulEventId )
{
	char buffer[ BUFFER_SIZE ];
	snprintf( buffer, BUFFER_SIZE, "%s end_ctx=%" PRIu64, pchEvent, ulEventId );
	EventWriteString( buffer );
}


uint64_t CScopedEventDurationGetContextID() {
	return ulMessageCounter++;
}

void CScopedEventDurationStart( const char *pchFormat, ... )
{
	va_list args;
	va_start( args, pchFormat );
	vsnprintf( m_buffer, sizeof( m_buffer ), pchFormat, args );
	va_end( args );

	m_unMessageID = CScopedEventDurationGetContextID();

	EventWriteStringBegin( m_buffer, m_unMessageID );
}

void CScopedEventDurationEnd()
{
	EventWriteStringEnd( m_buffer, m_unMessageID );
}


