/* SlippiNetwork.c
 * Slippi thread for handling network transactions.
 */

#include "SlippiNetwork.h"
#include "SlippiDebug.h"
#include "SlippiMemory.h"
#include "common.h"
#include "string.h"
#include "debug.h"
#include "net.h"
#include "ff_utf8.h"

// Game can transfer at most 784 bytes / frame
// That means 4704 bytes every 100 ms. Let's aim to handle
// double that, making our read buffer 10000 bytes
#define READ_BUF_SIZE 10000
#define THREAD_CYCLE_TIME_MS 100

// Thread stuff
static u32 SlippiNetwork_Thread;
extern char __slippi_network_stack_addr, __slippi_network_stack_size;
static u32 SlippiNetworkHandlerThread(void *arg);

// Connection variables
static struct sockaddr_in server __attribute__((aligned(32)));
static int server_sock __attribute__((aligned(32)));
static int client_sock __attribute__((aligned(32)));
static u32 client_alive_ts;

// Global network state
extern s32 top_fd;			 // from kernel/net.c
u32 SlippiServerStarted = 0; // used by kernel/main.c

/* Dispatch the server thread. This should only be run once in kernel/main.c
 * after NCDInit() has actually brought up the networking stack and we have
 * some connectivity. */
void SlippiNetworkShutdown() { thread_cancel(SlippiNetwork_Thread, 0); }
s32 SlippiNetworkInit()
{
	server_sock = -1;
	client_sock = -1;

	dbgprintf("net_thread is starting ...\r\n");
	SlippiNetwork_Thread = do_thread_create(
		SlippiNetworkHandlerThread,
		((u32 *)&__slippi_network_stack_addr),
		((u32)(&__slippi_network_stack_size)),
		0x78);
	thread_continue(SlippiNetwork_Thread);
	SlippiServerStarted = 1;
	ppc_msg("SERVER INIT OK\x00", 15);
	return 0;
}

/* Create a new socket for the server to bind and listen on. */
s32 startServer()
{
	s32 res;

	server_sock = socket(top_fd, AF_INET, SOCK_STREAM, IPPROTO_IP);
	dbgprintf("server_sock: %d\r\n", server_sock);

	memset(&server, 0, sizeof(server));
	server.sin_family = AF_INET;
	server.sin_port = 666;
	server.sin_addr.s_addr = INADDR_ANY;

	res = bind(top_fd, server_sock, (struct sockaddr *)&server);
	if (res < 0)
	{
		close(top_fd, server_sock);
		server_sock = -1;
		dbgprintf("bind() failed with: %d\r\n", res);
		return res;
	}
	res = listen(top_fd, server_sock, 1);
	if (res < 0)
	{
		close(top_fd, server_sock);
		server_sock = -1;
		dbgprintf("listen() failed with: %d\r\n", res);
		return res;
	}
	return 0;
}

/* Accept a client */
void listenForClient()
{
	if (client_sock >= 0)
	{
		// We already have a client
		return;
	}

	client_sock = accept(top_fd, server_sock);
	if (client_sock >= 0)
	{
		ppc_msg("CLIENT OK\x00", 10);
		client_alive_ts = read32(HW_TIMER);
	}
}

static u8 readBuf[READ_BUF_SIZE];
static u64 memReadPos = 0;
static SlpGameReader reader;
static char memerr[64];
/* Deal with sending Slippi data over the network. */
s32 handleFileTransfer()
{

	SlpMemError err = SlippiMemoryRead(&reader, readBuf, READ_BUF_SIZE, memReadPos);
	if (err)
	{
		_sprintf(memerr, "SLPMEMERR: %d\x00", err);
		ppc_msg(memerr, 13);
		client_alive_ts = 0;
		close(top_fd, client_sock);
		client_sock = -1;
		return -1;
		//mdelay(1000);
	}

	u32 bytesRead = reader.lastReadResult.bytesRead;
	if (bytesRead == 0)
		return 0;

	s32 res = sendto(top_fd, client_sock, readBuf, bytesRead, 0);

	// Naive client hangup detection
	if (res < 0)
	{
		close(top_fd, client_sock);
		client_sock = -1;
		client_alive_ts = 0;
		ppc_msg("CLIENT HUP\x00", 11);
		return res;
	}

	// Only update read position if transfer was successful
	memReadPos += bytesRead;

	return 0;
}

/* Return the status of the networking thread. */
int getConnectionStatus()
{
	if (server_sock < 0)
		return CONN_STATUS_NO_SERVER;
	if (client_sock < 0)
		return CONN_STATUS_NO_CLIENT;
	else if (client_sock >= 0)
		return CONN_STATUS_CONNECTED;

	return CONN_STATUS_UNKNOWN;
}

/* Give some naive indication of client hangup. If sendto() returns some error,
 * this probably indicates that we can stop talking to the current client */
static char alive_msg[] __attribute__((aligned(32))) = "HELO";
s32 checkAlive(void)
{
	if (TimerDiffSeconds(client_alive_ts) < 3)
	{
		// Only check alive if we haven't detected any communication
		return 0;
	}

	s32 res;
	res = sendto(top_fd, client_sock, alive_msg, sizeof(alive_msg), 0);

	if (res == sizeof(alive_msg))
	{
		client_alive_ts = read32(HW_TIMER);
		return 0;
	}
	else if (res <= 0)
	{
		client_alive_ts = 0;
		close(top_fd, client_sock);
		client_sock = -1;
		ppc_msg("CLIENT HUP\x00", 11);
		return -1;
	}

	return 0;
}


//804d7420 - global power on (no reset)
//80479d60 - resets on scene change
static void *frame_ctr = (void*)0x004d7420;
static u32 last_frame = 0;
static u32 crash_ts;
static u8 framemsg[] = "FRAME\x00\x00\x00";
static u8 crashmsg[] = "CRASHED\x00";
int checkCrash(void)
{
	u32 current_frame;
	if (TimerDiffSeconds(crash_ts) > 20) {
		current_frame = read32(frame_ctr);

		if (current_frame != last_frame) {
			last_frame = current_frame;
			crash_ts = read32(HW_TIMER);
			return 0;
		}
		else if (current_frame == last_frame) {
			sendto(top_fd, client_sock, crashmsg, sizeof(crashmsg), 0);
			return 1;
		}
	}
	else {
		return 0;
	}
}

// 804dec00 is the base of 1.02's stack region 
static void *dump_cur = (void*)0x004dec00;
static u32 dump_off = 0;
static int crash_sock __attribute__((aligned(32)));
void handleCrash(void)
{
	if (dump_off <= 0x10000) {
		sendto(top_fd, client_sock, (dump_cur + dump_off), 2048, 0);
		dump_off += 2048;
	}
}


/* @UnclePunch_
 * 1. Read the byte at 80479d30 (current major scene ID)
 * 2. Read the byte at 80479d33 (current minor scene ID)
 * 3. Iterate through the table at 803daca4 (0x14-byte members). 
 *    Find the member where (byte at offset 0x1 == current major scene ID)
 *    and save the minor scene pointer at offset 0x10.
 * 4. if ((minor scene pointer + (current minor scene ID * 0x18) + 0xC) == 0x8),
 *    then we're currently on some CSS.
 */
void *majorSceneTable = (void*)0x003daca4;
int checkCSS(void)
{
	int e;

	sync_before_read((void*)0x00479d20, 0x20);
	u8 currentMajorScene = *(u8*)0x00479d30;
	u8 currentMinorScene = *(u8*)0x00479d33;

	void *minorSceneTable = NULL;
	void *entry = majorSceneTable;
	for (e = 0; e < 46; entry += 0x14) {
		if (*(u8*)(entry + 0xc) == currentMajorScene) {
			minorSceneTable = (void*)(*(u32*)(entry + 0x10));
			break;
		}
		e++;
	}
	if (minorSceneTable == NULL)
		return 0;
	if (*(u8*)(minorSceneTable + (currentMinorScene * 0x18) + 0xc) == 0x8)
		return 1;
	else
		return 0;
}

int vsCheckCSS(void)
{
	u32 min_scene;
	sync_before_read((void*)0x00479d20, 0x20);
	min_scene = read32(0x00479d30) & 0x000000ff;
	if (min_scene == 0x00)
		return 1;
	else 
		return 0;
}

/* This is the main loop for the server.
 *   - Only transmit when there's some data left in SlipMem
 *   - When there's no valid data, periodically send some keep-alive
 *     messages to the client so we can determine if they've hung up
 */
static u32 SlippiNetworkHandlerThread(void *arg)
{
	int status;
	int crashed = 0;
	crash_ts = read32(HW_TIMER);

	while (1)
	{
		status = getConnectionStatus();
		switch (status)
		{
		case CONN_STATUS_NO_SERVER:
			startServer();
			break;
		case CONN_STATUS_NO_CLIENT:
			listenForClient();
			break;
		case CONN_STATUS_CONNECTED:
			if (crashed == 1) 
			{
				handleCrash();
				break;
			}
			else 
			{
				crashed = checkCrash();
				handleFileTransfer();
				if (vsCheckCSS() == 1) {
					ppc_msg("IN CSS\x00", 7);
					checkAlive();
				}
				break;
			}
		}


		mdelay(THREAD_CYCLE_TIME_MS);
	}

	return 0;
}