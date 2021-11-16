//===================== Copyright (c) Valve Corporation. All Rights Reserved. ======================

#include <atomic>
#include <cinttypes>
#include <cstdarg>
#include <cstdint>

#include <stdlib.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <cwchar>
#include <pthread.h>
#include <string.h>
#include <stdio.h>

#include "perftools.h"

static bool systemTimeInitialized = false;
static uint64_t systemTicksPerSecond = 0;
static double systemSecondsPerTick = 0;
static uint64_t systemBaseTicks = 0;

bool tracingActive = false;

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

uint64_t EventGetSystemTimeInTicks() {
	return GetSystemTimeInTicks();
}

static void InitSystemTime( uint64_t ulBaseTicks = 0 )
{
	systemTimeInitialized = true;

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


static int GetTraceMarkerFd()
{
	static int s_nTraceFileFD = -1;
	static double s_flLastAttemptTime = 0;
	//gabe: skip mutex since there are a zillion dependencies to include this from steamvr tools
	//static CThreadMutex s_TraceMarkerMutex;

	// Access unlocked is okay since we have a strict write once->read
	// usage pattern. And we don't care if the missing memory barrier
	// results in a few extra entrances to the locked block
	if ( s_nTraceFileFD >= 0 )
		return s_nTraceFileFD;

	// Only try to re-open once every few seconds. Otherwise we will end
	// up performing some unnecessary syscalls calls. Since EventWriteString
	// is called multiple times per frame this overhead adds up.
	// We need the retry logic because gpu-trace.sh might take a while to
	// mount the debug/tracing filesystem
	double flTimeSinceLastAttempt = GetSystemTime() - s_flLastAttemptTime;
	if ( flTimeSinceLastAttempt < 1.0f ) {
		return s_nTraceFileFD;
	}

	{
		//gabe: skip mutex since there are a zillion dependencies 
		//AUTO_LOCK( s_TraceMarkerMutex );

		// The first checks were unlocked, so it may have used stale data. Double
		// check our results once we are in mutex land. The first check is useful
		// since for most of the 5 seconds backoff period we will be hitting that
		// early out.
		double flCurrentAttemptTime = GetSystemTime();
		flTimeSinceLastAttempt = flCurrentAttemptTime - s_flLastAttemptTime;
		if ( s_nTraceFileFD >= 0 || flTimeSinceLastAttempt < 1.0f )
			return s_nTraceFileFD;
		s_flLastAttemptTime = flCurrentAttemptTime;

		// Could be in either of these two places
		s_nTraceFileFD = open( "/sys/kernel/debug/tracing/trace_marker", O_WRONLY );
		if ( s_nTraceFileFD < 0 )
		{
			s_nTraceFileFD = open( "/sys/kernel/tracing/trace_marker", O_WRONLY );
			
		}

		if ( s_nTraceFileFD >= 0 && !tracingActive)
		{
			//ExecuteOnce( Log( LogInfo, "Found trace_marker, enabling tracing\n" ) );
			printf("perftools: tracing OK! %d\n",s_nTraceFileFD);
			tracingActive = true;
		}
		else
		{
			//ExecuteOnce( Log( LogError, "Failed to open trace_marker, disabling tracing\n" ) );
		}
	}

	return s_nTraceFileFD;
}

void EventWriteString( const char *pchEvent, EVRTraceEvent eType )
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

#if defined(ENABLE_LOGGING)
	//Log( "%s\n", pchEvent );
#endif
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

bool VrInitGpuProfiling()
{
	return true;
}


uint64_t GetNextEventId()
{
	static std::atomic<uint64_t> ulMessageCounter{ 1 };
	return ulMessageCounter++;
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

CScopedEventDuration::CScopedEventDuration( const char *pchFormat, ... )
{
	va_list args;
	va_start( args, pchFormat );
	vsnprintf( m_buffer, sizeof( m_buffer ), pchFormat, args );
	va_end( args );

	m_unMessageID = GetNextEventId();

	EventWriteStringBegin( m_buffer, m_unMessageID );
}

CScopedEventDuration::~CScopedEventDuration()
{
	EventWriteStringEnd( m_buffer, m_unMessageID );
}
