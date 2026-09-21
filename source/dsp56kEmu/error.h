#pragma once

#include "dsp56kBase/dspassert.h"
#include "dsp56kBase/logging.h"

namespace dsp56k
{
}

#define LOG_ERR_BASE(A,ERR)					\
{											\
	std::stringstream ss;	ss << __func__ << "@" << __LINE__ << ": " << "DSP 56300 ERROR: " << ERR;		\
	LOGTOCONSOLE( ss );						\
	assertf( A, ss.str().c_str() );			\
}

// emulation errors
#define LOG_ERR_NOTIMPLEMENTED(T)					LOG_ERR_BASE( false, "Not implemented: " << T )

// memory errors
//
// An out-of-range access is per-INSTRUCTION, so a firmware that does it at all does
// it millions of times per second: the Virus TI emits 7.8M of these for a 2 s render.
// LOG_ERR_BASE builds and formats a std::stringstream every time, which cost more
// than the emulation itself -- the TI measured 0.478x real time with these enabled
// and 1.019x with them compiled out, a 2.13x difference, with sys time falling from
// 11.52 s to 0.42 s. Keep them in debug builds, where the diagnostic is the point.
#ifdef _DEBUG
#define LOG_ERR_MEM_WRITE(ADDR)						LOG_ERR_BASE( true, "Memory Write: " << HEX(ADDR) )
#define LOG_ERR_MEM_READ(ADDR)						LOG_ERR_BASE( true, "Memory Read: " << HEX(ADDR) )
#else
#define LOG_ERR_MEM_WRITE(ADDR)						do{}while(0)
#define LOG_ERR_MEM_READ(ADDR)						do{}while(0)
#endif
#define LOG_ERR_MEM_READ_UNINITIALIZED(AREA,ADDR)	LOG_ERR_BASE( true, "Read uninitialized memory: area " << AREA << " @" << HEX(ADDR) )
#define LOG_ERR_DSP_ILLEGALINSTRUCTION(OP)			LOG_ERR_BASE( false, "Illegal instruction: " << HEX(OP) )
