/*
ReadSpeed.h for Nintendont (Kernel)

Copyright (C) 2015 FIX94

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation version 2.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/
#include "Config.h"
#include "ReadSpeed.h"
#include "debug.h"

// This file exists to emulate the disc read speed
// The data used comes from my D2C wii disc drive

static u32 SEEK_TICKS = 94922; // 50 ms
static float READ_TICKS = 1.657f; // 3 MB/s
static const u32 READ_BLOCK = 65536; // 64 KB
static const float CACHE_TICKS = 8.627f; // 15.6 MB/s
static const u32 CACHE_SIZE = 1048576; // 1 MB
#define MIN(a, b)		(((a)>(b))?(b):(a))

static u32 CMDStartTime = 0;
static u32 CMDLastFinish = 0;
static u32 CMDTicks = UINT_MAX;
static u32 CMDBaseBlock = UINT_MAX;
static u32 CMDLastBlock = UINT_MAX;

extern u32 TITLE_ID;

void ReadSpeed_Init()
{
	if(ConfigGetConfig(NIN_CFG_REMLIMIT))
		dbgprintf("ReadSpeed:Disabled\r\n");
	CMDStartTime = 0;
	CMDLastFinish = 0;
	CMDTicks = UINT_MAX;
	CMDBaseBlock = UINT_MAX;
	CMDLastBlock = UINT_MAX;

	if(TITLE_ID == 0x47574B) //King Kong
	{
		dbgprintf("ReadSpeed:Using Slow Settings\r\n");
		SEEK_TICKS = 284765; // 150 ms
		READ_TICKS = 1.1f; // 2 MB/s
	}
}

u32 UseReadLimit = 1;
extern vu32 TRIGame;
extern u32 RealDiscCMD;
void ReadSpeed_Start()
{
	if(UseReadLimit == 0)
		return;

	CMDStartTime = read32(HW_TIMER);
}

void ReadSpeed_Motor()
{
	if(UseReadLimit == 0)
		return;

	CMDStartTime = read32(HW_TIMER);
	CMDTicks = SEEK_TICKS;
}

// Ignore Nintendont's disc cache emulation in our model
void ReadSpeed_Setup(u32 Offset, int Length)
{
	// Ignore this if the model is disabled
	if(UseReadLimit == 0) return;

	u32 CurrentBlock = ALIGN_BACKWARD(Offset, READ_BLOCK);
	CMDTicks = (Length / READ_TICKS) + SEEK_TICKS;
	CMDBaseBlock = ALIGN_BACKWARD(Offset+Length, READ_BLOCK);
	return;
}


//void ReadSpeed_Setup(u32 Offset, int Length)
//{
//	if(UseReadLimit == 0)
//		return;
//
//	u32 CurrentBlock = ALIGN_BACKWARD(Offset, READ_BLOCK);
//	if(CurrentBlock < CMDBaseBlock || //always seek and read
//		((Offset+Length) - CMDBaseBlock) > CACHE_SIZE)
//	{
//		CMDTicks = (Length / READ_TICKS) + SEEK_TICKS;
//		//dbgprintf("Reading uncached, %u ticks\r\n", CMDTicks);
//		CMDBaseBlock = ALIGN_BACKWARD(Offset+Length, READ_BLOCK);
//		return;
//	}
//	CMDTicks = 0; //start from fresh
//
//	u32 lenCached = MIN(TimerDiffTicks(CMDLastFinish) * READ_TICKS, CACHE_SIZE);
//	u32 CachedUpToOffset = CMDBaseBlock + READ_BLOCK + lenCached;
//
//	if(CachedUpToOffset > CurrentBlock)
//	{
//		int CacheUsableLen = CachedUpToOffset - CurrentBlock;
//		int CacheLen = MIN(Length, CacheUsableLen);
//		if(CacheLen > 0)
//		{
//			CMDTicks += CacheLen / CACHE_TICKS; //whats cached
//			Length -= CacheLen; //whats left to read
//		}
//		//dbgprintf("%i %i %u\r\n", CacheUsableLen, Length, CMDTicks);
//	}
//	if(Length > 0)
//		CMDTicks += Length / READ_TICKS;
//
//	//dbgprintf("Reading possibly cached, %u ticks\r\n", CMDTicks);
//	if((CurrentBlock - CMDBaseBlock) > READ_BLOCK)
//	{	//if more than a block apart
//		CMDBaseBlock = ALIGN_BACKWARD(Offset+Length, READ_BLOCK);
//	}
//}


u32 ReadSpeed_End()
{
	// Ignore this function if we aren't using the model
	if (UseReadLimit == 0) return 1;

	u32 readTicks;
	u32 ticksLeftUs;

	if (CMDTicks < UINT_MAX)
	{
		// How many ticks has this read taken?
		readTicks = TimerDiffTicks(CMDStartTime);

		// If we've already passed the target, complete the disc read.
		// We need to handle this case, but we want to minimize the
		// possibility of this happening.
		//
		// Otherwise, if we haven't reached the target timing, check
		// if we need to block in order to hit the target timing.

		if (readTicks > CMDTicks)
		{
			//dbgprintf("%dus, want %dus (overshot)\n", TicksToUs(readTicks), TicksToUs(CMDTicks));
			CMDTicks = UINT_MAX;
			return 1;
		}
		else
		{
			// How many microseconds until we reach the target?
			ticksLeftUs = TicksToUs(CMDTicks - readTicks);

			// If an "atomic" period is left, do not go off-CPU;
			// just block here until we reach the target timing.
			//
			// Otherwise, the target is far enough ahead that we
			// can go off-CPU without worrying about hitting the
			// target timing when we come back on-CPU later.

			if (ticksLeftUs < DI_IRQ_ATOMIC_PERIOD_US)
			{
				while (readTicks < CMDTicks)
				{
					readTicks = TimerDiffTicks(CMDStartTime);
				}
				//dbgprintf("%dus, want %dus (blocked)\n", TicksToUs(readTicks), TicksToUs(CMDTicks));
				CMDTicks = UINT_MAX;
				return 1;
			}
			else
			{
				return 0;
			}
		}
	}
	return 1;
}
