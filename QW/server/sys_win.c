/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
#include <windows.h>
#include <conio.h>
#include <direct.h>
#include <limits.h>

#include "qwsvdef.h"
#include "common.h"
#include "console.h"
#include "server.h"
#include "sys.h"

static cvar_t sys_nostdout = { "sys_nostdout", "0" };

static double timer_pfreq;
static int timer_lowshift;
static unsigned int timer_oldtime;
static qboolean timer_fallback;
static DWORD timer_fallback_start;

void MaskExceptions(void);
void Sys_PopFPCW(void);
void Sys_PushFPCW_SetHigh(void);

/*
================
Sys_FileTime
================
*/
int
Sys_FileTime(const char *path)
{
    FILE *f;

    f = fopen(path, "rb");
    if (f) {
	fclose(f);
	return 1;
    }

    return -1;
}

/*
================
Sys_mkdir
================
*/
void
Sys_mkdir(const char *path)
{
    _mkdir(path);
}


static void
Sys_InitTimers(void)
{
    LARGE_INTEGER freq, pcount;
    unsigned int lowpart, highpart;

    MaskExceptions();
    Sys_SetFPCW();

    if (!QueryPerformanceFrequency(&freq)) {
	Con_Printf("WARNING: No hardware timer available, using fallback\n");
	timer_fallback = true;
	timer_fallback_start = timeGetTime();
	return;
    }

    /*
     * get 32 out of the 64 time bits such that we have around
     * 1 microsecond resolution
     */
    lowpart = (unsigned int)freq.LowPart;
    highpart = (unsigned int)freq.HighPart;
    timer_lowshift = 0;

    while (highpart || (lowpart > 2000000.0)) {
	timer_lowshift++;
	lowpart >>= 1;
	lowpart |= (highpart & 1) << 31;
	highpart >>= 1;
    }
    timer_pfreq = 1.0 / (double)lowpart;

    /* Do first time initialisation */
    Sys_PushFPCW_SetHigh();
    QueryPerformanceCounter(&pcount);
    timer_oldtime = (unsigned int)pcount.LowPart >> timer_lowshift;
    timer_oldtime |= (unsigned int)pcount.HighPart << (32 - timer_lowshift);
    Sys_PopFPCW();
}


/*
================
Sys_Error
================
*/
void
Sys_Error(const char *error, ...)
{
    va_list argptr;
    char text[MAX_PRINTMSG];

    va_start(argptr, error);
    vsnprintf(text, sizeof(text), error, argptr);
    va_end(argptr);

//    MessageBox(NULL, text, "Error", 0 /* MB_OK */ );
    printf("ERROR: %s\n", text);

    exit(1);
}


/*
================
Sys_DoubleTime
================
*/
double
Sys_DoubleTime(void)
{
    static double curtime = 0.0;
    static double lastcurtime = 0.0;
    static int sametimecount;

    LARGE_INTEGER pcount;
    unsigned int temp, t2;
    double time;

    if (timer_fallback) {
	DWORD now = timeGetTime();
	if (now < timer_fallback_start)	/* wrapped */
	    return (now + (LONG_MAX - timer_fallback_start)) / 1000.0;
	return (now - timer_fallback_start) / 1000.0;
    }

    Sys_PushFPCW_SetHigh();

    QueryPerformanceCounter(&pcount);

    temp = (unsigned int)pcount.LowPart >> timer_lowshift;
    temp |= (unsigned int)pcount.HighPart << (32 - timer_lowshift);

    /* check for turnover or backward time */
    if ((temp <= timer_oldtime) && ((timer_oldtime - temp) < 0x10000000)) {
	timer_oldtime = temp;	/* so we don't get stuck */
    } else {
	t2 = temp - timer_oldtime;
	time = (double)t2 * timer_pfreq;
	timer_oldtime = temp;
	curtime += time;
	if (curtime == lastcurtime) {
	    sametimecount++;
	    if (sametimecount > 100000) {
		curtime += 1.0;
		sametimecount = 0;
	    }
	} else {
	    sametimecount = 0;
	}
	lastcurtime = curtime;
    }

    Sys_PopFPCW();

    return curtime;
}


/*
================
Sys_ConsoleInput
================
*/
char *
Sys_ConsoleInput(void)
{
    static char text[256];
    static int len;
    int c;

    // read a line out
    while (_kbhit()) {
	c = _getch();
	putch(c);
	if (c == '\r') {
	    text[len] = 0;
	    putch('\n');
	    len = 0;
	    return text;
	}
	if (c == 8) {
	    if (len) {
		putch(' ');
		putch(c);
		len--;
		text[len] = 0;
	    }
	    continue;
	}
	text[len] = c;
	len++;
	if (len == sizeof(text)) {
	    /* buffer is full */
	    len = 0;
	    text[0] = '\0';
	    fprintf (stderr, "\nConsole input too long!\n");
	    return text;
	} else {
	    text[len] = 0;
	}
    }

    return NULL;
}


/*
================
Sys_Printf
================
*/
void
Sys_Printf(const char *fmt, ...)
{
    va_list argptr;

    if (sys_nostdout.value)
	return;

    va_start(argptr, fmt);
    vprintf(fmt, argptr);
    va_end(argptr);
}

/*
================
Sys_Quit
================
*/
void
Sys_Quit(void)
{
    exit(0);
}


/*
=============
Sys_Init

Quake calls this so the system can register variables before host_hunklevel
is marked
=============
*/
void
Sys_Init(void)
{
    Cvar_RegisterVariable(&sys_nostdout);
    Sys_InitTimers();
}

/*
==================
main

==================
*/
int
main(int argc, const char **argv)
{
    quakeparms_t parms;
    double newtime, time, oldtime;

    // static char cwd[1024];
    struct timeval timeout;
    fd_set fdset;
    int t;

    COM_InitArgv(argc, argv);

    parms.argc = com_argc;
    parms.argv = com_argv;

    parms.memsize = 16 * 1024 * 1024;

    if ((t = COM_CheckParm("-heapsize")) != 0 && t + 1 < com_argc)
	parms.memsize = Q_atoi(com_argv[t + 1]) * 1024;

    if ((t = COM_CheckParm("-mem")) != 0 && t + 1 < com_argc)
	parms.memsize = Q_atoi(com_argv[t + 1]) * 1024 * 1024;

    parms.membase = malloc(parms.memsize);

    if (!parms.membase)
	Sys_Error("Insufficient memory.");

    parms.basedir = ".";
    parms.cachedir = NULL;

    SV_Init(&parms);

// run one frame immediately for first heartbeat
    SV_Frame(0.1);

//
// main loop
//
    oldtime = Sys_DoubleTime() - 0.1;
    while (1) {
	// select on the net socket and stdin
	// the only reason we have a timeout at all is so that if the last
	// connected client times out, the message would not otherwise
	// be printed until the next event.
	FD_ZERO(&fdset);
	FD_SET(net_socket, &fdset);
	timeout.tv_sec = 0;
	timeout.tv_usec = 100;
	if (select(net_socket + 1, &fdset, NULL, NULL, &timeout) == -1)
	    continue;

	// find time passed since last cycle
	newtime = Sys_DoubleTime();
	time = newtime - oldtime;
	oldtime = newtime;

	SV_Frame(time);
    }

    return 0;
}

#ifndef USE_X86_ASM
void
Sys_SetFPCW(void)
{
}

void
Sys_PushFPCW_SetHigh(void)
{
}

void
Sys_PopFPCW(void)
{
}

void
MaskExceptions(void)
{
}
#endif
