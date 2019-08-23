/*

Nintendont (Kernel) - Playing Gamecubes in Wii mode on a Wii U

Copyright (C) 2013  crediar

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
#include "string.h"
#include "global.h"
#include "common.h"
#include "alloc.h"
#include "DI.h"
#include "RealDI.h"
#include "ES.h"
#include "SI.h"
#include "BT.h"
#include "lwbt/bte.h"
#include "Stream.h"
#include "HID.h"
#include "EXI.h"
#include "GCNCard.h"
#include "debug.h"
#include "GCAM.h"
#include "TRI.h"
#include "Patch.h"

#include "ReadSpeed.h"

#include "SlippiMemory.h"
#include "SlippiFileWriter.h"
#include "SlippiNetwork.h"
#include "SlippiNetworkBroadcast.h"
#include "net.h"

#include "../common/include/KernelBoot.h"

#ifdef SLIPPI_DEBUG
#include "SlippiDebug.h"
#endif

#include "diskio.h"
#include "usbstorage.h"
#include "SDI.h"
#include "ff_utf8.h"

//#define USE_OSREPORTDM 1

//#undef DEBUG
bool access_led = false;
u32 USBReadTimer = 0;
extern u32 s_size;
extern u32 s_cnt;

/** Device mount/unmount. **/
// 0 == SD, 1 == USB
static FATFS *devices[2];

//this is just a single / as u16, easier to write in hex
static const WCHAR fatSdName[] = {'s', 'd', ':', 0};
static const WCHAR fatUsbName[] = {'u', 's', 'b', ':', 0};

extern u32 SI_IRQ;
extern bool DI_IRQ, EXI_IRQ;
extern u32 WaitForRealDisc;
extern struct ipcmessage DI_CallbackMsg;
extern u32 DI_MessageQueue;
extern vu32 DisableSIPatch;
extern char __bss_start, __bss_end;
extern char __di_stack_addr, __di_stack_size;

u32 virtentry = 0;
u32 drcAddress = 0;
u32 drcAddressAligned = 0;
bool isWiiVC = false;
bool wiiVCInternal = false;

// Global state for network connectivity, from kernel/net.c
extern u32 NetworkStarted;

// Server status, from kernel/SlippiNetwork.c
extern u32 SlippiServerStarted;

// The kernel entrypoint
int _main( int argc, char *argv[] )
{
	/* This kernel image is memcpy()'d into memory by the PPC loader.
	 * When booting with IOS syscall 0x43, presumably there's no guarantee
	 * that .data sections are initialized. It's possible that the memory
	 * we've overlaid the image on-top of is polluted with old data. 
	 * Manually initializing BSS here handles this?
	 */

	memset32(&__bss_start, 0, &__bss_end - &__bss_start);
	sync_after_write(&__bss_start, &__bss_end - &__bss_start);

	//Important to do this as early as possible
	if(read32(0x20109740) == 0xE59F1004)
		virtentry = 0x20109740; //Address on Wii 
	else if(read32(0x2010999C) == 0xE59F1004)
		virtentry = 0x2010999C; //Address on WiiU

	//Use libwiidrc values to detect Wii VC
	sync_before_read((void*)0x12FFFFC0, 0x20);
	isWiiVC = read32(0x12FFFFC0);
	if(isWiiVC)
	{
		drcAddress = read32(0x12FFFFC4); //used in PADReadGC.c
		drcAddressAligned = ALIGN_BACKWARD(drcAddress,0x20);
	}

	s32 ret = 0;
	u32 DI_Thread = 0;


/* ES_INIT BOOT STAGE
 * Flip some bit for enabling the DVD drive. Does HID initialization, although
 * this is unused for Slippi I think?
 */
	BootStatus(ES_INIT, 0, 0);

	if(!isWiiVC)
	{
		//Enable DVD Access
		write32(HW_DIFLAGS, read32(HW_DIFLAGS) & ~DI_DISABLEDVD);
	}

	thread_set_priority( 0, 0x50 );

	//Early HID for loader
	HIDInit();

/* LOADER_SIGNAL BOOT STAGE
 * I think we block here until the user has decided to boot a game.
 */
	BootStatus(LOADER_SIGNAL, 0, 0);
	dbgprintf("Sending signal to loader\r\n");
	mdelay(10);

	// give power button to loader
	set32(HW_GPIO_ENABLE, GPIO_POWER);
	clear32(HW_GPIO_DIR, GPIO_POWER);
	set32(HW_GPIO_OWNER, GPIO_POWER);

	// loader running, selects games
	while(1)
	{
		_ahbMemFlush(1);
		sync_before_read((void*)RESET_STATUS, 0x20);
		vu32 reset_status = read32(RESET_STATUS);
		if(reset_status != 0)
		{
			if(reset_status == 0x0DEA)
				break; //game selected
			else if(reset_status == 0x1DEA)
				goto WaitForExit;
			write32(RESET_STATUS, 0);
			sync_after_write((void*)RESET_STATUS, 0x20);
		}
		HIDUpdateRegisters(1);
		udelay(20);
		cc_ahbMemFlush(1);
	}

	// get time from loader
	InitCurrentTime();

	// get config from loader
	ConfigSyncBeforeRead();

/* STORAGE_INIT BOOT STAGE
 * Do initialization for SD/USB storage devices.
 * USB/SD initialization basically just sets up global state for later.
 * Spawn the RealDI thread if we're booting from the actual DVD drive.
 */
	BootStatus(STORAGE_INIT, 0, 0);

	u32 SlippiFileWrite = ConfigGetConfig(NIN_CFG_SLIPPI_FILE_WRITE);
	u32 UseUSB = ConfigGetUseUSB(); // Returns 0 for SD, 1 for USB
	SetDiskFunctions(UseUSB);


	bool shouldBootUsb = UseUSB || SlippiFileWrite;
	bool shouldBootSd = !UseUSB || SlippiFileWrite;

	// Boot up USB if it is being used for writing slp files OR booting a game
	if (shouldBootUsb)
	{
		ret = USBStorage_Startup();
		dbgprintf("USB:Drive size: %dMB SectorSize:%d\r\n", s_cnt / 1024 * s_size / 1024, s_size);
		if(ret != 1)
		{
			dbgprintf("USB Device Init failed:%d\r\n", ret );
			BootStatusError(-2, ret);
			mdelay(4000);
			Shutdown();
		}
	}

	// Boot up SD if it is being used to boot a game
	if (shouldBootSd)
	{
		s_size = PAGE_SIZE512; //manually set s_size
		ret = SDHCInit();
		if(ret != 1)
		{
			dbgprintf("SD Device Init failed:%d\r\n", ret );
			BootStatusError(-2, ret);
			mdelay(4000);
			Shutdown();
		}
	}

	//Verification if we can read from disc
	//if(memcmp(ConfigGetGamePath(), "di", 3) == 0)
	//{
	//	if(isWiiVC) //will be inited later
	//		wiiVCInternal = true;
	//	else //will shutdown on fail
	//		RealDI_Init();
	//}

/* STORAGE_MOUNT BOOT STAGE
 * Use fatfs to [conditionally] mount any SD/USB storage devices.
 */
	BootStatus(STORAGE_MOUNT, 0, 0);

	s32 res;
	// Mount SD card
	if (shouldBootSd)
	{
		devices[0] = (FATFS*)malloca( sizeof(FATFS), 32 );
		res = f_mount( devices[0], fatSdName, 1 );
		if( res != FR_OK )
		{
			dbgprintf("ES:f_mount() failed:%d\r\n", res );
			BootStatusError(-3, res);
			mdelay(4000);
			Shutdown();
		}
	}

	// Mount USB drive
	if (shouldBootUsb)
	{
		devices[1] = (FATFS*)malloca( sizeof(FATFS), 32 );
		res = f_mount( devices[1], fatUsbName, 1 );
		if( res != FR_OK )
		{
			dbgprintf("ES:f_mount() failed:%d\r\n", res );
			BootStatusError(-3, res);
			mdelay(4000);
			Shutdown();
		}
	}

/* BOOT_STATUS_4 BOOT STAGE
 * Unused right now. I think this is just some vestigial code.
 */
	BootStatus(BOOT_STATUS_4, 0, 0);


/* STORAGE_CHECK BOOT STAGE
 * Tests disk accesses on the main drive.
 * Presumably, as long as we don't return FR_DISK_ERR, everything is OK?
 */
	BootStatus(STORAGE_CHECK, 0, 0);

	dbgprintf("About to load bladie\r\n");

	FIL fp;
	s32 fres = f_open_main_drive(&fp, "/bladie", FA_READ|FA_OPEN_EXISTING);

	dbgprintf("Loaded bladie: %d\r\n", fres);

	switch (fres)
	{
		case FR_OK:
			f_close(&fp);
			break;

		case FR_NO_PATH:
		case FR_NO_FILE:
			fres = FR_OK;
			break;

		default:
		case FR_DISK_ERR:
			BootStatusError(-5, fres);
			mdelay(4000);
			Shutdown();
			break;
	}

	if(!UseUSB) //Use FAT values for SD
		s_cnt = devices[0]->n_fatent * devices[0]->csize;

/* NETWORK_INIT BOOT STAGE.
 * If a user has Slippi networking enabled, initialize the network.
 * Then, spawn the Slippi networking threads.
 */
	u32 UseNetwork = ConfigGetConfig(NIN_CFG_NETWORK);
	if (UseNetwork == 1)
	{
		BootStatus(NETWORK_INIT, s_size, s_cnt);
		NCDInit();
		NetworkStarted = 1;
		SlippiNetworkInit();
#ifdef SLIPPI_DEBUG
		SlippiDebugInit();
#endif
		//SlippiNetworkBroadcastInit();
	}


/* CONFIG_INIT BOOT STAGE
 * Double check that we've read a copy of the Nintendont config into memory
 * (this should be handled by the loader). If it doesn't exist, read it into 
 * memory from the main drive.
 */
	BootStatus(CONFIG_INIT, s_size, s_cnt);
	ConfigInit();

	if (ConfigGetConfig(NIN_CFG_LOG))
		SDisInit = 1;  // Looks okay after threading fix
	dbgprintf("Game path: %s\r\n", ConfigGetGamePath());

/* BOOT_STATUS_8 BOOT STAGE
 * I don't know what this does. Clears out these regions in memory?
 */
	BootStatus(BOOT_STATUS_8, s_size, s_cnt);

	// ???
	memset32((void*)RESET_STATUS, 0, 0x20);
	sync_after_write((void*)RESET_STATUS, 0x20);

	// Clear PadBuff data (???)
	memset32((void*)0x13003100, 0, 0x30);
	sync_after_write((void*)0x13003100, 0x30);

	// Relevant to patches for OSReport output? 
	memset32((void*)0x13160000, 0, 0x20);
	sync_after_write((void*)0x13160000, 0x20);

	// Clear fake interrupt regions
	memset32((void*)0x13026500, 0, 0x100);
	sync_after_write((void*)0x13026500, 0x100);

/* DI_INIT BOOT STAGE
 * Spawn the DI thread, then DI thread initialization. 
 */
	BootStatus(DI_INIT, s_size, s_cnt);

	DIRegister();
	DI_Thread = do_thread_create(DIReadThread, 
		((u32*)&__di_stack_addr), 
		((u32)(&__di_stack_size)), 
		0x78
	);
	thread_continue(DI_Thread);
	DIinit(true);

/* CARD_INIT BOOT STAGE
 * Initialize TRI Arcade. Initializes EXI/memcard emulation?
 */
	BootStatus(CARD_INIT, s_size, s_cnt);

	TRIInit();
	EXIInit();

/* BOOT_STATUS_11 BOOT STAGE
 * Initialize SI and streaming audio. Don't know what PatchInit() does.
 */
	BootStatus(BOOT_STATUS_11, s_size, s_cnt);

	SIInit();
	StreamInit();
	PatchInit();
	SlippiMemoryInit();

	// If we are using USB for writting slp files and USB is not the
	// primary device, initialize the file writer
	//if (SlippiFileWrite == 1)
	//	SlippiFileWriterInit();

/* KERNEL_RUNNING BOOT STAGE
 * Signal the loader to start booting a game in PPC-world.
 * After this, the kernel waits in the main loop (later in this function).
 * It's exactly not clear what the mdelay()'s are here for.
 */
	//Tell PPC side we are ready!
	cc_ahbMemFlush(1);
	mdelay(1000);
	BootStatus(KERNEL_RUNNING, s_size, s_cnt);
	mdelay(1000); //wait before hw flag changes
	dbgprintf("Kernel Start\r\n");
	dbgprintf("Main Thread ID: %d\r\n", thread_get_id());

#ifdef USE_OSREPORTDM
	write32( 0x1860, 0xdeadbeef );	// Clear OSReport area
	sync_after_write((void*)0x1860, 0x20);
#endif

	u32 Now = read32(HW_TIMER);
	u32 PADTimer = Now;
	u32 DiscChangeTimer = Now;
	u32 ResetTimer = Now;
	u32 InterruptTimer = Now;

#ifdef PERFMON
	u32 loopCnt = 0;
	u32 loopPrintTimer = Now;
#endif

	USBReadTimer = Now;
	u32 Reset = 0;
	bool SaveCard = false;

	//enable ios led use
	access_led = ConfigGetConfig(NIN_CFG_LED);
	if(access_led)
	{
		set32(HW_GPIO_ENABLE, GPIO_SLOT_LED);
		clear32(HW_GPIO_DIR, GPIO_SLOT_LED);
		clear32(HW_GPIO_OWNER, GPIO_SLOT_LED);
	}

	set32(HW_GPIO_ENABLE, GPIO_SENSOR_BAR);
	clear32(HW_GPIO_DIR, GPIO_SENSOR_BAR);
	clear32(HW_GPIO_OWNER, GPIO_SENSOR_BAR);
	set32(HW_GPIO_OUT, GPIO_SENSOR_BAR);	//turn on sensor bar

	clear32(HW_GPIO_OWNER, GPIO_POWER); //take back power button

	write32( HW_PPCIRQMASK, (1<<30) ); //only allow IPC IRQ
	write32( HW_PPCIRQFLAG, read32(HW_PPCIRQFLAG) );

	//This bit seems to be different on japanese consoles
	u32 ori_ppcspeed = read32(HW_PPCSPEED);
	switch (BI2region)
	{
		case BI2_REGION_JAPAN:
		case BI2_REGION_SOUTH_KOREA:
		default:
			// JPN games.
			set32(HW_PPCSPEED, (1<<17));
			break;

		case BI2_REGION_USA:
		case BI2_REGION_PAL:
			// USA/PAL games.
			clear32(HW_PPCSPEED, (1<<17));
			break;
	}

	// Set the Wii U widescreen setting.
	u32 ori_widesetting = 0;
	if (IsWiiU())
	{
		ori_widesetting = read32(0xd8006a0);
		//if( ConfigGetConfig(NIN_CFG_WIIU_WIDE) )
		//	write32(0xd8006a0, 0x30000004);
		//else
			write32(0xd8006a0, 0x30000002);
		mask32(0xd8006a8, 0, 2);
	}

	// Main kernel loop
	while (1)
	{
		_ahbMemFlush(0);

#ifdef PERFMON
		loopCnt++;
		if(TimerDiffTicks(loopPrintTimer) > 1898437)
		{
			dbgprintf("%08i\r\n",loopCnt);
			loopPrintTimer = read32(HW_TIMER);
			loopCnt = 0;
		}
#endif

		//Does interrupts again if needed
		if(TimerDiffTicks(InterruptTimer) > 15820) //about 120 times a second
		{
			sync_before_read((void*)INT_BASE, 0x80);
			if((read32(RSW_INT) & 2) || (read32(DI_INT) & 4) || 
				(read32(SI_INT) & 8) || (read32(EXI_INT) & 0x10))
				write32(HW_IPC_ARMCTRL, 8); //throw irq
			InterruptTimer = read32(HW_TIMER);
		}

#ifdef PATCHALL
		if (EXI_IRQ == true)
		{
			if(EXICheckTimer())
				EXIInterrupt();
		}
#endif

		if (SI_IRQ != 0)
		{
			// About 240 times a second
			if ((TimerDiffTicks(PADTimer) > 7910) || (SI_IRQ & 0x2))
			{
				SIInterrupt();
				PADTimer = read32(HW_TIMER);
			}
		}

		// If we're in the middle of servicing a DI IRQ, periodically check to
		// see if DIReadThread has ACKed the IOCTL and completed the work
		if(DI_IRQ == true)
		{
			// If DIReadThread is done with the work, try to clear the interrupt.
			// Otherwise, do some work for approximately "the atomic period."
			//
			// If DIInterrupt() sees that we are at least "an atomic period"
			// away from the ReadSpeed model's target time, it will simply block
			// until it's time to complete the read.

			if (DiscCheckAsync()) 
				DIInterrupt();
			else 
				udelay(DI_IRQ_ATOMIC_PERIOD_US);
		}
		// DI IRQ indicated that we might read asynchronously, so either we
		// "do some work in DIReadThread" OR "spend time writing to the card"
		else if(SaveCard == true)
		{
			if(TimerDiffSeconds(Now) > 2) /* after 3 second earliest */
			{
				GCNCard_Save();
				SaveCard = false;
			}
		}

		else if(UseUSB && TimerDiffSeconds(USBReadTimer) > 149) /* Read random sector every 2 mins 30 secs */
		{
			DIFinishAsync(); //if something is still running
			DI_CallbackMsg.result = -1;
			sync_after_write(&DI_CallbackMsg, 0x20);
			IOS_IoctlAsync( DI_Handle, 2, NULL, 0, NULL, 0, DI_MessageQueue, &DI_CallbackMsg );
			DIFinishAsync();
			USBReadTimer = read32(HW_TIMER);
		}
		else /* No device I/O so make sure this stays updated */
			GetCurrentTime();

		udelay(20); //wait for other threads

		if( WaitForRealDisc == 1 )
		{
			if(RealDI_NewDisc())
			{
				DiscChangeTimer = read32(HW_TIMER);
				WaitForRealDisc = 2; //do another flush round, safety!
			}
		}
		else if( WaitForRealDisc == 2 )
		{
			if(TimerDiffSeconds(DiscChangeTimer))
			{
				//identify disc after flushing everything
				RealDI_Identify(false);
				//clear our fake regs again
				sync_before_read((void*)DI_BASE, 0x40);
				write32(DI_IMM, 0);
				write32(DI_COVER, 0);
				sync_after_write((void*)DI_BASE, 0x40);
				//mask and clear interrupts
				write32( DIP_STATUS, 0x54 );
				//disable cover irq which DIP enabled
				write32( DIP_COVER, 4 );
				DIInterrupt();
				WaitForRealDisc = 0;
			}
		}

		if ( DiscChangeIRQ == 1 )
		{
			DiscChangeTimer = read32(HW_TIMER);
			DiscChangeIRQ = 2;
		}
		else if ( DiscChangeIRQ == 2 )
		{
			if ( TimerDiffSeconds(DiscChangeTimer) > 2 )
			{
				DIInterrupt();
				DiscChangeIRQ = 0;
			}
		}
		_ahbMemFlush(1);

		// Check to see if there are any outstanding disc requests.
		// If so, dispatch an IOCTL to DIReadThread and set DI_IRQ
		DIUpdateRegisters();

#ifdef PATCHALL
		EXIUpdateRegistersNEW();

		// We most likely don't need any of these for our use case
		//GCAMUpdateRegisters();
		//BTUpdateRegisters();
		//HIDUpdateRegisters(0);
		//if(DisableSIPatch == 0) SIUpdateRegisters();

#endif
		StreamUpdateRegisters();

		CheckOSReport();
		if(GCNCard_CheckChanges())
		{
			Now = read32(HW_TIMER);
			SaveCard = true;
		}
		sync_before_read((void*)RESET_STATUS, 0x20);
		vu32 reset_status = read32(RESET_STATUS);
		if (reset_status == 0x1DEA)
		{
			dbgprintf("Game Exit\r\n");
			DIFinishAsync();
			break;
		}
		if (reset_status == 0x3DEA)
		{
			if (Reset == 0)
			{
				dbgprintf("Fake Reset IRQ\r\n");
				write32( RSW_INT, 0x2 ); // Reset irq
				sync_after_write( (void*)RSW_INT, 0x20 );
				write32(HW_IPC_ARMCTRL, 8); //throw irq
				Reset = 1;
			}
		}
		else if (Reset == 1)
		{
			write32( RSW_INT, 0x10000 ); // send pressed
			sync_after_write( (void*)RSW_INT, 0x20 );
			ResetTimer = read32(HW_TIMER);
			Reset = 2;
		}
		/* The cleanup is not connected to the button press */
		if (Reset == 2)
		{
			if (TimerDiffTicks(ResetTimer) > 949219) //free after half a second
			{
				write32( RSW_INT, 0 ); // done, clear
				sync_after_write( (void*)RSW_INT, 0x20 );
				Reset = 0;
			}
		}
		if(reset_status == 0x4DEA)
			PatchGame();
		if(reset_status == 0x5DEA)
		{
			SetIPL();
			PatchGame();
		}
		if(reset_status == 0x6DEA)
		{
			SetIPL_TRI();
			write32(RESET_STATUS, 0);
			sync_after_write((void*)RESET_STATUS, 0x20);
		}
		if(reset_status == 0x7DEA || (read32(HW_GPIO_IN) & GPIO_POWER))
		{
			DIFinishAsync();
			#ifdef PATCHALL
			BTE_Shutdown();
			#endif
			Shutdown();
		}

#ifdef USE_OSREPORTDM
		sync_before_read( (void*)0x1860, 0x20 );
		if( read32(0x1860) != 0xdeadbeef )
		{
			if( read32(0x1860) != 0 )
			{
				dbgprintf(	(char*)(P2C(read32(0x1860))),
							(char*)(P2C(read32(0x1864))),
							(char*)(P2C(read32(0x1868))),
							(char*)(P2C(read32(0x186C))),
							(char*)(P2C(read32(0x1870))),
							(char*)(P2C(read32(0x1874)))
						);
			}
			write32(0x1860, 0xdeadbeef);
			sync_after_write( (void*)0x1860, 0x20 );
		}
#endif

		cc_ahbMemFlush(1);
	}
	HIDClose();
	IOS_Close(DI_Handle); //close game
	thread_cancel(DI_Thread, 0);
	DIUnregister();

	if( ConfigGetConfig(NIN_CFG_MEMCARDEMU) )
		EXIShutdown();

	SlippiNetworkShutdown();
	SlippiFileWriterShutdown();

	if (ConfigGetConfig(NIN_CFG_LOG))
		closeLog();

#ifdef PATCHALL
	BTE_Shutdown();
#endif

	// unmount SD card
	if (shouldBootSd)
	{
		f_mount(NULL, fatSdName, 1);
		free(devices[0]);
		devices[0] = NULL;
		SDHCShutdown();
	}

	// unmount USB drive
	if (shouldBootUsb)
	{
		f_mount(NULL, fatUsbName, 1);
		free(devices[1]);
		devices[1] = NULL;
		USBStorage_Shutdown();
	}

//make sure drive led is off before quitting
	if( access_led ) clear32(HW_GPIO_OUT, GPIO_SLOT_LED);

//make sure we set that back to the original
	write32(HW_PPCSPEED, ori_ppcspeed);

	if (IsWiiU())
	{
		write32(0xd8006a0, ori_widesetting);
		mask32(0xd8006a8, 0, 2);
	}
WaitForExit:
	/* Allow all IOS IRQs again */
	write32(HW_IPC_ARMCTRL, 0x36);
	/* Wii VC is unable to cleanly use ES */
	if(isWiiVC)
	{
		dbgprintf("Force reboot into WiiU Menu\n");
		WiiUResetToMenu();
	}
	else
	{
		dbgprintf("Kernel done, waiting for IOS Reload\n");
		write32(RESET_STATUS, 0);
		sync_after_write((void*)RESET_STATUS, 0x20);
	}
	while(1) mdelay(100);
	return 0;
}
