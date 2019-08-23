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
#ifndef _READSPEED_H_
#define _READSPEED_H_

// This is the smallest possible timeframe we are willing to let Nintendont's
// kernel thread go off-CPU for (so the DI thread can do the work requred to
// service a disc interrupt). When the main kernel thread is less than this
// many microseconds away from reaching the target number of ticks (CMDTicks),
// we simply block the in the context of the Nintendont kernel thread until
// we've hit the target timing.

#define DI_IRQ_ATOMIC_PERIOD_US 100

void ReadSpeed_Init();
void ReadSpeed_Motor();
void ReadSpeed_Start();
void ReadSpeed_Setup(u32 Offset, int Length);
u32 ReadSpeed_End();

#endif
