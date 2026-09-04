/*
crash_posix.c - advanced crashhandler
Copyright (C) 2016 Mittorn

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "common.h"

#if XASH_FREEBSD || XASH_NETBSD || XASH_OPENBSD || XASH_ANDROID || XASH_LINUX || XASH_APPLE
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <stdarg.h>
#if defined( XASH_ANDROID ) || defined( XASH_APPLE )
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif
#if XASH_ANDROID
#include <android/log.h>
#endif
#if XASH_APPLE
#include <dlfcn.h>
#include <execinfo.h>
#endif
#include "library.h"
#include "input.h"
#include "crash.h"
#include "platform/platform.h"

#if XASH_ANDROID
static char crashlog_path[MAX_OSPATH];
static char enginelog_path[MAX_OSPATH];
#endif

static qboolean have_libbacktrace = false;
static char crash_message[16384];

// capturo the developer level (from -dev N) up-front, in a non-signal context,
// so the crash handler can decide how much detail to emit without touching
// the (potentially corrupted) cvar state from inside a signal handler.
static int crash_devlevel = DEV_NONE;

// Append a line to the crash report. Safe for use from a signal handler:
// only write()/minimal libc calls, no allocation, no formatting through the
// potentially-reentrant console. Returns the new message length.
static int Sys_CrashAppend( char *message, int len, size_t max_len, int logfd, const char *fmt, ... )
{
	char line[512];
	va_list va;

	va_start( va, fmt );
	int n = Q_vsnprintf( line, sizeof( line ), fmt, va );
	va_end( va );

	if( n <= 0 )
		return len;

	if( logfd >= 0 )
	{
		ssize_t unused = write( logfd, line, n );
		(void)unused;
	}
	{
		ssize_t unused = write( STDERR_FILENO, line, n );
		(void)unused;
	}

	if( len + n < (int)max_len - 1 )
	{
		memcpy( message + len, line, n );
		len += n;
	}
	return len;
}

#if XASH_ANDROID || XASH_LINUX || XASH_APPLE
// Human-readable description of the most common si_code values.
static const char *Sys_SignalCodeName( int signal, int code )
{
	switch( signal )
	{
	case SIGSEGV:
		switch( code )
		{
		case SEGV_MAPERR: return "SEGV_MAPERR (address not mapped to object)";
		case SEGV_ACCERR: return "SEGV_ACCERR (invalid permissions for mapped object)";
#ifdef SEGV_BNDERR
		case SEGV_BNDERR: return "SEGV_BNDERR (failed address bound check)";
#endif
#ifdef SEGV_PKUERR
		case SEGV_PKUERR: return "SEGV_PKUERR (access was denied by memory protection keys)";
#endif
		default: return "SEGV unknown code";
		}
	case SIGBUS:
		switch( code )
		{
		case BUS_ADRALN: return "BUS_ADRALN (invalid address alignment)";
		case BUS_ADRERR: return "BUS_ADRERR (nonexistent physical address)";
		case BUS_OBJERR: return "BUS_OBJERR (object-specific hardware error)";
		default: return "SIGBUS unknown code";
		}
	case SIGILL:
		switch( code )
		{
		case ILL_ILLOPC: return "ILL_ILLOPC (illegal opcode)";
		case ILL_ILLOPN: return "ILL_ILLOPN (illegal operand)";
		case ILL_ILLADR: return "ILL_ILLADR (illegal addressing mode)";
		case ILL_PRVOPC: return "ILL_PRVOPC (privileged opcode)";
		case ILL_PRVREG: return "ILL_PRVREG (privileged register)";
		default: return "SIGILL unknown code";
		}
	case SIGABRT: return "SIGABRT (abort)";
	default: return NULL;
	}
}

// Dump the CPU register state from the ucontext, only when -dev >= 2.
// OS-provided ucontext layouts differ per architecture; guard each one.
static void Sys_DumpRegisters( void *context, char *message, int *lenp, size_t max_len, int logfd )
{
	ucontext_t *uc = (ucontext_t *)context;
	int len = *lenp;

	if( !uc )
		return;

	len = Sys_CrashAppend( message, len, max_len, logfd, "Registers:\n" );

#if defined( __aarch64__ )
	{
		const char *regnames[31] = {
			"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9",
			"x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18", "x19",
			"x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28", "x29" };

		for( int i = 0; i < 30; i += 4 )
		{
			len = Sys_CrashAppend( message, len, max_len, logfd, "  %-3s=%016llx  %-3s=%016llx  %-3s=%016llx  %-3s=%016llx\n",
				regnames[i], (unsigned long long)uc->uc_mcontext.regs[i],
				regnames[i+1], (unsigned long long)uc->uc_mcontext.regs[i+1],
				regnames[i+2], (unsigned long long)uc->uc_mcontext.regs[i+2],
				regnames[i+3], (unsigned long long)uc->uc_mcontext.regs[i+3] );
		}
		len = Sys_CrashAppend( message, len, max_len, logfd, "  x30 (lr)=%016llx\n",
			(unsigned long long)uc->uc_mcontext.regs[30] );
		len = Sys_CrashAppend( message, len, max_len, logfd, "  sp   =%016llx  pc    =%016llx  pstate=%016llx\n",
			(unsigned long long)uc->uc_mcontext.sp,
			(unsigned long long)uc->uc_mcontext.pc,
			(unsigned long long)uc->uc_mcontext.pstate );
	}
#elif defined( __arm__ )
	{
		len = Sys_CrashAppend( message, len, max_len, logfd, "  r0=%08x r1=%08x r2=%08x r3=%08x\n",
			uc->uc_mcontext.arm_r0, uc->uc_mcontext.arm_r1,
			uc->uc_mcontext.arm_r2, uc->uc_mcontext.arm_r3 );
		len = Sys_CrashAppend( message, len, max_len, logfd, "  r4=%08x r5=%08x r6=%08x r7=%08x\n",
			uc->uc_mcontext.arm_r4, uc->uc_mcontext.arm_r5,
			uc->uc_mcontext.arm_r6, uc->uc_mcontext.arm_r7 );
		len = Sys_CrashAppend( message, len, max_len, logfd, "  r8=%08x r9=%08x r10=%08x fp=%08x\n",
			uc->uc_mcontext.arm_r8, uc->uc_mcontext.arm_r9,
			uc->uc_mcontext.arm_r10, uc->uc_mcontext.arm_fp );
		len = Sys_CrashAppend( message, len, max_len, logfd, "  ip=%08x sp=%08x lr=%08x pc=%08x cpsr=%08x\n",
			uc->uc_mcontext.arm_ip, uc->uc_mcontext.arm_sp,
			uc->uc_mcontext.arm_lr, uc->uc_mcontext.arm_pc,
			uc->uc_mcontext.arm_cpsr );
	}
#elif defined( __x86_64__ )
	{
		len = Sys_CrashAppend( message, len, max_len, logfd, "  rip=%016llx rsp=%016llx rbp=%016llx\n",
			(unsigned long long)uc->uc_mcontext.gregs[REG_RIP],
			(unsigned long long)uc->uc_mcontext.gregs[REG_RSP],
			(unsigned long long)uc->uc_mcontext.gregs[REG_RBP] );
		len = Sys_CrashAppend( message, len, max_len, logfd, "  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n",
			(unsigned long long)uc->uc_mcontext.gregs[REG_RAX],
			(unsigned long long)uc->uc_mcontext.gregs[REG_RBX],
			(unsigned long long)uc->uc_mcontext.gregs[REG_RCX],
			(unsigned long long)uc->uc_mcontext.gregs[REG_RDX] );
		len = Sys_CrashAppend( message, len, max_len, logfd, "  rsi=%016llx rdi=%016llx r8=%016llx r9=%016llx\n",
			(unsigned long long)uc->uc_mcontext.gregs[REG_RSI],
			(unsigned long long)uc->uc_mcontext.gregs[REG_RDI],
			(unsigned long long)uc->uc_mcontext.gregs[REG_R8],
			(unsigned long long)uc->uc_mcontext.gregs[REG_R9] );
		len = Sys_CrashAppend( message, len, max_len, logfd, "  r10=%016llx r11=%016llx r12=%016llx r13=%016llx r14=%016llx r15=%016llx\n",
			(unsigned long long)uc->uc_mcontext.gregs[REG_R10],
			(unsigned long long)uc->uc_mcontext.gregs[REG_R11],
			(unsigned long long)uc->uc_mcontext.gregs[REG_R12],
			(unsigned long long)uc->uc_mcontext.gregs[REG_R13],
			(unsigned long long)uc->uc_mcontext.gregs[REG_R14],
			(unsigned long long)uc->uc_mcontext.gregs[REG_R15] );
	}
#endif

	*lenp = len;
}
#endif // XASH_ANDROID || XASH_LINUX || XASH_APPLE

static void Sys_Crash( int signal, siginfo_t *si, void *context ){
	// safe actions first, stack and memory may be corrupted
	int len = Q_snprintf( crash_message, sizeof( crash_message ), "Ver: " XASH_ENGINE_NAME " " XASH_VERSION " (build %i-%s-%s, %s-%s)\n",
		Q_buildnum(), g_buildcommit, g_buildbranch, Q_buildos(), Q_buildarch() );

#if !XASH_FREEBSD && !XASH_NETBSD && !XASH_OPENBSD && !XASH_APPLE
	len += Q_snprintf( crash_message + len, sizeof( crash_message ) - len, "Crash: signal %d errno %d with code %d at %p %p\n", signal, si->si_errno, si->si_code, si->si_addr, si->si_ptr );
#else
	len += Q_snprintf( crash_message + len, sizeof( crash_message ) - len, "Crash: signal %d errno %d with code %d at %p\n", signal, si->si_errno, si->si_code, si->si_addr );
#endif

	ssize_t unused = write( STDERR_FILENO, crash_message, len );

#if XASH_ANDROID
	__android_log_write( ANDROID_LOG_FATAL, "Xash", crash_message );
#endif

	// now get log fd and write trace directly to log
	int logfd = Sys_LogFileNo();
	if( logfd >= 0 )
		unused = write( logfd, crash_message, len );
	(void)unused;

#if XASH_ANDROID || XASH_LINUX || XASH_APPLE
	// Extended (-dev 2 / DEV_EXTENDED) diagnostics: signal code explanation
	// and CPU register dump. Keep them on the same crash_message buffer so the
	// libbacktrace pass below continues appending after them.
	if( crash_devlevel >= DEV_EXTENDED )
	{
		const char *codename = Sys_SignalCodeName( signal, si->si_code );
		if( codename )
			len = Sys_CrashAppend( crash_message, len, sizeof( crash_message ), logfd, "  code: %s\n", codename );

		Sys_DumpRegisters( context, crash_message, &len, sizeof( crash_message ), logfd );

#if XASH_ANDROID
		__android_log_write( ANDROID_LOG_FATAL, "Xash", crash_message );
#endif
	}
#endif // XASH_ANDROID || XASH_LINUX || XASH_APPLE

#if HAVE_LIBBACKTRACE
#if XASH_APPLE
	// libbacktrace depends on libunwind (arm64e on iOS 17+), which uses
	// pointer authentication (PAC). When called from an arm64 signal handler,
	// unwinding through the stack triggers recursive PAC IB traps.
	// Use Apple's frame-pointer backtrace() fallback instead (below).
	(void)have_libbacktrace;
	(void)logfd;
#else
	if( have_libbacktrace )
	{
		len = Sys_CrashDetailsLibbacktrace( logfd, crash_message, len, sizeof( crash_message ));
	}
#endif
#endif // HAVE_LIBBACKTRACE

#if XASH_APPLE
	// libbacktrace Mach-O backend requires DWARF debug info which is stripped
	// from iOS release builds. Fallback to Apple's frame-pointer-based backtrace.
	{
		void *bt_buffer[128];
		int bt_size = backtrace( bt_buffer, 128 );
		if( bt_size > 0 )
		{
			int start = 0;
			// Skip crash handler frames
			for( int i = 0; i < bt_size && i < 4; i++ )
			{
				Dl_info info;
				if( dladdr( bt_buffer[i], &info ) && info.dli_sname )
				{
					if( !Q_strcmp( info.dli_sname, "Sys_Crash" ) ||
						!Q_strcmp( info.dli_sname, "Sys_CrashDetailsLibbacktrace" ) ||
						!Q_strcmp( info.dli_sname, "_sigtramp" ))
						start = i + 1;
				}
			}
			ssize_t unused;
			unused = write( STDERR_FILENO, "\nBacktrace:\n", 11 );
			if( logfd >= 0 )
				unused = write( logfd, "\nBacktrace:\n", 11 );
			len += Q_snprintf( crash_message + len, sizeof( crash_message ) - len, "\n" );

			for( int i = start; i < bt_size; i++ )
			{
				char line[256];
				int n;
				Dl_info info;
				if( dladdr( bt_buffer[i], &info ) && info.dli_sname )
				{
					n = Q_snprintf( line, sizeof( line ), "  %2d: %s + %#zx (%s)\n",
						i - start, info.dli_sname,
						(uintptr_t)bt_buffer[i] - (uintptr_t)info.dli_saddr,
						info.dli_fname ? COM_FileWithoutPath( info.dli_fname ) : "???" );
				}
				else
				{
					n = Q_snprintf( line, sizeof( line ), "  %2d: %p\n", i - start, bt_buffer[i] );
				}
				unused = write( STDERR_FILENO, line, n );
				if( logfd >= 0 )
					unused = write( logfd, line, n );
				if( len + n < (int)sizeof( crash_message ) - 1 )
				{
					memcpy( crash_message + len, line, n );
					len += n;
				}
			}
			(void)unused;
		}
	}
#endif // XASH_APPLE

#if XASH_IOS
	// Write crash details to xash_ios.log in Documents directory
	{
		const char *docs = IOS_GetDocsDir();
		if( docs )
		{
			char path[1024];
			Q_snprintf( path, sizeof( path ), "%s/xash_ios.log", docs );
			int fd = open( path, O_WRONLY | O_CREAT | O_APPEND, 0644 );
			if( fd >= 0 )
			{
				ssize_t unused = write( fd, crash_message, len );
				close( fd );
				(void)unused;
			}
		}
	}
#endif // XASH_IOS

#if XASH_ANDROID
	// also write to a dedicated crash report file the Java side picks up on next launch
	if( crashlog_path[0] )
	{
		int crashfd = open( crashlog_path, O_WRONLY|O_CREAT|O_TRUNC, 0644 );
		if( crashfd >= 0 )
		{
			write( crashfd, crash_message, len );
			close( crashfd );
		}
	}

	// make a copy of engine.log in staging directory
	// TODO: dump log from console buffers, if -log not enabled
	if( logfd >= 0 && enginelog_path[0] && lseek( logfd, 0, SEEK_SET ) == 0 )
	{
		int outfd = open( enginelog_path, O_WRONLY|O_CREAT|O_TRUNC, 0644 );
		if( outfd >= 0 )
		{
			static char buf[8192];
			while( 1 )
			{
				ssize_t n = read( logfd, buf, sizeof( buf ));
				if( n <= 0 )
					break;
				if( write( outfd, buf, (size_t)n ) != n )
					break;
			}
			close( outfd );
		}
	}

	// JNI/SDL calls aren't safe from a signal handler on Android
	_exit( 128 + signal );
#else
#if !XASH_DEDICATED
	IN_SetMouseGrab( false );
#endif
	host.status = HOST_CRASHED;

	// put MessageBox as Sys_Error
	Platform_MessageBox( "Xash Error", crash_message, false );

	// log saved, now we can try to save configs and close log correctly, it may crash
	if( host.type == HOST_NORMAL )
		CL_Crashed();

	Sys_Quit( "crashed" );
#endif // XASH_ANDROID
}

static struct sigaction old_segv_act;
static struct sigaction old_abrt_act;
static struct sigaction old_bus_act;
static struct sigaction old_ill_act;

void Sys_SetupCrashHandler( const char *argv0 )
{
	struct sigaction act =
	{
		.sa_sigaction = Sys_Crash,
		.sa_flags = SA_SIGINFO | SA_ONSTACK,
	};

#if XASH_ANDROID
	const char *crashdir = getenv( "XASH3D_CRASH_DIR" );

	if( !COM_StringEmptyOrNULL( crashdir ))
	{
		Q_snprintf( crashlog_path, sizeof( crashlog_path ), "%s/crash.log", crashdir );
		Q_snprintf( enginelog_path, sizeof( enginelog_path ), "%s/engine.log", crashdir );
	}

	// unblock the engine/SDL_main thread just in case
	sigset_t set;
	sigemptyset( &set );
	sigaddset( &set, SIGSEGV );
	sigaddset( &set, SIGABRT );
	sigaddset( &set, SIGBUS );
	sigaddset( &set, SIGILL );
	pthread_sigmask( SIG_UNBLOCK, &set, NULL );
#endif

#if XASH_ANDROID || XASH_LINUX || XASH_APPLE
	// remember -dev level for the crash handler (signal-safe copy)
	{
		int dev = ( int )host_developer.value;
		if( dev < DEV_NONE ) dev = DEV_NONE;
		if( dev > 9 ) dev = 9;
		crash_devlevel = dev;
	}
#endif

#if HAVE_LIBBACKTRACE
	have_libbacktrace = Sys_SetupLibbacktrace( argv0 );
#endif // HAVE_LIBBACKTRACE

	sigaction( SIGSEGV, &act, &old_segv_act );
	sigaction( SIGABRT, &act, &old_abrt_act );
	sigaction( SIGBUS,  &act, &old_bus_act );
	sigaction( SIGILL,  &act, &old_ill_act );
}

void Sys_RestoreCrashHandler( void )
{
	sigaction( SIGSEGV, &old_segv_act, NULL );
	sigaction( SIGABRT, &old_abrt_act, NULL );
	sigaction( SIGBUS,  &old_bus_act, NULL );
	sigaction( SIGILL,  &old_ill_act, NULL );
}

#endif // XASH_FREEBSD || XASH_NETBSD || XASH_OPENBSD || XASH_ANDROID || XASH_LINUX
