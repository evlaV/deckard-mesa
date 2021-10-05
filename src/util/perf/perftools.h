//========= Copyright Valve Corporation ============//
#pragma once

#include <stdint.h>

#define BUFFER_SIZE 256

typedef enum  {
	VRTraceEvent_Normal = 0,
	VRTraceEvent_Severe = 1,
	VRTraceEvent_Camera = 2
} EVRTraceEvent;

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
void EventWriteStringF(const char *pchFormat, ...) ATTR_FORMAT(printf, 1, 2);


// For tracking events in xperf, gpuview, etc.
void EventWriteString( const char *pwchEvent);

// For events that have a duration
void EventWriteStringBegin( const char *pchEvent, uint64_t ulEventId);
void EventWriteStringEnd( const char *pchEvent, uint64_t ulEventId );

uint64_t CScopedEventDurationGetContextID(void);
void CScopedEventDurationStart( const char *pchFormat, ... );
void CScopedEventDurationEnd(void);

#define EventDurationStart(x,dstr) uint64_t x = CScopedEventDurationGetContextID(); char ctxstr_##x[]=dstr; EventWriteStringBegin(ctxstr_##x,x); 
#define EventDurationStop(x) EventWriteStringEnd(ctxstr_##x,x) 