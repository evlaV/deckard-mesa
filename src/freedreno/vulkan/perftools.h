//========= Copyright Valve Corporation ============//
#pragma once

#include <cstdint>

#define BUFFER_SIZE 256

enum EVRTraceEvent {
	VRTraceEvent_Normal = 0,
	VRTraceEvent_Severe = 1,
	VRTraceEvent_Camera = 2
};

#if defined(__clang__) || defined(COMPILER_GCC)
#ifndef ATTR_FORMAT
// Used to tell the compiler to validate a format string and associated arguments.
// type: Can be scanf or printf.
// format_pos: The argument position of the format string.
// arg_pos: The argument position where the variadic arguments start.
//          Set to 0 if no variadic args are present, e.g. va_list cases.
#define ATTR_FORMAT(type,format_pos,arg_pos) __attribute__((format(printf,format_pos,arg_pos)))
#endif
#else
#ifndef ATTR_FORMAT
// Only available on clang and gcc, no msvc equivalent
#define ATTR_FORMAT(type,format_pos,arg_pos)
#endif
#endif

#define BUFFER_SIZE 256

// For tracking events in xperf, gpuview, etc.
void EventWriteString( const char *pwchEvent, EVRTraceEvent eType = EVRTraceEvent::VRTraceEvent_Normal );
void EventWriteStringF(const char *pchFormat, ...) ATTR_FORMAT(printf, 1, 2);

// For events that have a duration
uint64_t GetNextEventId();
void EventWriteStringBegin( const char *pchEvent, uint64_t ulEventId);
void EventWriteStringEnd( const char *pchEvent, uint64_t ulEventId );

// For any kind of initialization required for gpu profiling, e.g. register etw manifest
bool VrInitGpuProfiling();

uint64_t EventGetSystemTimeInTicks();

class CScopedEventDuration
{
public:
	CScopedEventDuration( const char *pchFormat, ... );
	~CScopedEventDuration();
	
private:
	char m_buffer[ BUFFER_SIZE ];
	uint64_t m_unMessageID;
};

