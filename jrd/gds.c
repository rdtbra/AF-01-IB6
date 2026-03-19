/*
 *	PROGRAM:	JRD Access Method
 *	MODULE:		gds.c
 *	DESCRIPTION:	User callable routines
 *
 * The contents of this file are subject to the Interbase Public
 * License Version 1.0 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy
 * of the License at http://www.Inprise.com/IPL.html
 *
 * Software distributed under the License is distributed on an
 * "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, either express
 * or implied. See the License for the specific language governing
 * rights and limitations under the License.
 *
 * The Original Code was created by Inprise Corporation
 * and its predecessors. Portions created by Inprise Corporation are
 * Copyright (C) Inprise Corporation.
 *
 * All Rights Reserved.
 * Contributor(s): ______________________________________.
 */

#define IO_RETRY	20

#ifdef SHLIB_DEFS
#define LOCAL_SHLIB_DEFS
#endif

#define ISC_TIME_SECONDS_PRECISION		10000L
#define ISC_TIME_SECONDS_PRECISION_SCALE	-4

#include "../jrd/ib_stdio.h"
#include <stdlib.h>
#include <string.h>
#include "../jrd/ibsetjmp.h"
#include "../jrd/common.h"
#include "../jrd/gdsassert.h"
#include "../jrd/file_params.h"
#include "../jrd/msg_encode.h"
#include "../jrd/iberr.h"

#ifndef WIN_NT
#include <sys/param.h>
#endif

#include <stdarg.h>
#include "../jrd/time.h"
#include "../jrd/misc.h"

#if (defined PC_PLATFORM && !defined NETWARE_386)
#include <io.h>
#endif

#if (defined DOS_ONLY && !defined WINDOWS_ONLY)
#include <dos.h>
#define InProtectMode()	_AX = 0x1686, geninterrupt(0x2f), _AX = !_AX 
#endif

#ifdef  DOS_ONLY
#include "../jrd/doserr.h"   /* include before remote.h if included */
#endif

#ifdef WINDOWS_ONLY
#include <windows.h>
#include <malloc.h>
#endif

#ifdef OS2_ONLY
#define INCL_DOSMEMMGR
#define INCL_DOSMISC
#define INCL_DOSPROCESS
#include <os2.h>
#include <fcntl.h>
#endif

#if (defined WIN_NT || defined OS2_ONLY)
#include <io.h>
#endif

#ifdef VMS
#include <file.h>
#include <ib_perror.h>
#include <descrip.h>
#include <types.h>
#include <stat.h>
#include <rmsdef.h>

#else  /* !VMS */

#include <errno.h>

#ifdef NeXT
#include <mach/mach.h>
#endif

#ifdef mpexl
#include <sys/types.h>
#include <mpe.h>
#include <errno.h>
#include "../jrd/mpexl.h"

#else  /* !mpexl */

#include <sys/types.h>
#include <sys/stat.h>
#ifndef PC_PLATFORM
#ifndef OS2_ONLY
#ifndef WIN_NT
#include <sys/file.h>
#endif
#endif
#endif

#endif  /* mpexl */

#ifndef O_RDWR
#include <fcntl.h>
#endif

#ifdef UNIX
#ifndef NeXT
#include <unistd.h>
#endif
#ifdef SUPERSERVER
#include <sys/mman.h>
#include <sys/resource.h>
#endif
#endif

#endif  /* VMS */

/* Turn on V4 mutex protection for gds__alloc/free */
   
#ifdef WIN_NT
#define V4_THREADING
#endif

#ifdef SOLARIS_MT
#ifndef PIPE_LIBRARY
#define V4_THREADING
#endif
#endif

#if (defined(HP10) && defined(SUPERSERVER))
#define V4_THREADING
#endif

#ifdef SUPERSERVER
static CONST TEXT	gdslogid[] = " (Server)";
#else
#ifdef SUPERCLIENT
static CONST TEXT	gdslogid[] = " (Client)";
#else
static CONST TEXT	gdslogid[] = "";
#endif
#endif

#include "../jrd/sql_code.h"
#include "../jrd/thd.h"
#include "../jrd/codes.h"
#include "../jrd/blr.h"
#include "../jrd/msg.h"
#include "../jrd/fil.h"
#include "../jrd/gds_proto.h"
#include "../jrd/isc_proto.h"
#ifndef REQUESTER
#include "../jrd/isc_i_proto.h"
#endif

#ifdef	WINDOWS_ONLY
#include "../jrd/seg_proto.h"
#endif

#ifdef WIN_NT
#define _WINSOCKAPI_
#include <windows.h>
#undef leave
#define TEXT		SCHAR
#define MAXPATHLEN	MAX_PATH
#endif

#ifdef sparc
#ifndef SOLARIS
extern int	ib_printf();
#endif
#endif

#ifndef VMS
#ifndef PC_PLATFORM
#ifndef OS2_ONLY
#ifndef WIN_NT
#ifndef linux
extern int	errno;
extern SCHAR	*sys_errlist[];
extern int	sys_nerr;
#endif
#endif
#endif
#endif
#endif

#ifdef NETWARE_386
#include <conio.h>
#define	PRINTF			ConsolePrintf
#endif

#ifndef PRINTF
#define PRINTF 			ib_printf
#endif

static char *ib_prefix = NULL_PTR;
static char *ib_prefix_lock = NULL_PTR;
static char *ib_prefix_msg = NULL_PTR;



static CONST SCHAR * FAR_VARIABLE CONST messages [] = {
#include "../jrd/msgs.h"
	0			/* Null entry to terminate list */
};

#ifdef APOLLO
std_$call	pfm_$static_cleanup();
#include <apollo/base.h>
#include <apollo/error.h>
#include <apollo/pad.h>
#include "/sys/ins/streams.ins.c"
#include "/sys/ins/type_uids.ins.c"
#include "/sys/ins/ios.ins.c"
#include "../jrd/termtype.h"
#endif

#ifdef IMP
typedef unsigned int    mode_t;
typedef int    		pid_t;
#endif

#ifdef M88K
#define GETTIMEOFDAY(time,tz)	gettimeofday (time)
#endif

#ifndef GETTIMEOFDAY
#define GETTIMEOFDAY(time,tz)	gettimeofday (time, tz)
#define TIMEOFDAY_TZ
#endif

#ifdef NETWARE_386
extern int	regular_malloc;
struct timezon {
    int		tz_minuteswest;
    int		tz_dsttime;
};
#endif

#if (ALIGNMENT == 8)
#define GDS_ALLOC_ALIGNMENT	8
#else
#define GDS_ALLOC_ALIGNMENT	4
#endif
#define ALLOC_ROUNDUP(n)		ROUNDUP ((n),GDS_ALLOC_ALIGNMENT)

typedef struct free {
    struct free	*free_next;
    SLONG	free_length;
#ifdef DEBUG_GDS_ALLOC
    SLONG	free_count;
#endif
} *FREE;


#ifdef DEBUG_GDS_ALLOC

#define DBG_MEM_FILE	"memory.dbg"

#ifdef NETWARE_386
#define	NEWLINE	"\r\n"
#endif

#ifndef NEWLINE
#define NEWLINE "\n"
#endif

typedef struct alloc {
    union {
	ULONG		alloc_length;	/* aka block[0] */
	struct free	alloc_freed;
	}	alloc_status;
    TEXT	alloc_magic1 [6];	/* set to ALLOC_HEADER1_MAGIC */
    TEXT	alloc_filename [30];	/* filename of caller */
    ULONG	alloc_lineno;		/* lineno   of caller */
    ULONG	alloc_requested_length;	/* requested len from caller */
    ULONG	alloc_flags;		/* Internal flags */
    ULONG	alloc_callno;		/* Which call to gds__alloc() */
    struct alloc *alloc_next;		/* Next buffer in the chain */
    TEXT	alloc_magic2 [8];	/* set to ALLOC_HEADER2_MAGIC */
} *ALLOC;
#define ALLOC_HEADER_SIZE 	ALLOC_ROUNDUP(sizeof (struct alloc))
#define ALLOC_TAILER_SIZE	6

#define ALLOC_HEADER1_MAGIC_ALLOCATED	"XYZZY"
#define ALLOC_HEADER1_MAGIC_FREED	"FREED"
#define ALLOC_HEADER2_MAGIC		"@PLOVER#"
#define ALLOC_TAILER_MAGIC		"PLUGH"

#define ALLOC_FREED_PATTERN		0xCB
#define ALLOC_ALLOC_PATTERN		0xFD

#define ALLOC_FREQUENCY			99	/* how often to validate memory */


static ALLOC	gds_alloc_chain = NULL;
static ULONG	gds_alloc_call_count = 0;
static ULONG	gds_alloc_state = ~0;
static ULONG	gds_alloc_nomem = ~0;
static ULONG	gds_free_call_count = 0;
static ULONG	gds_bug_alloc_count = 0;
static ULONG	gds_bug_free_count = 0;
static struct alloc	
		*gds_alloc_watchpoint = NULL;	/* For SUN debugging */
static ULONG	gds_alloc_watch_call_count = 0;

#define SAVE_DEBUG_INFO(b,f,l) {\
	strncpy (((ALLOC) (b))->alloc_magic1, ALLOC_HEADER1_MAGIC_ALLOCATED, sizeof (((ALLOC) (b))->alloc_magic1)); \
	strncpy (((ALLOC) (b))->alloc_magic2, ALLOC_HEADER2_MAGIC, sizeof (((ALLOC) (b))->alloc_magic2)); \
	strncpy (((ALLOC) (b))->alloc_filename, f, sizeof (((ALLOC) (b))->alloc_filename)); \
	((ALLOC) (b))->alloc_requested_length = size_request; \
	((ALLOC) (b))->alloc_flags = 0; \
	((ALLOC) (b))->alloc_lineno = l; \
	((ALLOC) (b))->alloc_callno = gds_alloc_call_count; \
	\
	/* Link this block into the list of blocks */ \
	((ALLOC) (b))->alloc_next = gds_alloc_chain; \
	gds_alloc_chain = ((ALLOC) (b)); \
	\
	/* Set the "tailer" at the end of the allocated memory */ \
	memcpy ( ((BLOB_PTR *) (b)) + ALLOC_HEADER_SIZE + size_request, \
		ALLOC_TAILER_MAGIC, ALLOC_TAILER_SIZE); \
	\
	/* Initialize the allocated memory */ \
	memset (((UCHAR *) (b)) + ALLOC_HEADER_SIZE, ALLOC_ALLOC_PATTERN, size_request); \
	}

static void	gds_alloc_validate (ALLOC);
static BOOLEAN	gds_alloc_validate_free_pattern (UCHAR *, ULONG);
static void	gds_alloc_validate_freed (ALLOC);
static void	validate_memory (void);

#endif

#ifndef SAVE_DEBUG_INFO
#define SAVE_DEBUG_INFO(b,f,l)
#endif

#ifndef ALLOC_HEADER_SIZE
#define ALLOC_HEADER_SIZE 	ALLOC_ROUNDUP(sizeof (ULONG))
#endif

#ifndef ALLOC_TAILER_SIZE
#define ALLOC_TAILER_SIZE	0
#endif

#define ALLOC_OVERHEAD		(ALLOC_HEADER_SIZE + ALLOC_TAILER_SIZE)

#ifdef PC_PLATFORM
#ifdef NETWARE_386
#define GDS_ALLOC_EXTEND_SIZE	102400L
#else
#define GDS_ALLOC_EXTEND_SIZE	2048L
#endif
#endif

#ifndef GDS_ALLOC_EXTEND_SIZE
#ifdef SUPERCLIENT
#define GDS_ALLOC_EXTEND_SIZE	8192
#else
#define GDS_ALLOC_EXTEND_SIZE	102400
#endif
#endif

#ifdef mpexl
#define FOPEN_APPEND_TYPE	"a Ds2 V E32 S256000 L M2 X3"
#endif

#ifndef FOPEN_APPEND_TYPE
#define FOPEN_APPEND_TYPE	"a"
#endif

#ifndef O_BINARY
#define O_BINARY		0
#endif

#ifndef MAXPATHLEN
#define MAXPATHLEN		1024
#endif

#ifndef GENERIC_SQLCODE 	
#define GENERIC_SQLCODE		-999
#endif

static char ib_prefix_val[MAXPATHLEN];
static char ib_prefix_lock_val[MAXPATHLEN];
static char ib_prefix_msg_val[MAXPATHLEN];


#define DEFINED_GDS_QUAD
#ifndef DEFINED_GDS_QUAD
typedef struct {
    SLONG	gds_quad_high;
    ULONG	gds_quad_low;
} GDS__QUAD;
#define DEFINED_GDS_QUAD
#endif


typedef struct msg {
    ULONG	msg_top_tree;
    int		msg_file;
    USHORT	msg_bucket_size;
    USHORT	msg_levels;
    SCHAR	msg_bucket [1];
} *MSG;

typedef struct ctl {
    UCHAR	*ctl_blr;		/* Running blr string */
    UCHAR	*ctl_blr_start;		/* Original start of blr string */
    void	(*ctl_routine)();	/* Call back */
    SCHAR	*ctl_user_arg;		/* User argument */
    TEXT	*ctl_ptr;
    SSHORT	ctl_language;
    TEXT	ctl_buffer [1024];
} *CTL;

#ifdef DEV_BUILD
void	GDS_breakpoint (int);
#endif


static void	blr_error (CTL, TEXT *, TEXT *);
static void	blr_format (CTL, TEXT *, ...);
static void	blr_indent (CTL, SSHORT);
static void	blr_print_blr (CTL, UCHAR);
static SCHAR	blr_print_byte (CTL);
static int	blr_print_char (CTL);
static void	blr_print_cond (CTL);
static int	blr_print_dtype (CTL);
static void	blr_print_join (CTL);
static SLONG	blr_print_line (CTL, SSHORT);
static void	blr_print_verb (CTL, SSHORT);
static int	blr_print_word (CTL);
#ifdef OS2_ONLY
static void APIENTRY gds__cleanup (ULONG);
#endif

static void	cleanup_malloced_memory (void *);
static ULONG	free_memory (void*);
static void	init (void);
static int	yday (struct tm *);
#if (defined SOLARIS && defined SUPERSERVER)
static UCHAR	*mmap_anon (SLONG);
#endif
static void	ndate (SLONG, struct tm *);
static GDS_DATE	nday (struct tm *);
static void	sanitize (TEXT *);

/* Generic cleanup handlers */

typedef struct clean {
    struct clean *clean_next;
    void	(*clean_routine)();
    void	*clean_arg;
} *CLEAN;

static CLEAN	cleanup_handlers = NULL;
static FREE	pool = NULL;
static void	**malloced_memory = NULL;
static MSG	default_msg = NULL;
static SLONG	initialized = FALSE;
static MUTX_T	alloc_mutex;
#ifdef DEV_BUILD
SLONG		gds_delta_alloc = 0, gds_max_alloc = 0; /* Used in DBG_memory */
#endif
#ifdef UNIX
static SLONG    gds_pid = 0;
#endif
#if (defined SOLARIS && defined SUPERSERVER)
static int	anon_fd = -1;
static int	page_size = -1;
#endif

/* VMS structure to declare exit handler */

#ifdef VMS
static SLONG	exit_status = 0;
static struct {
    SLONG	link;
    int		(*exit_handler)();
    SLONG	args;
    SLONG	arg [1];
    }		exit_description;
#endif


#ifdef SHLIB_DEFS
#define SETJMP		(*_libgds_setjmp)
#define sprintf		(*_libgds_sprintf)
#define vsprintf	(*_libgds_vsprintf)
#define strlen		(*_libgds_strlen)
#define strcpy		(*_libgds_strcpy)
#define LONGJMP		(*_libgds_longjmp)
#define _iob		(*_libgds__iob)
#define getpid          (*_libgds_getpid)
#define ib_fprintf		(*_libgds_fprintf)
#define ib_printf		(*_libgds_printf)
#define ib_fopen		(*_libgds_fopen)
#define ib_fclose		(*_libgds_fclose)
#define sys_nerr	(*_libgds_sys_nerr)
#define sys_errlist	(*_libgds_sys_errlist)
#define malloc		(*_libgds_malloc)
#define gettimeofday	(*_libgds_gettimeofday)
#define ctime		(*_libgds_ctime)
#define getenv		(*_libgds_getenv)
#define lseek		(*_libgds_lseek)
#define read		(*_libgds_read)
#define memcpy		(*_libgds_memcpy)
#define open		(*_libgds_open)
#define strcat		(*_libgds_strcat)
#define ib_fputs		(*_libgds_fputs)
#define ib_fputc		(*_libgds_fputc)
#define mktemp		(*_libgds_mktemp)
#define unlink		(*_libgds_unlink)
#define close		(*_libgds_close)
#define strncmp		(*_libgds_strncmp)
#define time		(*_libgds_time)
#define ib_fflush		(*_libgds_fflush)
#define errno		(*_libgds_errno)
#define umask		(*_libgds_umask)
#define atexit		(*_libgds_atexit)
#define ib_vfprintf	(*_libgds_vfprintf)

extern int		SETJMP();
extern int		sprintf();
extern int		vsprintf();
extern int		strlen();
extern SCHAR		*strcpy();
extern void		LONGJMP();
extern IB_FILE		_iob [];
extern pid_t            getpid();
extern int		ib_fprintf();
extern int		ib_printf();
extern IB_FILE		*ib_fopen();
extern int		ib_fclose();
extern int		sys_nerr;
extern SCHAR		*sys_errlist [];
extern void		*malloc();
extern int		gettimeofday();
extern SCHAR		*ctime();
extern SCHAR		*getenv();
extern off_t		lseek();
extern int		read();
extern void		*memcpy();
extern int		open();
extern SCHAR		*strcat();
extern int		ib_fputs();
extern int		ib_fputc();
extern SCHAR		*mktemp();
extern int		unlink();
extern int		close();
extern int		strncmp();
extern time_t		time();
extern int		ib_fflush();
extern int		errno;
extern mode_t		umask();
extern int		atexit();
extern int		ib_vfprintf();
#endif  /* SHLIB_DEFS */

/* BLR Pretty print stuff */

#ifndef JMP_BUF
error JMP_BUF not defined!
#else	/* JMP_BUF */
static JMP_BUF	env;		/* Error return environment */
#endif	/* JMP_BUF */

#define PRINT_VERB 	blr_print_verb (control, level)
#define PRINT_LINE	blr_print_line (control, (SSHORT) offset)
#define PRINT_BYTE	blr_print_byte (control)
#define PRINT_CHAR	blr_print_char (control)
#define PRINT_WORD	blr_print_word (control)
#define PRINT_COND	blr_print_cond (control)
#define PRINT_DTYPE	blr_print_dtype (control)
#define PRINT_JOIN	blr_print_join (control)
#define BLR_BYTE	*(control->ctl_blr)++
#define PUT_BYTE(byte)	*(control->ctl_ptr)++ = byte

#define op_line		 1
#define op_verb		 2
#define op_byte		 3
#define op_word		 4
#define op_pad		 5
#define op_dtype	 6
#define op_message	 7
#define op_literal	 8
#define op_begin	 9
#define op_map		 10
#define op_args		 11
#define op_union	 12
#define op_indent	 13
#define op_join		 14
#define op_parameters	 15
#define op_error_handler 16
#define op_set_error	 17
#define op_literals	 18
#define op_relation	 20

static CONST UCHAR

    /* generic print formats */

    zero []	= { op_line, 0},
    one []	= { op_line, op_verb, 0},	
    two []	= { op_line, op_verb, op_verb, 0},
    three []	= { op_line, op_verb, op_verb, op_verb, 0},
    field []	= { op_byte, op_byte, op_literal, op_pad, op_line, 0},
    byte [] 	= { op_byte, op_line, 0},
    byte_args [] = { op_byte, op_line, op_args, 0},
    byte_byte [] = { op_byte, op_byte, op_line, 0},
    byte_verb [] = { op_byte, op_line, op_verb, 0},
    byte_verb_verb [] = { op_byte, op_line, op_verb, op_verb, 0},
    byte_literal [] = { op_byte, op_literal, op_line, 0 },
    byte_byte_verb []	= { op_byte, op_byte, op_line, op_verb, 0},
    parm []	= { op_byte, op_word, op_line, 0},	/* also field id */
    parm2 []	= { op_byte, op_word, op_word, op_line, 0},
    parm3 []	= { op_byte, op_word, op_word, op_word, op_line, 0},

    /* formats specific to a verb */

    begin []	= { op_line, op_begin, op_verb, 0},
    literal []	= { op_dtype, op_literal, op_line, 0},
    message []	= { op_byte, op_word, op_line, op_message, 0},
    rse []	= { op_byte, op_line, op_begin, op_verb, 0},
    relation []	= { op_byte, op_literal, op_pad, op_byte, op_line, 0},
    relation2[] = { op_byte, op_literal, op_line, op_indent, op_byte, op_literal, op_pad, op_byte, op_line, 0},
    aggregate[]	= { op_byte, op_line, op_verb, op_verb, op_verb, 0},
    rid []	= { op_word, op_byte, op_line, 0},
    rid2 []	= { op_word, op_byte, op_literal, op_pad, op_byte, op_line, 0},
    union_ops []= { op_byte, op_byte, op_line, op_union, 0},
    map []	= { op_word, op_line, op_map, 0},
    function []	= { op_byte, op_literal, op_byte, op_line, op_args, 0},
    gen_id []	= { op_byte, op_literal, op_line, op_verb, 0},
    declare []	= { op_word, op_dtype, op_line, 0 },
    variable [] = { op_word, op_line, 0 },
    indx []    = { op_line, op_verb, op_indent, op_byte, op_line, op_args, 0},
    find []	= { op_byte, op_verb, op_verb, op_indent, op_byte, op_line, op_args, 0},
    seek []	= { op_line, op_verb, op_verb, 0},
    join []	= { op_join, op_line, 0},
    exec_proc[]	= { op_byte, op_literal, op_line, op_indent, op_word, op_line,
		    op_parameters, op_indent, op_word, op_line, op_parameters, 0},
    procedure[]	= { op_byte, op_literal, op_pad, op_byte, op_line,
		    op_indent, op_word, op_line, op_parameters, 0},
    pid[]	= { op_word, op_pad, op_byte, op_line,
		    op_indent, op_word, op_line, op_parameters, 0},
    error_handler[]= { op_word, op_line, op_error_handler, 0},
    set_error[]	= { op_set_error, op_line, 0},
    cast[]      = { op_dtype, op_line, op_verb, 0},
    indices[]	= { op_byte, op_line, op_literals, 0},
    lock_relation [] = { op_line, op_indent, op_relation, op_line, op_verb, 0},
    range_relation [] = { op_line, op_verb, op_indent, op_relation, op_line, 0},
    extract []	= { op_line, op_byte, op_verb, 0};

static CONST struct {
    CONST SCHAR	*blr_string;
    CONST UCHAR	**blr_operators;
} FAR_VARIABLE blr_table [] = {
#include "../jrd/blp.h"
    0, 0
};

#define ISC_ENV		"INTERBASE"
#define ISC_LOCK_ENV    "INTERBASE_LOCK"
#define ISC_MSG_ENV     "INTERBASE_MSG"

#ifdef WIN_NT
#define EXPAND_PATH(relative, absolute)		_fullpath(absolute, relative, MAXPATHLEN)
#define COMPARE_PATH(a,b)			_stricmp(a,b)
#else
#define EXPAND_PATH(relative, absolute)		realpath(relative, absolute)
#define COMPARE_PATH(a,b)			strcmp(a,b)
#endif


/* This is duplicated from gds.h */

#define blr_gds_code		0
#define blr_sql_code		1
#define blr_exception		2
#define blr_trigger_code	3
#define blr_default_code	4

#define gds__bpb_version1	1
#define gds__bpb_source_type	1
#define gds__bpb_target_type	2
#define gds__bpb_type		3
#define gds__bpb_source_interp  4
#define gds__bpb_target_interp  5

#define gds__bpb_type_segmented	0
#define gds__bpb_type_stream	1

#ifdef SUPERSERVER
extern SLONG    allr_delta_alloc;
extern SLONG    alld_delta_alloc;
extern SLONG    all_delta_alloc;
SLONG		trace_pools=0;
SLONG 		free_map_debug=0;
static void freemap(int cod);
void gds_print_delta_counters(IB_FILE* );
#endif



#ifdef DEBUG_GDS_ALLOC

UCHAR * API_ROUTINE gds__alloc_debug (
    SLONG	size_request,
    TEXT	*filename,
    ULONG	lineno)

#else

UCHAR * API_ROUTINE gds__alloc (
    SLONG	size_request)

#endif
{
/**************************************
 *
 *	g d s _ $ a l l o c
 *
 **************************************
 *
 * Functional description
 *	Allocate a block of memory.
 *
 **************************************/
SLONG	*block = NULL;
SLONG	size;
ULONG	factor;
#ifndef WINDOWS_ONLY
FREE	*next, *best, free_blk;
SLONG	tail, best_tail = 0;
#endif
#ifdef VMS
SLONG	status;
#endif

if (size_request <= 0)
    {
    DEV_REPORT ("gds__alloc: non-positive size allocation request\n"); /* TXNN */
    BREAKPOINT (__LINE__);
    return NULL;
    }

if (!initialized)
    init();


#ifdef DEBUG_GDS_ALLOC
gds_alloc_call_count++;
if (gds_alloc_call_count == gds_bug_alloc_count)
    {
    PRINTF ("gds_alloc() call %ld reached\n", gds_bug_alloc_count);
    BREAKPOINT (__LINE__);
    }

/* If we want to validate all memory on each call, do so now */
if ((gds_alloc_state < gds_alloc_call_count) || 
    (gds_bug_alloc_count && (gds_bug_alloc_count < gds_alloc_call_count)) ||
	((gds_alloc_call_count % ALLOC_FREQUENCY) == 0))
    {
    V4_MUTEX_LOCK (&alloc_mutex);
    gds_alloc_report (ALLOC_silent, __FILE__, __LINE__);
    V4_MUTEX_UNLOCK (&alloc_mutex);
    }

/* Simulate out of memory */
if (gds_alloc_nomem <= gds_alloc_call_count)
    return NULL;
#endif

/* Add the size of malloc overhead to the block, and then
 * round up the block size according to the desired alignment
 * for the platform
 * Make sure that for a multiple of 1K allocate ALLOC_OVERHEAD 
 * times the multiple. This ensures that on freeing and subsequent
 * re-allocating we don't fragment the free memory list
*/
factor = 1;
if (!(size_request & (1024-1)))
    factor = size_request/1024;
size = size_request + ALLOC_OVERHEAD*factor;
size = ROUNDUP (size, GDS_ALLOC_ALIGNMENT);
#ifdef DEV_BUILD
gds_delta_alloc += size;
#endif

#ifdef NETWARE_386
if (regular_malloc)
    {
    block = (SLONG*) malloc (size);
    if (!block)
	{
	ib_perror ("gds__alloc()");
	return NULL;
	}

    block [0] = size;
    SAVE_DEBUG_INFO (block, filename, lineno);
    return (UCHAR*) block + ALLOC_HEADER_SIZE;
    }
#endif
#ifdef WINDOWS_ONLY
{
#ifdef DEBUG_GDS_ALLOC
if (size > 65535)
    {
    UCHAR	buffer [150];
	sprintf (buffer,
        "gds__alloc: more than 64K requested, len=%5ld(%5ld) file=%15.30s line=%4ld%s",
        size_request,
        size,
        filename,
        lineno,
        NEWLINE);
    DEV_REPORT (buffer);
    }
#endif

block = (SLONG *) _fmalloc (size);

if (!block) 
    {
    if (GetFreeSpace(0) < size)
	MessageBox (NULL, "Not enough free space", "gds__alloc () error", MB_ICONEXCLAMATION);
    else
	MessageBox (NULL, "Allocation Failed", "gds__alloc() error", MB_ICONEXCLAMATION);
    ib_perror ("gds__alloc()");
    return NULL;
    }

block [0] = size;
SAVE_DEBUG_INFO (block, filename, lineno);
return (UCHAR*) block + ALLOC_HEADER_SIZE;
}
#else

#ifdef DOS_ONLY
/* If we are in protect mode (Windows or DPMI), bypass
   the normal memory pool management and simply allocate from system */

if (InProtectMode()) 
    {
    block = (SLONG*) malloc (size);
    if (!block) 
	{
	ib_perror ("gds__alloc()");
	return NULL;
	}

    block [0] = size;
    SAVE_DEBUG_INFO (block, filename, lineno);
    return (UCHAR*) block + ALLOC_HEADER_SIZE;
    }
#endif

V4_MUTEX_LOCK (&alloc_mutex);

/* Find the free block with the shortest tail */

best = NULL;

for (next = &pool; (free_blk = *next) != NULL; next = &(*next)->free_next)
    {
    if (free_blk->free_length <= 0 ||
	(free_blk->free_next && (UCHAR HUGE_PTR*) free_blk + free_blk->free_length > (UCHAR HUGE_PTR*) free_blk->free_next))
	{
	DEV_REPORT ("gds__alloc: memory pool corrupted\n");    /* TXNN */
	BREAKPOINT (__LINE__);
	*next = NULL;
	break;
	}
    if ((tail = free_blk->free_length - size) >= 0)
	if (!best || tail < best_tail)
	    {
	    best = next;
	    if (!(best_tail = tail))
		break;
	    }
    }

/* If there was a fit, return the block */

if (best)
    {
    if (best_tail <= sizeof (struct free) + ALLOC_OVERHEAD)
	{
	/* There's only a little left over, give client the whole chunk */
	size = (*best)->free_length;
	block = (SLONG*) *best;
	*best = (*best)->free_next;
#ifdef DEBUG_GDS_ALLOC
	/* We're going to give some memory off the free list - before doing
	 * so, let's make sure it still looks like free memory.
	 */
	gds_alloc_validate_freed ((ALLOC) block);
#endif DEBUG_GDS_ALLOC
	}
    else
	{
	/* Shave off what we need from the end of the free chunk */
	block = (SLONG*) ((UCHAR*) *best + best_tail);
	(*best)->free_length -= size;
#ifdef DEBUG_GDS_ALLOC
	/* We're going to give some memory off the free list - before doing
	 * so, let's make sure it still looks like free memory.
	 */
	(void) gds_alloc_validate_free_pattern ((UCHAR *) block, (ULONG) size);
#endif DEBUG_GDS_ALLOC
	}
    block [0] = size;

    SAVE_DEBUG_INFO (block, filename, lineno);

    V4_MUTEX_UNLOCK (&alloc_mutex);
    return (UCHAR*) block + ALLOC_HEADER_SIZE;
    }

/* We need to extend the address space.  Do so in multiples of the
 * extend quantity, release the extra, and return the block.
 * If we can't get a multiple of the extend size, try again using
 * the minimal amount of memory needed before returning "out of memory".
 * The minimal amount must include the malloc/free overhead amounts.
 */

tail = (((size + sizeof (struct free) + ALLOC_ROUNDUP (sizeof (void *)) + 
		ALLOC_OVERHEAD) / GDS_ALLOC_EXTEND_SIZE) + 1) * GDS_ALLOC_EXTEND_SIZE;

#ifdef APOLLO
rws_$alloc (&tail, &block);
if (!block)
    {
    tail = (size + sizeof (struct free) + 
		ALLOC_ROUNDUP (sizeof (void *)) + ALLOC_OVERHEAD);
    rws_$alloc (&tail, &block);
    if (!block)
	{
	V4_MUTEX_UNLOCK (&alloc_mutex);
	return NULL;
	}
    }
#define MEMORY_ALLOCATED
#endif

#ifdef VMS
status = lib$get_vm (&tail, &block);
if (!(status & 1))
    {
    tail = (size + sizeof (struct free) + 
		ALLOC_ROUNDUP (sizeof (void *)) + ALLOC_OVERHEAD);
    status = lib$get_vm (&tail, &block);
    if (!(status & 1))
	{
	V4_MUTEX_UNLOCK (&alloc_mutex);
	return NULL;
	}
    }
#define MEMORY_ALLOCATED
#endif

#ifdef OS2_ONLY
if (DosAllocMem (&block, tail, PAG_COMMIT | PAG_READ | PAG_WRITE))
    {
    tail = (size + sizeof (struct free) + 
		ALLOC_ROUNDUP (sizeof (void *)) + ALLOC_OVERHEAD);
    if (DosAllocMem (&block, tail, PAG_COMMIT | PAG_READ | PAG_WRITE))
	{
	V4_MUTEX_UNLOCK (&alloc_mutex);
	return NULL;
	}
    }
#define MEMORY_ALLOCATED
#endif

#ifndef MEMORY_ALLOCATED
if ((block = (SLONG*) malloc (tail)) == NULL)
    {
    tail = (size + sizeof (struct free) + 
		ALLOC_ROUNDUP (sizeof (void *)) + ALLOC_OVERHEAD);
    if ((block = (SLONG*) malloc (tail)) == NULL)
	{
	V4_MUTEX_UNLOCK (&alloc_mutex);
	return NULL;
	}
    }
#endif
#undef MEMORY_ALLOCATED

/* Insert the allocated block into a simple linked list */

*((void**) block) = (void*) malloced_memory;
malloced_memory = (void**) block;
block = (SLONG*) ((UCHAR*) block + ALLOC_ROUNDUP (sizeof (void *)));

#ifdef DEV_BUILD
gds_max_alloc += tail;
#endif

tail -= size + ALLOC_ROUNDUP (sizeof (void *));

#ifdef DEV_BUILD
gds_delta_alloc += tail;
#endif

#ifdef DEBUG_GDS_ALLOC
memset ((UCHAR*) block, ALLOC_FREED_PATTERN, tail);
#endif

block [0] = tail;
(void) free_memory ((SLONG*) ((UCHAR*) block + ALLOC_HEADER_SIZE));

#ifdef SUPERSERVER
if (free_map_debug)
     freemap(0);
#endif


block = (SLONG*) ((UCHAR*) block + tail);
block [0] = size;

SAVE_DEBUG_INFO (block, filename, lineno);

V4_MUTEX_UNLOCK (&alloc_mutex);
return (UCHAR*) block + ALLOC_HEADER_SIZE;

#endif /* WINDOWS_ONLY */
}

STATUS API_ROUTINE gds__decode (
    STATUS	code,
    USHORT	*fac,
    USHORT	*class)
{
/**************************************
 *
 *	g d s _ $ d e c o d e
 *
 **************************************
 *
 * Functional description
 *	Translate a status codes from system dependent form to
 *	network form.
 *
 **************************************/

if (!code)
    return SUCCESS;
else if (code & ISC_MASK != ISC_MASK)
    /* not an ISC error message */
    return code; 

*fac = GET_FACILITY(code);
*class = GET_CLASS(code);
return GET_CODE(code);

}

void API_ROUTINE isc_decode_date (
    GDS__QUAD	*date,
    void	*times_arg)
{
/**************************************
 *
 *	i s c _ d e c o d e _ d a t e
 *
 **************************************
 *
 * Functional description
 *	Convert from internal timestamp format to UNIX time structure.
 *
 *	Note: this API is historical - the prefered entrypoint is
 *	isc_decode_timestamp
 *
 **************************************/
isc_decode_timestamp ((GDS_TIMESTAMP *) date, times_arg);
}

void API_ROUTINE isc_decode_sql_date (
    GDS_DATE	*date,
    void	*times_arg)
{
/**************************************
 *
 *	i s c _ d e c o d e _ s q l _ d a t e
 *
 **************************************
 *
 * Functional description
 *	Convert from internal DATE format to UNIX time structure.
 *
 **************************************/
SLONG		minutes;
struct tm	*times;

times = (struct tm*) times_arg;
memset (times, 0, sizeof (*times));

ndate (*date, times);
times->tm_yday = yday (times);
if ((times->tm_wday = (*date + 3) % 7) < 0)
    times->tm_wday += 7;
}

void API_ROUTINE isc_decode_sql_time (
    GDS_TIME	*sql_time,
    void	*times_arg)
{
/**************************************
 *
 *	i s c _ d e c o d e _ s q l _ t i m e
 *
 **************************************
 *
 * Functional description
 *	Convert from internal TIME format to UNIX time structure.
 *
 **************************************/
ULONG		minutes;
struct tm	*times;

times = (struct tm*) times_arg;
memset (times, 0, sizeof (*times));

minutes = *sql_time / (ISC_TIME_SECONDS_PRECISION * 60);
times->tm_hour = minutes / 60;
times->tm_min = minutes % 60;
times->tm_sec = (*sql_time / ISC_TIME_SECONDS_PRECISION) % 60;
}

void API_ROUTINE isc_decode_timestamp (
    GDS_TIMESTAMP	*date,
    void		*times_arg)
{
/**************************************
 *
 *	i s c _ d e c o d e _ t i m e s t a m p
 *
 **************************************
 *
 * Functional description
 *	Convert from internal timestamp format to UNIX time structure.
 *
 *	Note: the date arguement is really an ISC_TIMESTAMP -- however the
 *	definition of ISC_TIMESTAMP is not available from all the source
 *	modules that need to use isc_encode_timestamp
 *
 **************************************/
SLONG		minutes;
struct tm	*times;

times = (struct tm*) times_arg;
memset (times, 0, sizeof (*times));

ndate (date->timestamp_date, times);
times->tm_yday = yday (times);
if ((times->tm_wday = (date->timestamp_date + 3) % 7) < 0)
    times->tm_wday += 7;

minutes = date->timestamp_time / (ISC_TIME_SECONDS_PRECISION * 60);
times->tm_hour = minutes / 60;
times->tm_min = minutes % 60;
times->tm_sec = (date->timestamp_time / ISC_TIME_SECONDS_PRECISION) % 60;
}

STATUS API_ROUTINE gds__encode (
    STATUS	code,
    USHORT	facility)
{
/**************************************
 *
 *	g d s _ $ e n c o d e
 *
 **************************************
 *
 * Functional description
 *	Translate a status codes from network format to system
 *	dependent form.
 *
 **************************************/

if (!code)
    return SUCCESS;

return ENCODE_ISC_MSG(code, facility);
}

void API_ROUTINE isc_encode_date (
    void	*times_arg,
    GDS__QUAD	*date)
{
/**************************************
 *
 *	i s c _ e n c o d e _ d a t e
 *
 **************************************
 *
 * Functional description
 *	Convert from UNIX time structure to internal timestamp format.
 *
 *	Note: This API is historical -- the prefered entrypoint is
 *	isc_encode_timestamp
 *
 **************************************/
isc_encode_timestamp (times_arg, (GDS_TIMESTAMP *) date);
}

void API_ROUTINE isc_encode_sql_date (
    void	*times_arg,
    GDS_DATE	*date)
{
/**************************************
 *
 *	i s c _ e n c o d e _ s q l _ d a t e
 *
 **************************************
 *
 * Functional description
 *	Convert from UNIX time structure to internal date format.
 *
 **************************************/

*date = nday ((struct tm*) times_arg);
}

void API_ROUTINE isc_encode_sql_time (
    void	*times_arg,
    GDS_TIME	*isc_time)
{
/**************************************
 *
 *	i s c _ e n c o d e _ s q l _ t i m e
 *
 **************************************
 *
 * Functional description
 *	Convert from UNIX time structure to internal TIME format.
 *
 **************************************/
struct tm	*times;

times = (struct tm*) times_arg;
*isc_time = ((times->tm_hour * 60 + times->tm_min) * 60 + 
		times->tm_sec) * ISC_TIME_SECONDS_PRECISION;
}

void API_ROUTINE isc_encode_timestamp (
    void		*times_arg,
    GDS_TIMESTAMP	*date)
{
/**************************************
 *
 *	i s c _ e n c o d e _ t i m e s t a m p
 *
 **************************************
 *
 * Functional description
 *	Convert from UNIX time structure to internal timestamp format.
 *
 *	Note: the date arguement is really an ISC_TIMESTAMP -- however the
 *	definition of ISC_TIMESTAMP is not available from all the source
 *	modules that need to use isc_encode_timestamp
 *
 **************************************/
struct tm	*times;

times = (struct tm*) times_arg;

date->timestamp_date = nday (times);
date->timestamp_time = 
	((times->tm_hour * 60 + times->tm_min) * 60 + 
		times->tm_sec) * ISC_TIME_SECONDS_PRECISION;
}

ULONG API_ROUTINE gds__free (
    void	*blk)
{
/**************************************
 *
 *	g d s _ $ f r e e
 *
 **************************************
 *
 * Functional description
 *	Release a block allocated by gds__alloc.  Return number
 *	of bytes released. Called under mutex protection.
 *
 **************************************/
ULONG	released;

V4_MUTEX_LOCK (&alloc_mutex);

#ifdef DEBUG_GDS_ALLOC
gds_free_call_count++;
if (!blk)
    {
    DEV_REPORT ("gds__free: tried to release NULL\n");
    BREAKPOINT (__LINE__);
    }
else
    {
    ALLOC	alloc, *p;
    BOOLEAN	found = FALSE;
    ULONG	save_length;

    alloc = (ALLOC) (((char *) blk) - ALLOC_HEADER_SIZE);

    for (p = &gds_alloc_chain; *p; p = &((*p)->alloc_next))
	if (*p == alloc)
	    {
	    found++;
	    *p = (*p)->alloc_next;
	    break;
	    }
    if (!found)
	{
	DEV_REPORT ("gds__free: Tried to free unallocated block\n");
	BREAKPOINT (__LINE__);
	}
    else
	{
	/* Always validate the buffer we're about to free */
	gds_alloc_validate (alloc);

	/* If we want to validate all memory on each call, do so now */
	if (gds_alloc_state < gds_alloc_call_count)
	    gds_alloc_report (ALLOC_silent, __FILE__, __LINE__);

	/* Now zap the contents of the freed block - including the debug
	 * information as it is difficult to distinguish blocks freed
	 * by the suballocater vs blocks freed from a client
	 */
	strncpy (alloc->alloc_magic1, ALLOC_HEADER1_MAGIC_FREED, sizeof (alloc->alloc_magic1));

	save_length = alloc->alloc_status.alloc_length;
	memset (alloc, ALLOC_FREED_PATTERN, save_length);
	alloc->alloc_status.alloc_length = save_length;;
	}
    };
#endif

released = free_memory (blk);
blk = NULL;
V4_MUTEX_UNLOCK (&alloc_mutex);
return released;
}

#ifdef DEV_BUILD

void GDS_breakpoint (int parameter)
{
/**************************************
 *
 *	G D S _ b r e a k p o i n t 
 *
 **************************************
 *
 * Functional description
 *	Just a handy place to put a debugger breakpoint
 *	Calls to this can then be embedded for DEV_BUILD
 *	using the BREAKPOINT(x) macro.
 *
 **************************************/
#ifdef NETWARE_386
Breakpoint (parameter);
#endif

/* Put a breakpoint here via the debugger */
}
#endif

SINT64 API_ROUTINE isc_portable_integer (
    UCHAR	*ptr,
    SSHORT	length)
{
/**************************************
 *
 *	i s c _ p o r t a b l e _ i n t e g e r
 *
 **************************************
 *
 * Functional description
 *	Pick up (and convert) a Little Endian (VAX) style integer 
 *      of length 1, 2, 4 or 8 bytes to local system's Endian format.
 *
 *   various parameter blocks (i.e., dpb, tpb, spb) flatten out multibyte
 *   values into little endian byte ordering before network transmission.
 *   programs need to use this function in order to get the correct value
 *   of the integer in the correct Endian format.
 *
 * **Note**
 *
 *   This function is similar to gds__vax_integer() in functionality.
 *   The difference is in the return type. Changed from SLONG to SINT64
 *   Since gds__vax_integer() is a public API routine, it could not be
 *   changed. This function has been made public so gbak can use it.
 *
 **************************************/
SINT64	value;
SSHORT	shift;

assert (length <= 8);
value = shift = 0;

while (--length >= 0)
    {
    value += ((SINT64) *ptr++) << shift;
    shift += 8;
    }

return value;
}

#ifdef DEBUG_GDS_ALLOC

static void validate_memory (void)
{
/**************************************
 *
 *	v a l i d a t e _ m e m o r y
 *
 **************************************
 *
 * Functional description
 *	Utility function to call from the debugger, which sometimes
 *	doesn't like passing parameters.
 *
 **************************************/
gds_alloc_report (ALLOC_silent, "from debugger", 0);
}
#endif

#ifdef DEBUG_GDS_ALLOC

static void gds_alloc_validate (
    ALLOC	p)
{
/**************************************
 *
 *	g d s _ a l l o c _ v a l i d a t e
 *
 **************************************
 *
 * Functional description
 *	Test a memory buffer to see if it appears intact.
 *
 **************************************/
USHORT	errors = 0;

/* This might be a garbage pointer, so try and validate it first */

if (!p)
    {
    errors++;
    DEV_REPORT ("gds_alloc_validate: pointer doesn't look right\n");
    }

if (!errors && strncmp (p->alloc_magic1, ALLOC_HEADER1_MAGIC_ALLOCATED, sizeof (p->alloc_magic1)))
    {
    errors++;
    DEV_REPORT ("gds_alloc_validate: Header marker1 mismatch\n");
    }

if (!errors && strncmp (p->alloc_magic2, ALLOC_HEADER2_MAGIC, sizeof (p->alloc_magic2)))
    {
    errors++;
    DEV_REPORT ("gds_alloc_validate: Header marker2 mismatch\n");
    }

if (!errors && memcmp (((BLOB_PTR *)p) + ALLOC_HEADER_SIZE + p->alloc_requested_length,
	ALLOC_TAILER_MAGIC, ALLOC_TAILER_SIZE))
    {
    errors++;
    DEV_REPORT ("gds_alloc_validate: tailer marker mismatch\n");
    }

if (!errors && p->alloc_status.alloc_length < p->alloc_requested_length)
    {
    errors++;
    DEV_REPORT ("gds_alloc_validate: Header has been trashed (smaller than req)\n");
    }

if (!errors && p->alloc_status.alloc_length < ALLOC_OVERHEAD)
    {
    errors++;
    DEV_REPORT ("gds_alloc_validate: Header has been trashed (smaller than min)\n");
    }

if (errors)
    {
    UCHAR	buffer [150];
    sprintf (buffer, 
	    "gds_alloc_validate: %lx len=%5ld file=%15.30s line=%4ld call=%ld alloc=%ld free=%ld%s",
	    p,
	    p->alloc_requested_length,
	    p->alloc_filename,
	    p->alloc_lineno,
	    p->alloc_callno,
	    gds_alloc_call_count,
	    gds_free_call_count,
	    NEWLINE);
    DEV_REPORT (buffer);
    BREAKPOINT (__LINE__);
    }
}
#endif 

#ifdef DEBUG_GDS_ALLOC
static BOOLEAN gds_alloc_validate_free_pattern (
    register UCHAR	*ptr,
    register ULONG	len)
{
/**************************************
 *
 *	g d s _ a l l o c _ v a l i d a t e _ f r e e _ p a t t e r n
 *
 **************************************
 *
 * Functional description
 *	Walk a chunk of memory to see if it is all the same
 *	"color", eg: freed.
 *
 *	Return TRUE if the memory looks OK, FALSE if it has
 *	been clobbered.
 *
 **************************************/
while (len--)
    if (*ptr++ != ALLOC_FREED_PATTERN)
	{
	UCHAR	buffer [100];
	sprintf (buffer, "gds_alloc_validate_free_pattern: Write to freed memory at %lx\n", (ptr-1));
	DEV_REPORT (buffer);
	BREAKPOINT (__LINE__);
	return FALSE;
	}

return TRUE;
}
#endif DEBUG_GDS_ALLOC

#ifdef DEBUG_GDS_ALLOC
static void gds_alloc_validate_freed (
    ALLOC	p)
{
/**************************************
 *
 *	g d s _ a l l o c _ v a l i d a t e _ f r e e d
 *
 **************************************
 *
 * Functional description
 *	Test a memory buffer to see if it appears freed.
 *
 **************************************/
USHORT	errors = 0;

/* This might be a garbage pointer, so try and validate it first */

if (!p)
    {
    errors++;
    DEV_REPORT ("gds_alloc_validate_freed: pointer doesn't look right\n");
    }

/* Now look at the space, is it all the same pattern? */
if (!errors)
    {
    register UCHAR	*ptr;
    register ULONG	len;
    ptr = ((UCHAR *) p) + sizeof (struct free);
    len = p->alloc_status.alloc_freed.free_length - sizeof (struct free);
    if (!gds_alloc_validate_free_pattern (ptr, len))
	errors++;
    }

if (errors)
    {
    UCHAR	buffer [150];
    sprintf (buffer, 
	    "gds_alloc_validate_freed: %lx len=%5ld alloc=%ld free=%ld%s",
	    p,
	    p->alloc_status.alloc_freed.free_length,
	    gds_alloc_call_count,
	    gds_free_call_count,
	    NEWLINE);
    DEV_REPORT (buffer);
    BREAKPOINT (__LINE__);
    }
}
#endif 

#ifdef DEBUG_GDS_ALLOC

void gds_alloc_watch (
    void	*p)
{
/**************************************
 *
 *	g d s _ a l l o c _ w a t c h 
 *
 **************************************
 *
 * Functional description
 *	Start watching a memory location.
 *
 **************************************/

gds_alloc_watch_call_count++;

/* Do we have a new place to watch?  If so, set our watcher */
if (p)
    {
    gds_alloc_watchpoint = (ALLOC) ((UCHAR *) p - ALLOC_HEADER_SIZE);
    gds_alloc_watch_call_count = 1;
    }

/* If we have a watchpoint, check that it's still valid */
if (gds_alloc_watchpoint)
    gds_alloc_validate (gds_alloc_watchpoint);
}
#endif DEBUG_GDS_ALLOC

#ifdef DEBUG_GDS_ALLOC

void API_ROUTINE gds_alloc_flag_unfreed (
    void	*blk)
{
/**************************************
 *
 *	g d s _ a l l o c _ f l a g _ u n f r e e d
 *
 **************************************
 *
 * Functional description
 *	Flag a buffer as "known to be unfreed" so we
 *	don't report it in gds_alloc_report
 *
 **************************************/
ALLOC	p;

/* Point to the start of the block */
p = (ALLOC) (((UCHAR *) blk) - ALLOC_HEADER_SIZE);

/* Might as well validate it while we're here */
gds_alloc_validate (p);

/* Flag it as "already known" */
p->alloc_flags |= ALLOC_dont_report;

}
#endif /* DEBUG_GDS_ALLOC */

#ifdef DEBUG_GDS_ALLOC

void API_ROUTINE gds_alloc_report (
    ULONG	flags,
    UCHAR	*filename,
    ULONG	lineno)
{
/**************************************
 *
 *	g d s _ a l l o c _ r e p o r t 
 *
 **************************************
 *
 * Functional description
 *	Print buffers that might be memory leaks.
 *	Or that might have been clobbered.
 *
 **************************************/
ALLOC	p;
IB_FILE	*f = NULL;
UCHAR	buffer [150];
TEXT	name[MAXPATHLEN];
BOOLEAN	stderr_reporting = TRUE;

if (flags & ALLOC_check_each_call)
    gds_alloc_state = gds_alloc_call_count;
if (flags & ALLOC_dont_check)
    gds_alloc_state = ~0;

for (p = gds_alloc_chain; p; p = p->alloc_next)
    {
    gds_alloc_validate (p);
    if ((!(p->alloc_flags & ALLOC_dont_report) && !(flags & ALLOC_silent)) ||
       (flags & ALLOC_verbose))
	{
	if (f == NULL)
            {
#ifdef WINDOWS_ONLY
            f = ib_fopen ("C:\iserver.err", "at");
#else
	    gds__prefix (name, "iserver.err");
            f = ib_fopen (name, "at");
#endif
	    ib_fflush (ib_stdout);
	    ib_fflush (ib_stderr);
	    sprintf (buffer, "%sgds_alloc_report: flags %ld file %15.15s lineno %ld alloc=%ld free=%ld%s%s", 
			NEWLINE, 
			flags, 
			filename ? filename : (UCHAR *) "?",
			lineno,
			gds_alloc_call_count,
			gds_free_call_count,
			NEWLINE, NEWLINE);
	    if (getenv ("NO_GDS_ALLOC_REPORT"))
		stderr_reporting = FALSE;
	    if (stderr_reporting)
		DEV_REPORT (buffer);
	    ib_fprintf (f, buffer);
            }
	sprintf (buffer, 
	    "Unfreed alloc: %lx len=%5ld file=%15.30s line=%4ld call=%4ld%s",
	    p,
	    p->alloc_requested_length,
	    p->alloc_filename,
	    p->alloc_lineno,
	    p->alloc_callno,
	    NEWLINE);
	if (stderr_reporting)
	    DEV_REPORT (buffer);
	ib_fprintf (f, buffer);
	}
    if (flags & ALLOC_mark_current)
	p->alloc_flags |= ALLOC_dont_report;
    }

/* Walk the list of free blocks and validate they haven't been referenced */
for (p = (ALLOC) pool; p; p = (ALLOC) p->alloc_status.alloc_freed.free_next)
    gds_alloc_validate_freed (p);

if (f)
    ib_fclose (f);
}
#endif

SLONG API_ROUTINE gds__interprete (
    SCHAR	*s,
    STATUS	**vector)
{
/**************************************
 *
 *	g d s _ $ i n t e r p r e t e
 *
 **************************************
 *
 * Functional description
 *	Translate a status code with arguments to a string.  Return the
 *	length of the string while updating the vector address.  If the
 *	message is null (end of messages) or invalid, return 0;
 *
 **************************************/
TEXT		*p, *q, *temp;
SSHORT		l, temp_len; 
SLONG		len;
TEXT		**arg, *args [10];
STATUS		code, *v;
UCHAR		x;
STATUS		decoded;
#ifdef APOLLO
status_$t	apollo_status;
TEXT		*subsys_p, *module_p, *code_p;
SSHORT		subsys_l, module_l, code_l;
#endif
#ifdef VMS
STATUS		status;
TEXT		flags [4];
struct dsc$descriptor_s	desc;
#endif
#ifdef mpexl
t_mpe_status	code_stat, errmsg_stat;
STATUS		status;
SSHORT		errcode;
#endif

if (!**vector)
    return 0;

temp = NULL;

/* handle a case: "no errors, some warning(s)" */
if ((*vector) [1] == 0 && (*vector) [2] == isc_arg_warning)
    {
    v = *vector + 4;
    code = (*vector) [3];
    }
else
    {
    v = *vector + 2;
    code = (*vector) [1];
    }

arg = args;

/* Parse and collect any arguments that may be present */

for (;;)
    {
    x = (UCHAR) *v++ ;
    switch (x)
	{
	case gds_arg_string:
	case gds_arg_number:
	    *arg++ = (TEXT*) *v++;
	    continue;

	case gds_arg_cstring:
	    if (!temp)
		{
		/* We need a temporary buffer when cstrings are involved.
		   Give up if we can't get one. */

		p = temp = (TEXT*) gds__alloc ((SLONG) BUFFER_SMALL);
		temp_len = (SSHORT) BUFFER_SMALL;
		/* FREE: at procedure exit */
		if (!temp)		/* NOMEM: */
		    return 0;
		}
	    l = *v++;
	    q = (TEXT*) *v++;
	    *arg++ = p;

	    /* ensure that we do not overflow the buffer allocated */
	    l = (temp_len < l) ? temp_len : l;
	    if (l)
		do *p++ = *q++; while (--l);
	    *p++ = 0;
	    continue;

	default:
	    break;
	}
    --v;
    break;
    }

/* Handle primary code on a system by system basis */

switch ((UCHAR) (*vector)[0])
    {
    case isc_arg_warning:
    case gds_arg_gds:
	{
	USHORT fac = 0, class = 0;
	decoded = gds__decode (code, &fac, &class);
	if (gds__msg_format (NULL_PTR, fac, (USHORT) decoded,
		128, s, args [0], args [1], args [2], args [3], args [4]) < 0)
	    {
	    if ((decoded < (sizeof (messages)/sizeof (messages [0]))-1)
	       && (decoded >= 0))
	        sprintf (s, messages [decoded], 
		    args [0], args [1], args [2], args [3], args [4]);
	    else
		sprintf (s, "unknown ISC error %ld", code);	/* TXNN */
	    }
	}
	break;

    case gds_arg_interpreted:
	p = s;
	q = (TEXT*) (*vector) [1];
	while ((*p++ = *q++) != NULL)
	    ;
	break;

    case gds_arg_netware:
        if (code > 0 && code < sys_nerr && (p = sys_errlist [code]))
            strcpy (s, p);
        else if (code == 60)
            strcpy (s, "connection timed out");
        else if (code == 61)
            strcpy (s, "connection refused");
        else
            sprintf (s, "unknown NetWare error %ld", code);
        break;

    case gds_arg_unix:
#ifdef NETWARE_386
        if (code > 0 && code < sys_nerr && (p = sys_errlist [code]))
            strcpy (s, p);
        else if (code == 60)
            strcpy (s, "connection timed out");
        else if (code == 61)
            strcpy (s, "connection refused");
        else
            sprintf (s, "unknown NetWare error %ld", code);
#endif  

#ifndef NETWARE_386
#ifndef PC_PLATFORM
#if (defined OS2_ONLY && defined __IBMC__)
	if (p = strerror (code))
#else
	if (code > 0 && code < sys_nerr && (p = sys_errlist [code]))
#endif
	    strcpy (s, p);
	else if (code == 60)
	    strcpy (s, "connection timed out");
	else if (code == 61)
	    strcpy (s, "connection refused");
	else
#endif
	    sprintf (s, "unknown unix error %ld", code);    	/* TXNN */
#endif
	break;

    case gds_arg_dos:
#ifdef OS2_ONLY
	if (DosGetMessage (&s, 0, s, 128, code, "OSO001.MSG", &len))
	    sprintf (s, "unknown OS/2 error %ld", code);	/* TXNN */
	else
	    s [len] = 0;
#else
#ifdef DOS_ONLY
	if (code > 0 && code < _sys_nerr && (p = _sys_errlist [code]))
	    strcpy (s, p);
	else
        {
        SSHORT  number = MAP_DOS_ERROR(code);

        if (number >= 0)
            gds__msg_lookup (NULL_PTR, 20, number, 128, s, NULL);
        else if (number == -1)
    	    sprintf (s, "unknown network error %ld", (long)(SUB_DOS_BASE(code)));
        else
    	    sprintf (s, "unknown error %ld", code);
        }
#else
#ifdef NETWARE_386
	sprintf (s, "unknown Netware error %ld", code);		/* TXNN */
#else
	sprintf (s, "unknown dos error %ld", code);		/* TXNN */
#endif
#endif
#endif
	break;

#ifdef APOLLO
    case gds_arg_domain:
	apollo_status.all = code;
	error_$find_text (apollo_status, &subsys_p, &subsys_l, &module_p,
		&module_l, &code_p, &code_l);
	if (subsys_l && code_l)
	    sprintf (s, "%.*s (%.*s/%.*s)", 
		code_l, code_p, subsys_l, subsys_p,
		module_l, module_p);
	else
	    sprintf (s, "Domain status %X %.*s (%.*s/%.*s)", 	/* TXNN */
		code, code_l, code_p, subsys_l, subsys_p, 
		module_l, module_p);
	break;
#endif

#ifdef VMS
    case gds_arg_vms:
	l = 0;
	desc.dsc$b_class = DSC$K_CLASS_S;
	desc.dsc$b_dtype = DSC$K_DTYPE_T;
	desc.dsc$w_length = 128;
	desc.dsc$a_pointer = s;
	status = sys$getmsg (code, &l, &desc, 15, flags);
	if (status & 1)
	    s [l] = 0;
	else
	    sprintf (s, "uninterpreted VMS code %x", code);    /* TXNN */
	break;
#endif

#ifdef mpexl
    case gds_arg_mpexl:
	l = 128;
	if (code & 0xFFFF)
	    {
	    code_stat.word = code;
	    HPERRMSG (3, 1,, code_stat.decode, s, &l, &errmsg_stat.word);
	    status = errmsg_stat.word;
	    }
	else
	    {
	    errcode = code >> 16;
	    FERRMSG (&errcode, s, &l);
	    status = (ccode() == CCE) ? 0 : 1;
	    }
	if (!status)
	    s [l] = 0;
	else
	    {
	    errcode = code >> 16;
	    sprintf (s, "uninterpreted MPE/XL code %d from subsystem %d", errcode, (USHORT) code);  /* TXNN */
	    }
	break;

    case gds_arg_mpexl_ipc:
	len = 128;
	IPCERRMSG (code, s, &len, &status);
	if (!status)
	    s [len] = 0;
	else
	    sprintf (s, "uninterpreted MPE/XL IPC code %ld", code);    /* TXNN */
	break;
#endif

#ifdef NeXT
    case gds_arg_next_mach:
	if (code && (p = mach_errormsg ((kern_return_t) code)))
	    strcpy (s, p);
	else
	    sprintf (s, "unknown mach error %ld", code);    /* TXNN */
	break;
#endif

#ifdef WIN_NT
    case gds_arg_win32:
	if (!(l = FormatMessage (FORMAT_MESSAGE_FROM_SYSTEM,
		NULL,
		code,
		GetUserDefaultLangID(),
		s,
		128,
		NULL)))
	    sprintf (s, "unknown Win32 error %ld", code);  /* TXNN */
	break;
#endif

    default:
	if (temp)
	    gds__free ((SLONG*) temp);
	return 0;
    }


if (temp)
    gds__free ((SLONG*) temp);

*vector = v;
p = s;
while (*p)
    p++;

return p - s;
}

void API_ROUTINE gds__interprete_a (
    SCHAR	*s,
    SSHORT	*length,
    STATUS	*vector,
    SSHORT	*offset)
{
/**************************************
 *
 *	g d s _ $ i n t e r p r e t e _ a
 *
 **************************************
 *
 * Functional description
 *	Translate a status code with arguments to a string.  Return the
 *	length of the string while updating the vector address.  If the
 *	message is null (end of messages) or invalid, return 0;
 *	Ada being an unsatisfactory excuse for a language, fall back on
 *	the concept of indexing into the vector.
 *
 **************************************/
STATUS *v;

v = vector + *offset;
*length = gds__interprete (s, &v);
*offset = v - vector;
}

void API_ROUTINE gds__log (
    TEXT	*text,
    ...)
{
/**************************************
 *
 *	g d s _ l o g
 *
 **************************************
 *
 * Functional description
 *	Post an event to a log file.
 *
 **************************************/
va_list	ptr;
IB_FILE	*file;
SLONG	t [2], z [2];
int	oldmask;
#if defined(STACK_EFFICIENT) && !defined(DEV_BUILD) && !defined(DEBUG_GDS_ALLOC)
TEXT	*name = (TEXT *)gds__alloc ((SLONG)(sizeof (TEXT) * MAXPATHLEN));
#else	/* STACK_EFFICIENT */
TEXT	name [MAXPATHLEN];
#endif	/* STACK_EFFICIENT */

#if !(defined VMS || defined PC_PLATFORM || defined OS2_ONLY || defined WIN_NT || defined mpexl)
GETTIMEOFDAY (t, z);
#define LOG_TIME_OBTAINED
#endif

#ifdef LOG_TIME_OBTAINED
#undef LOG_TIME_OBTAINED
#else
time (t);
#endif

gds__prefix (name, LOGFILE);

#ifndef mpexl
oldmask = umask (0111);
#endif

if ((file = ib_fopen (name, FOPEN_APPEND_TYPE)) != NULL)
    {
#ifdef mpexl
    ISC_file_lock (_mpe_fileno (ib_fileno (file)));
#endif
    ib_fprintf (file, "%s%s\t%.25s\t", ISC_get_host (name, MAXPATHLEN), gdslogid, ctime (t));
    VA_START (ptr, text);
    ib_vfprintf (file, text, ptr);
    ib_fprintf (file, "\n\n");
#ifdef mpexl
    ISC_file_unlock (_mpe_fileno (ib_fileno (file)));
#endif
    ib_fclose (file);
    }

#ifndef mpexl
umask (oldmask);
#endif
#if defined(STACK_EFFICIENT) && !defined(DEV_BUILD) && !defined(DEBUG_GDS_ALLOC)
gds__free((SLONG *)name);
#endif /* STACK_EFFICIENT */
}

void API_ROUTINE gds__log_status (
    TEXT	*database,
    STATUS	*status_vector)
{
/**************************************
 *
 *	g d s _ $ l o g _ s t a t u s
 *
 **************************************
 *
 * Functional description
 *	Log error to error log.
 *
 **************************************/
TEXT	*buffer, *p;

buffer = (TEXT*) gds__alloc ((SLONG) BUFFER_XLARGE);
/* FREE: at procedure exit */
if (!buffer)		/* NOMEM: */
    return;

p = buffer;
sprintf (p, "Database: %s", (database) ? database : "");

do {
    while (*p)
	p++;
    *p++ = '\n';
    *p++ = '\t';
} while (gds__interprete (p, &status_vector));

p [-2] = 0;
gds__log (buffer, 0);
gds__free ((SLONG*) buffer);
}

int API_ROUTINE gds__msg_close (
    void	*handle)
{
/**************************************
 *
 *	g d s _ $ m s g _ c l o s e
 *
 **************************************
 *
 * Functional description
 *	Close the message file and zero out static variable.
 *
 **************************************/
MSG	message;
int	fd;

if (!(message = handle) && !(message = default_msg))
    return 0;

default_msg = (MSG) NULL;

fd = message->msg_file;

FREE_LIB_MEMORY (message);

if (fd <= 0)
    return 0;

return close (fd);
}

SSHORT API_ROUTINE gds__msg_format (
    void	*handle,
    USHORT	facility,
    USHORT	number,
    USHORT	length,
    TEXT	*buffer,
    TEXT	*arg1,
    TEXT	*arg2,
    TEXT	*arg3,
    TEXT	*arg4,
    TEXT	*arg5)
{
/**************************************
 *
 *	g d s _ $ m s g _ f o r m a t
 *
 **************************************
 *
 * Functional description
 *	Lookup and format message.  Return as much of formatted string
 *	as fits in caller's buffer.
 *
 **************************************/
TEXT	*p, *formatted, *end;
USHORT	l;
SLONG	size;
int	n;

size = (SLONG) (((arg1) ? MAX_ERRSTR_LEN : 0) +
		((arg2) ? MAX_ERRSTR_LEN : 0) +
		((arg3) ? MAX_ERRSTR_LEN : 0) +
		((arg4) ? MAX_ERRSTR_LEN : 0) +
		((arg5) ? MAX_ERRSTR_LEN : 0) + MAX_ERRMSG_LEN);

size = (size < length) ? length : size;

formatted = (TEXT*) gds__alloc ((SLONG) size);

if (!formatted)	/* NOMEM: */
    return -1;

/* Let's assume that the text to be output will never be shorter
   than the raw text of the message to be formatted.  Then we can
   use the caller's buffer to temporarily hold the raw text. */

n = gds__msg_lookup (handle, facility, number, length, buffer, NULL);

if (n > 0 && n < length)
    sprintf (formatted, buffer, arg1, arg2, arg3, arg4, arg5);
else
    {
    sprintf (formatted, "can't format message %d:%d -- ", facility, number);
    if (n == -1)
	strcat (formatted, "message text not found");
    else if (n == -2)
	{
	strcat (formatted, "message file ");
	for (p = formatted; *p;)
	    p++;
	gds__prefix_msg (p, MSG_FILE);
	strcat (p, " not found");
	}
    else
	{
	for (p = formatted; *p;)
	    p++;
	sprintf (p, "message system code %d", n);
	}
    }

l = strlen (formatted);
end = buffer + length - 1;

for (p = formatted; *p && buffer < end;)
    *buffer++ = *p++;
*buffer = 0;

gds__free ((SLONG*) formatted);
return ((n > 0) ? l : -l);
}

SSHORT API_ROUTINE gds__msg_lookup (
    void	*handle,
    USHORT	facility,
    USHORT	number,
    USHORT	length,
    TEXT	*buffer,
    USHORT	*flags)
{
/**************************************
 *
 *	g d s _ $ m s g _ l o o k u p
 *
 **************************************
 *
 * Functional description
 *	Lookup a message.  Return as much of the record as fits in the
 *	callers buffer.  Return the FULL size of the message, or negative
 *	number if we can't find the message.
 *
 **************************************/
MSG	message;
int	status;
USHORT	n;
MSGREC	leaf;		
MSGNOD	node, end;
ULONG	position, code;
TEXT	*p;

/* Handle default message file */

if (!(message = handle) && !(message = default_msg))
    {
    /* Try environment variable setting first */

    if ((p = getenv ("ISC_MSGS")) == NULL || 
	(status = gds__msg_open (&message, p)))
   	{
	TEXT	translated_msg_file [sizeof (MSG_FILE_LANG) + LOCALE_MAX + 1];
	TEXT	*msg_file;

	/* Try declared language of this attachment */
	/* This is not quite the same as the language declared on the
	   READY statement */

	msg_file = (TEXT*) gds__alloc ((SLONG) MAXPATHLEN);
	/* FREE: at block exit */
	if (!msg_file)		/* NOMEM: */
	    return -2;

	p = getenv ("LC_MESSAGES");
	if (p != NULL)
	    {
	    sanitize (p);
	    sprintf (translated_msg_file, MSG_FILE_LANG, p);
	    gds__prefix_msg (msg_file, translated_msg_file);
	    status = gds__msg_open (&message, msg_file);
	    }
	else
	    status = 1;
	if (status)
	    {
	    /* Default to standard message file */

	    gds__prefix_msg (msg_file, MSG_FILE);
	    status = gds__msg_open (&message, msg_file);
	    }
	gds__free ((SLONG*) msg_file);
	}

    if (status)
       return status;

    default_msg = message;
    }

/* Search down index levels to the leaf.  If we get lost, punt */

code = MSG_NUMBER (facility, number);
end = (MSGNOD) ((SCHAR*) message->msg_bucket + message->msg_bucket_size);
position = message->msg_top_tree;

for (n = 1, status = 0; !status; n++)
    {
    if (lseek (message->msg_file, position, 0) < 0)
	status = -6;
    else if (read (message->msg_file, message->msg_bucket, 
		   message->msg_bucket_size) < 0)
	status = -7;
    else if (n == message->msg_levels)
	break;
    else
	{
	for (node = (MSGNOD) message->msg_bucket; !status; node++)
	    {
	    if (node >= end)
		{
		status = -8;
		break;
		}
	    else if (node->msgnod_code >= code)
		{
		position = node->msgnod_seek;
		break;
		}
	    }
	}
    }

if (!status)
    {
    /* Search the leaf */
    for (leaf = (MSGREC) message->msg_bucket; !status; leaf = NEXT_LEAF (leaf))
	{
	if (leaf >= (MSGREC) end || leaf->msgrec_code > code)
	    {
 	    status = -1;
	    break;
	    }
	else if (leaf->msgrec_code == code)
	    {
	    /* We found the correct message, so return it to the user */
	    n = MIN (length - 1, leaf->msgrec_length);
	    memcpy ( buffer, leaf->msgrec_text, n);
	    buffer [n] = 0;
      
	    if (flags)
		*flags = leaf->msgrec_flags;

	    status = leaf->msgrec_length;
	    break;
	    }
	}
    }

#ifdef WINDOWS_ONLY
/*
** Currently, the onexit cleanup isn't working correctly for Windows,
** so we close the file every time to prevent files handles from being
** orphaned.
*/
gds__msg_close (NULL);
#endif  /* WINDOWS_ONLY */

return status;
}

int API_ROUTINE gds__msg_open (
    void	**handle,
    TEXT	*filename)
{
/**************************************
 *
 *	g d s _ $ m s g _ o p e n
 *
 **************************************
 *
 * Functional description
 *	Try to open a message file.  If successful, return a status of 0
 *	and update a message handle.
 *
 **************************************/
int	n;
MSG	message;
ISC_MSGHDR	header;

if ((n = open (filename, O_RDONLY | O_BINARY, 0)) < 0)
    return -2;

if (read (n, &header, sizeof (header)) < 0)
    {
    (void) close (n);
    return -3;
    }

if (header.msghdr_major_version != MSG_MAJOR_VERSION ||
    (SSHORT) header.msghdr_minor_version < MSG_MINOR_VERSION)
    {
    (void) close (n);
    return -4;
    }

message = (MSG) ALLOC_LIB_MEMORY ((SLONG) sizeof (struct msg) + header.msghdr_bucket_size - 1);
/* FREE: in gds__msg_close */
if (!message)		/* NOMEM: return non-open error */
    {
    (void) close (n);
    return -5;
    }

#ifndef WINDOWS_ONLY
#ifdef DEBUG_GDS_ALLOC
/* This will only be freed if the client closes the message file for us */
gds_alloc_flag_unfreed ((void *) message);
#endif
#endif

message->msg_file = n;
message->msg_bucket_size = header.msghdr_bucket_size;
message->msg_levels = header.msghdr_levels;
message->msg_top_tree = header.msghdr_top_tree;


*handle = message;

return 0;
}

void API_ROUTINE gds__msg_put (
    void	*handle,
    USHORT	facility,
    USHORT	number,
    TEXT	*arg1,
    TEXT	*arg2,
    TEXT	*arg3,
    TEXT	*arg4,
    TEXT	*arg5)
{
/**************************************
 *
 *	g d s _ $ m s g _ p u t
 *
 **************************************
 *
 * Functional description
 *	Lookup and format message.  Return as much of formatted string
 *	as fits in callers buffer.
 *
 **************************************/
#ifdef STACK_EFFICIENT
TEXT	*formatted = (TEXT *)gds__alloc((SLONG)(sizeof(TEXT) * BUFFER_MEDIUM));
#else	/* STACK_EFFICIENT */
TEXT	formatted [512];
#endif	/* STACK_EFFICIENT */

gds__msg_format (handle, facility, number, sizeof(TEXT) * BUFFER_MEDIUM, formatted, 
	arg1, arg2, arg3, arg4, arg5);
gds__put_error (formatted);
#ifdef STACK_EFFICIENT
gds__free((SLONG *)formatted);
#endif	/* STACK_EFFICIENT */
}

SLONG API_ROUTINE gds__get_prefix (
    SSHORT	arg_type,
    TEXT	*passed_string)
{
/**************************************
 *
 *	g d s _ $ g e t _ p r e f i x
 *
 **************************************
 *
 * Functional description
 *	Find appropriate InterBase command line arguments 
 *	for Interbase file prefix.
 *
 *      arg_type is 0 for $INTERBASE, 1 for $INTERBASE_LOCK 
 *      and 2 for $INTERBASE_MSG
 *
 *      Function returns 0 on success and -1 on failure
 **************************************/

char *prefix_ptr;
int count = 0;
char *prefix_array; /* = {ib_prefix, ib_prefix_lock, ib_prefix_msg}; */


if (arg_type < IB_PREFIX_TYPE || arg_type > IB_PREFIX_MSG_TYPE)
    return ((SLONG) -1);

switch (arg_type)
    {
    case IB_PREFIX_TYPE:
        prefix_ptr = ib_prefix = ib_prefix_val; 
        break;
    case IB_PREFIX_LOCK_TYPE:
        prefix_ptr = ib_prefix_lock = ib_prefix_lock_val; 
        break;
    case IB_PREFIX_MSG_TYPE:
        prefix_ptr = ib_prefix_msg = ib_prefix_msg_val; 
        break;
    default:
        return ((SLONG) -1);
    }
/* the command line argument was 'H' for interbase home */
while (*prefix_ptr++ = *passed_string++) 
    {
    /* if the next character is space, newline or carriage return OR
       number of characters exceeded */
    if (*passed_string == ' ' || *passed_string == 10 
        || *passed_string == 13 || (count == MAXPATHLEN))
        {
        *(prefix_ptr++) = '\0';
        break;
        }
    count++; 
    }
if (!count) 
    {
    prefix_ptr = NULL;
    return ((SLONG) -1);
    }
return ((SLONG) 0);
}

#ifndef VMS
void API_ROUTINE gds__prefix (
    TEXT	*string,
    TEXT	*root)
{
/**************************************
 *
 *	g d s _ $ p r e f i x	( n o n - V M S )
 *
 **************************************
 *
 * Functional description
 *	Find appropriate InterBase file prefix.
 *	Override conditional defines with
 *	the enviroment variable INTERBASE if it is set.
 *
 **************************************/

string [0] = 0;

if (ib_prefix == NULL)
    {
    if (!(ib_prefix = getenv (ISC_ENV)))
        {
#if (defined(WIN_NT) || defined(WINDOWS_ONLY))
        ib_prefix = ib_prefix_val;
#ifdef WIN_NT
	if (ISC_get_registry_var ("RootDirectory", ib_prefix, MAXPATHLEN, NULL_PTR) != -1)
#else   /* WIN_NT */
	if (GetProfileString ("InterBase", "RootDirectory", "c:/interbas/", ib_prefix,
 	    MAXPATHLEN) != 0)
#endif  /* WIN_NT */
    	    {
    	    TEXT *p = ib_prefix + strlen (ib_prefix);
    	    if (p != ib_prefix)
	        if (p [-1] == '\\')
	   	    p [-1] = '/';
	        else if (p [-1] != '/')
	            {
	            *p++ = '/';
	            *p = 0;
	            }
            }
	else strcpy (ib_prefix_val, ISC_PREFIX);
#else
	ib_prefix = ISC_PREFIX;
	strcat (ib_prefix_val, ib_prefix);
#endif
	ib_prefix = ib_prefix_val;
        }
    }
#ifdef mpexl
strcat (string, root);
strcat (string, ib_prefix);
#else
strcat (string, ib_prefix);
#ifndef NETWARE_386
if (string [strlen (string) - 1] != '/')
    strcat (string, "/");
#endif
strcat (string, root);
#endif
}
#endif

#ifdef VMS
void API_ROUTINE gds__prefix (
    TEXT	*string,
    TEXT	*root)
{
/**************************************
 *
 *	g d s _ $ p r e f i x	( V M S )
 *
 **************************************
 *
 * Functional description
 *	Find appropriate InterBase file prefix.
 *	Override conditional defines with
 *	the enviroment variable INTERBASE if it is set.
 *
 **************************************/
TEXT		temp [256], *p, *q;
ISC_VMS_PREFIX	prefix;
SSHORT		len;

if (*root != '[')
    {
    strcpy (string, root);
    return;
    }

/* Check for the existence of an InterBase logical name.  If there is 
   one use it, otherwise use the system directories. */

if (ISC_expand_logical_once (ISC_LOGICAL, sizeof (ISC_LOGICAL) - 2, temp))
    {
    strcpy (string, ISC_LOGICAL);
    strcat (string, root);
    return;
    }

for (p = temp, q = root; *p = UPPER7 (*q); q++)
    if (*p++ == ']')
	break;
    
len = p - temp;
for (prefix = trans_prefix; prefix->isc_prefix; prefix++)
    if (!strncmp (temp, prefix->isc_prefix, len))
	{
	strcpy (string, prefix->vms_prefix);
	strcat (string, &root [len]);
	return;
	}

strcpy (string, &root [len]);
}
#endif

#ifndef VMS
void API_ROUTINE gds__prefix_lock (
    TEXT	*string,
    TEXT	*root)
{
/********************************************************
 *
 *	g d s _ $ p r e f i x _ l o c k	( n o n - V M S )
 *
 ********************************************************
 *
 * Functional description
 *	Find appropriate InterBase lock file prefix.
 *	Override conditional defines with the enviroment
 *      variable INTERBASE_LOCK if it is set.
 *
 **************************************/
string [0] = 0;

if (ib_prefix_lock == NULL)
    {
    if (!(ib_prefix_lock = getenv (ISC_LOCK_ENV)))
        {
        ib_prefix_lock = ib_prefix_lock_val;
        gds__prefix(ib_prefix_lock, ""); 
        }
    else
        {
        strcat (ib_prefix_lock_val, ib_prefix_lock); 
        ib_prefix_lock = ib_prefix_lock_val;
        }
    }
#ifdef mpexl
strcat (string, root);
strcat (string, ib_prefix_lock);
#else
strcat (string, ib_prefix_lock);
#ifndef NETWARE_386
if (string [strlen (string) - 1] != '/')
    strcat (string, "/");
#endif
strcat (string, root);
#endif
}
#endif

#ifdef VMS
void API_ROUTINE gds__prefix_lock (
    TEXT	*string,
    TEXT	*root)
{
/************************************************
 *
 *	g d s _ $ p r e f i x _ l o c k	( V M S )
 *
 ************************************************
 *
 * Functional description
 *	Find appropriate InterBase lock file prefix.
 *	Override conditional defines with the enviroment
 *      variable INTERBASE_LOCK if it is set.
 *
 *************************************************/
TEXT		temp [256], *p, *q;
ISC_VMS_PREFIX	prefix;
SSHORT		len;

if (*root != '[')
    {
    strcpy (string, root);
    return;
    }

/* Check for the existence of an InterBase logical name.  If there is 
   one use it, otherwise use the system directories. */

if (ISC_expand_logical_once (ISC_LOGICAL_LOCK, sizeof (ISC_LOGICAL_LOCK) - 2, temp))
    {
    strcpy (string, ISC_LOGICAL_LOCK);
    strcat (string, root);
    return;
    }

for (p = temp, q = root; *p = UPPER7 (*q); q++)
    if (*p++ == ']')
	break;
    
len = p - temp;
for (prefix = trans_prefix; prefix->isc_prefix; prefix++)
    if (!strncmp (temp, prefix->isc_prefix, len))
	{
	strcpy (string, prefix->vms_prefix);
	strcat (string, &root [len]);
	return;
	}

strcpy (string, &root [len]);
}
#endif

#ifndef VMS
void API_ROUTINE gds__prefix_msg (
    TEXT        *string,
    TEXT        *root)
{
/********************************************************
 *
 *      g d s _ $ p r e f i x _ m s g ( n o n - V M S )
 *
 ********************************************************
 *
 * Functional description
 *      Find appropriate InterBase message file prefix.
 *      Override conditional defines with the enviroment
 *      variable INTERBASE_MSG if it is set.
 *
 *
 **************************************/
string [0] = 0;

if (ib_prefix_msg == NULL)
    {
    if (!(ib_prefix_msg = getenv (ISC_MSG_ENV)))
        {
        ib_prefix_msg = ib_prefix_msg_val;
        gds__prefix(ib_prefix_msg, ""); 
        }
    else
        {
        strcat (ib_prefix_msg_val, ib_prefix_msg); 
        ib_prefix_msg = ib_prefix_msg_val;
        }
    }

#ifdef mpexl
strcat (string, root);
strcat (string, ib_prefix_msg);
#else
strcat (string, ib_prefix_msg);
#ifndef NETWARE_386
if (string [strlen (string) - 1] != '/')
    strcat (string, "/");
#endif
strcat (string, root);
#endif
}
#endif

#ifdef VMS
void API_ROUTINE gds__prefix_msg (
    TEXT        *string,
    TEXT        *root)
{
/************************************************
 *
 *      g d s _ $ p r e f i x _ m s g ( V M S )
 *
 ************************************************
 *
 * Functional description
 *      Find appropriate InterBase message file prefix.
 *      Override conditional defines with the enviroment
 *      variable INTERBASE_MSG if it is set.
 *
 *************************************************/
TEXT            temp [256], *p, *q;
ISC_VMS_PREFIX  prefix;
SSHORT          len;

if (*root != '[')
    {
    strcpy (string, root);
    return;
    }


/* Check for the existence of an InterBase logical name.  If there is
   one use it, otherwise use the system directories. */

/* ISC_LOGICAL_MSG macro needs to be defined, check non VMS version of routine
   for functionality. */
if (ISC_expand_logical_once (ISC_LOGICAL_MSG, sizeof (ISC_LOGICAL_MSG) - 2, te
mp))
    {
    strcpy (string, ISC_LOGICAL_MSG);
    strcat (string, root);
    return;
    }

for (p = temp, q = root; *p = UPPER7 (*q); q++)
    if (*p++ == ']')
        break;

len = p - temp;
for (prefix = trans_prefix; prefix->isc_prefix; prefix++)
    if (!strncmp (temp, prefix->isc_prefix, len))
        {
        strcpy (string, prefix->vms_prefix);
        strcat (string, &root [len]);
        return;
        }

strcpy (string, &root [len]);
}
#endif

STATUS API_ROUTINE gds__print_status (
    STATUS	*vec)
{
/**************************************
 *
 *	g d s _ p r i n t _ s t a t u s
 *
 **************************************
 *
 * Functional description
 *	Interprete a status vector.
 *
 **************************************/
STATUS	*vector;
TEXT	*s;

if (!vec || (!vec [1] && vec [2] == gds_arg_end))
    return SUCCESS;

s = (TEXT*) gds__alloc ((SLONG) BUFFER_LARGE);
/* FREE: at procedure return */
if (!s)		/* NOMEM: */
    return vec [1];

vector = vec;

if (!gds__interprete (s, &vector))
    {
    gds__free ((SLONG*) s);
    return vec [1];
    }

gds__put_error (s);
s [0] = '-';

while (gds__interprete (s + 1, &vector))
    gds__put_error (s);

gds__free ((SLONG*) s);

return vec [1];
}

USHORT API_ROUTINE gds__parse_bpb (
    USHORT	bpb_length,
    UCHAR	*bpb,
    USHORT	*source,
    USHORT	*target)
{
/**************************************
 *
 *	g d s _ p a r s e _ b p b
 *
 **************************************
 *
 * Functional description
 *	Get blob type, source sub_type and target sub-type
 *	from a blob parameter block.
 *
 **************************************/

return gds__parse_bpb2 (bpb_length, bpb, source, target, NULL, NULL);
}

USHORT API_ROUTINE gds__parse_bpb2 (
    USHORT	bpb_length,
    UCHAR	*bpb,
    USHORT	*source,
    USHORT	*target,
    USHORT	*source_interp,
    USHORT	*target_interp)
{
/**************************************
 *
 *	g d s _ p a r s e _ b p b 2
 *
 **************************************
 *
 * Functional description
 *	Get blob type, source sub_type and target sub-type
 *	from a blob parameter block.
 *	Basically gds__parse_bpb() with additional:
 *	source_interp and target_interp.
 *
 **************************************/
UCHAR	*p, *end, op;
USHORT	type, length;

type = *source = *target = 0;

if (source_interp)
    *source_interp = 0;
if (target_interp)
    *target_interp = 0;

if (!bpb_length)
    return type;

p = bpb;
end = p + bpb_length;

if (*p++ != gds__bpb_version1)
    return type;

while (p < end)
    {
    op = *p++;
    length = *p++;
    switch (op)
	{
	case gds__bpb_source_type:
	    *source = gds__vax_integer (p, length);
	    break;

	case gds__bpb_target_type:
	    *target = gds__vax_integer (p, length);
	    break;

	case gds__bpb_type:
	    type = gds__vax_integer (p, length);
	    break;

	case gds__bpb_source_interp:
	    if (source_interp)
		*source_interp = gds__vax_integer (p, length);
	    break;

	case gds__bpb_target_interp:
	    if (target_interp)
		*target_interp = gds__vax_integer (p, length);
	    break;

	default:
	    break;
	}
    p += length;
    }

return type;
}

SLONG API_ROUTINE gds__ftof (
    register SCHAR	*string,
    register USHORT	GDS_VAL (length1),
    register SCHAR	*field,
    register USHORT	GDS_VAL (length2))
{
/**************************************
 *
 *	g d s _ f t o f
 *
 **************************************
 *
 * Functional description
 *	Move a fixed length string to a fixed length string.
 *	This is typically generated by the preprocessor to
 *	move strings around.
 *
 **************************************/
USHORT	l, fill;

fill = GDS_VAL (length2) - GDS_VAL (length1);

if ((l = MIN (GDS_VAL (length1), GDS_VAL (length2))) > 0)
    do *field++ = *string++; while (--l);

if (fill > 0)
    do *field++ = ' '; while (--fill);

return 0;
}

int API_ROUTINE gds__print_blr (
    UCHAR	*blr,
    void	(*routine)(),
    SCHAR	*user_arg,
    SSHORT	language)
{
/**************************************
 *
 *	g d s _ $ p r i n t _ b l r
 *
 **************************************
 *
 * Functional description
 *	Pretty print blr thru callback routine.
 *
 **************************************/
struct ctl	ctl, *control;
SCHAR		eoc;
SLONG		offset;
SSHORT		version, level;

if (SETJMP (env))
    return -1;

control = &ctl; 
level = 0;
offset = 0;

if (!routine)
    {
    routine = (void(*)()) PRINTF;
    user_arg = "%4d %s\n";
    }

control->ctl_routine = routine;
control->ctl_user_arg = user_arg;
control->ctl_blr = control->ctl_blr_start = blr;
control->ctl_ptr = control->ctl_buffer;
control->ctl_language = language;

version = BLR_BYTE;

if ((version != blr_version4) && (version != blr_version5))
    blr_error (control, "*** blr version %d is not supported ***", (TEXT*) version);

blr_format (control, (version==blr_version4)?"blr_version4,":"blr_version5,");
PRINT_LINE;
PRINT_VERB;

offset = control->ctl_blr - control->ctl_blr_start;
eoc = BLR_BYTE;

if (eoc != blr_eoc)
    blr_error (control, "*** expected end of command, encounted %d ***", (TEXT*) eoc);

blr_format (control, "blr_eoc");
PRINT_LINE;

return 0;
}

void API_ROUTINE gds__put_error (
    TEXT	*string)
{
/**************************************
 *
 *	g d s _ p u t _ e r r o r
 *
 **************************************
 *
 * Functional description
 *	Put out a line of error text.
 *
 **************************************/

#ifdef VMS
#define PUT_ERROR
struct dsc$descriptor_s	desc;

ISC_make_desc (string, &desc, 0);
lib$put_output (&desc);
#endif

#ifdef WINDOWS_ONLY
#define PUT_ERROR
/* This needs work since it shouldn't use a separate message box for each 
   line -- CM */

MessageBox (NULL, string, "InterBase Error", MB_ICONEXCLAMATION | MB_OK);
#endif

#ifdef OS2_ONLY
#define PUT_ERROR
TEXT	buffer [256], *p, c;
ULONG	length;

for (p = buffer; ((c = *string++) != 0) && c != '\n';)
    *p++ = c;

*p++ = '\n';
*p++ = '\r';
DosWrite (2, buffer, p - buffer, &length);
#endif

#ifdef NETWARE_386
#define PUT_ERROR
ConsolePrintf ("%s\n", string);
#endif

#ifdef PUT_ERROR
#undef PUT_ERROR
#else
ib_fputs (string, ib_stderr);
ib_fputc ('\n', ib_stderr);
ib_fflush (ib_stderr);
#endif
}

void API_ROUTINE gds__qtoq (
    void	*quad_in,
    void	*quad_out)
{
/**************************************
 *
 *	g d s _ q t o q
 *
 **************************************
 *
 * Functional description
 *	Move a quad value to another quad value.  This
 *      call is generated by the preprocessor when assigning
 *      quad values in FORTRAN.
 *
 **************************************/

*((GDS__QUAD*) quad_out) = *((GDS__QUAD*) quad_in);
}

void API_ROUTINE gds__register_cleanup (
  void (*routine)(),
  void *arg)
{
/* RDT: 20260318 - Registra função de cleanup. */
/**************************************
 *
 *	g d s _ r e g i s t e r _ c l e a n u p
 *
 **************************************
 *
 * Functional description
 *	Register a cleanup handler.
 *
 **************************************/

/* 
 * Ifdef out for windows client.  We have not implemented any way of 
 * determining when a task ends, therefore this never gets called.
*/

  CLEAN clean;

  if (!initialized)
    init();

  clean = (CLEAN) ALLOC_LIB_MEMORY ((SLONG) sizeof (struct clean));
  clean->clean_next = cleanup_handlers;
  cleanup_handlers = clean;
  clean->clean_routine = routine;
  clean->clean_arg = arg;

#ifdef DEBUG_GDS_ALLOC
  /* Startup function - known to be unfreed */
  gds_alloc_flag_unfreed ((void *)clean);
#endif
}


SLONG API_ROUTINE gds__sqlcode (
    STATUS	*status_vector)
{
/**************************************
 *
 *	g d s _ s q l c o d e
 *
 **************************************
 *
 * Functional description
 *	Translate GDS error code to SQL error code.  This is a little
 *	imprecise (to say the least) because we don't know the proper
 *	SQL codes.  One must do what what can; stiff upper lip, and all
 *	that.
 *
 *	Scan a status vector - if we find gds__sqlerr in the "right"
 *	positions, return the code that follows.  Otherwise, for the
 *	first code for which there is a non-generic SQLCODE, return it.
 *
 **************************************/
USHORT	code;
SLONG	sqlcode;
STATUS	*s;
USHORT	have_sqlcode;

if (!status_vector)
    {
    DEV_REPORT ("gds__sqlcode: NULL status vector");
    return GENERIC_SQLCODE;
    }

have_sqlcode = FALSE;
sqlcode = GENERIC_SQLCODE;		/* error of last resort */

/* SQL code -999 (GENERIC_SQLCODE) is generic, meaning "no other sql code
 * known".  Now scan the status vector, seeing if there is ANY sqlcode
 * reported.  Make note of the first error in the status vector who's 
 * SQLCODE is NOT -999, that will be the return code if there is no specific
 * sqlerr reported.
 */

s = status_vector;
while (*s != gds_arg_end)
    {
    if (*s == gds_arg_gds)
	{
	s++;
	if (*s == gds__sqlerr)
	    {
	    return *(s+2);
	    }

	if (!have_sqlcode)
	    {
            /* Now check the hard-coded mapping table of gds_codes to
	       sql_codes */
	    USHORT fac = 0, class = 0;

            code = gds__decode (status_vector [1], &fac, &class);

	    if ((code < sizeof (gds__sql_code) / sizeof (gds__sql_code [0]))
		&& (gds__sql_code [code] != GENERIC_SQLCODE))
		{
		sqlcode = gds__sql_code [code];
		have_sqlcode = TRUE;
		}
	    }
	s++;
	}
    else if (*s == gds_arg_cstring)
	s += 3;	/* skip: gds_arg_cstring <len> <ptr> */
    else
	s += 2;	/* skip: gds_arg_* <item> */
    };

return sqlcode;
}

void API_ROUTINE gds__sqlcode_s (
    STATUS	*status_vector,
    ULONG	*sqlcode)
{
/**************************************
 *
 *	g d s _ s q l c o d e _ s
 *
 **************************************
 *
 * Functional description
 *	Translate GDS error code to SQL error code.  This is a little
 *	imprecise (to say the least) because we don't know the proper
 *	SQL codes.  One must do what what can; stiff upper lip, and all
 *	that.  THIS IS THE COBOL VERSION.  (Some cobols son't have 
 *	return values for calls...
 *
 **************************************/

*sqlcode = gds__sqlcode (status_vector);
}

UCHAR * API_ROUTINE gds__sys_alloc (
    SLONG	size)
{
/**************************************
 *
 *	g d s _ $ s y s _ a l l o c
 *
 **************************************
 *
 * Functional description
 *	Allocate a block of system memory.   This
 *	memory may or may not, depending on the
 *	platform, be part of InterBase's suballocated
 *	memory pool.
 *
 **************************************/

#ifndef SUPERSERVER
return gds__alloc (size);

#else
UCHAR	*memory;
ULONG	*block;

size += ALLOC_OVERHEAD;
size = ALLOC_ROUNDUP (size);

#ifdef NETWARE
REPLACE THIS ILLEGAL COMMENT WITH NETWARE IMPLEMENTATION

Malloc/free may be good enough as long as free() returns
the memory back to a system-wide heap.
#define SYS_ALLOC_DEFINED
#endif

#ifdef UNIX
#ifdef MAP_ANONYMOUS

memory = mmap (NULL, size, (PROT_READ | PROT_WRITE),
	       (MAP_ANONYMOUS | 
#ifndef LINUX
/* In LINUX, there is no such thing as MAP_VARIABLE. Hence, it gives 
   compilation error. The equivalent functionality is default, 
   if you do not specify MAP_FIXED */
		MAP_VARIABLE | 
#endif  /* LINUX */
		MAP_PRIVATE), -1, 0);
if (memory == (UCHAR*) -1)
    {
    if (errno == ENOMEM)
    	return NULL;
    else
	ERR_post (isc_sys_request, isc_arg_string, "mmap", isc_arg_unix, errno, 0);
    }
#define SYS_ALLOC_DEFINED
#else
#ifdef SOLARIS

if (!(memory = mmap_anon (size)))	/* Jump thru hoops for Solaris. */
    return NULL;
#define SYS_ALLOC_DEFINED
#endif
#endif	/* MAP_ANONYMOUS */
#endif	/* UNIX */

#ifdef VMS
{
SLONG	status;

status = lib$get_vm (&size, &memory);
if (!(status & 1))
    return NULL;
}
#define SYS_ALLOC_DEFINED
#endif

#ifdef WIN_NT

if (!(memory = VirtualAlloc (NULL, size, MEM_COMMIT, PAGE_READWRITE)))
    {
    return NULL;
/***
    DWORD error = GetLastError();

    if (error == ERROR_NOT_ENOUGH_MEMORY)
	return NULL;
    else
    	ERR_post (gds__sys_request, gds_arg_string, "VirtualAlloc",
        	  gds_arg_win32, error, 0);
***/
    }
#define SYS_ALLOC_DEFINED
#endif

#ifdef SYS_ALLOC_DEFINED
block = (ULONG*) memory;
*block = size;
return memory + ALLOC_HEADER_SIZE;
#else
return NULL;
#endif

#endif	/* SUPERSERVER */
}

SLONG API_ROUTINE gds__sys_free (
    void	*blk)
{
/**************************************
 *
 *	g d s _ $ s y s _ f r e e
 *
 **************************************
 *
 * Functional description
 *	Free system memory back where it came from.
 *
 **************************************/

#ifndef SUPERSERVER
return gds__free (blk);

#else
ULONG	*block, length;

block = (ULONG*) ((UCHAR*) blk - ALLOC_HEADER_SIZE);
blk = (void*) block;
length = *block;

#ifdef NETWARE
REPLACE THIS ILLEGAL COMMENT WITH NETWARE IMPLEMENTATION

Malloc/free may be good enough as long as free() returns
the memory back to a system-wide heap.
#define SYS_FREE_DEFINED
#endif

#ifdef UNIX
if (munmap (blk, length) == -1)
    ERR_post (isc_sys_request, isc_arg_string, "munmap", isc_arg_unix, errno, 0);
#define SYS_FREE_DEFINED
#endif

#ifdef VMS
{
SLONG	status;

status = lib$free_vm (&length, &blk);
if (!(status & 1))
    ERR_post (isc_sys_request, isc_arg_string, "lib$free_vm", isc_arg_vms, status, 0);
}
#define SYS_FREE_DEFINED
#endif

#ifdef WIN_NT
if (!VirtualFree ((LPVOID) blk, (DWORD) 0, MEM_RELEASE))
    ERR_post (gds__sys_request, gds_arg_string, "VirtualFree",
              gds_arg_win32, GetLastError(), 0);
#define SYS_FREE_DEFINED
#endif

#ifdef SYS_FREE_DEFINED
return length - ALLOC_OVERHEAD;
#else
return 0L;
#endif
#endif	/* SUPERSERVER */
}

#ifndef mpexl
void * API_ROUTINE gds__temp_file (
    BOOLEAN	flag,
    TEXT	*string,
    TEXT	*expanded_string)
{
/**************************************
 *
 *	g d s _ $ t e m p _ f i l e
 *
 **************************************
 *
 * Functional description
 *	Create and open a temp file with a given root.  Unless the
 *	address of a buffer for the expanded file name string is
 *	given, make up the file "pre-deleted". Return -1 on failure.
 *
 **************************************/

return ( gds__tmp_file2 (flag, string, expanded_string, NULL) );
}

void * gds__tmp_file2 (
    BOOLEAN	flag,
    TEXT	*string,
    TEXT	*expanded_string,
    TEXT	*dir)
{
/**************************************
 *
 *      g d s _ _ t m p _ f i l e 2
 *
 **************************************
 *
 * Functional description
 *      Create and open a temp file with a given location.
 *      Unless the address of a buffer for the expanded file name string is
 *      given, make up the file "pre-deleted". Return -1 on failure.
 *
 **************************************/

IB_FILE	*file;
TEXT	file_name [256], *p, *q, *end, temp_dir [256];
SSHORT	i;

p = file_name;
end = p + sizeof (file_name) - 8;

#ifdef VMS
q = WORKFILE;
#else
if ( !(q = dir) &&  !(q = getenv ("INTERBASE_TMP")) && !(q = getenv ("TMP")))
#ifdef WIN_NT
    if ((i = GetTempPath (sizeof (temp_dir), temp_dir)) &&
	i < sizeof (temp_dir))
	q = temp_dir;
    else
#endif 
#ifdef WINDOWS_ONLY
    if (!(q = getenv ("TEMP")))
#endif 
	q = WORKFILE;
#endif /* VMS */

for ( ; p < end && *q;)
    *p++ = *q++;

#ifndef VMS
#if (defined PC_PLATFORM || defined OS2_ONLY || defined WIN_NT)
if (p > file_name && p [-1] != '\\' && p [-1] != '/' && p < end)
    *p++ = '\\';
#else
if (p > file_name && p [-1] != '/' && p < end)
    *p++ = '/';
#endif
#endif

for (q = string; p < end && *q;)
    *p++ = *q++;

for (q = TEMP_PATTERN; *q;)
    *p++ = *q++;

*p = 0;
MKTEMP (file_name, string);

if (expanded_string)
    strcpy (expanded_string, file_name);

#ifdef VMS
if (flag)
    {
    file = (expanded_string) ?
	ib_fopen (file_name, "w") :
	ib_fopen (file_name, "w", "fop=tmd");
    if (!file)
 	return (void*) -1;
    }
else
    {
    file = (expanded_string) ?
	open (file_name, O_RDWR | O_CREAT | O_TRUNC, 0600) :
	open (file_name, O_RDWR | O_CREAT | O_TRUNC, 0600, "fop=tmd");
    if (file == (IB_FILE*) -1)
	return (void*) -1;
    }
#else
if (flag)
    {
    for (i = 0; i < IO_RETRY; i++)
	{
	if (file = ib_fopen (file_name, "w+"))
   	    break;
#ifndef NETWARE_386
	if (!SYSCALL_INTERRUPTED(errno))
	    return (void*) -1;
#endif
	}
    if (!file)
        return (void*) -1;
    }
else
    {
    for (i = 0; i < IO_RETRY; i++)
	{
	file = (IB_FILE*) (int) open (file_name, O_RDWR | O_CREAT | O_TRUNC | O_BINARY, 0600);
	if (file != (IB_FILE*) -1)
   	    break;
#ifndef NETWARE_386
        if (!SYSCALL_INTERRUPTED(errno))
	    return (void*) -1;
#endif
	}
    if (file == (IB_FILE*) -1)
	return (void*) -1;
    }

if (!expanded_string)
    unlink (file_name);
#endif

return (void*) file;
}
#endif /* ifndef mpexl */

#ifdef mpexl
void * API_ROUTINE gds__temp_file (
    BOOLEAN	flag,
    TEXT	*string,
    TEXT	*expanded_string)
{
/**************************************
 *
 *	g d s _ $ t e m p _ f i l e
 *
 **************************************
 *
 * Functional description
 *	Create and open a temp file with a given root.  Unless the
 *	address of a buffer for the expanded file name string is
 *	given, make up the file "pre-deleted".  Return -1 on failure.
 *
 *	It appears that files with names longer than 32 characters
 *	don't open, so truncate overly ambitious input strings.
 *
 **************************************/
IB_FILE	*file;
TEXT	file_name [33], *p, *q, *end;

p = file_name;

for (q = string, end = q + 2; q < end && *q;)
    *p++ = *q++;

for (q = TEMP_PATTERN; *q;)
    *p++ = *q++;

if (!(q = getenv ("ISC_TMP")))
    q = WORKFILE;

while (*q)
    *p++ = *q++;

*p = 0;
mktemp (file_name);

if (expanded_string)
    strcpy (expanded_string, file_name);

if (flag)
    {
    file = (expanded_string) ?
	ib_fopen (file_name, "w V Ds2 E32 S256000") :
	ib_fopen (file_name, "w V Ds2 E32 S256000 Te");
    if (!file)
	return (void*) -1;
    }
else
    {
    file = (expanded_string) ?
	open (file_name, O_RDWR | O_CREAT | O_TRUNC | O_MPEOPTS, 0600, "b Ds2 E32 S256000") :
	open (file_name, O_RDWR | O_CREAT | O_TRUNC | O_MPEOPTS, 0600, "b Ds2 E32 S256000 Te");
    if ((int) file == -1)
	return (void*) -1;
    }

return (void*) file;
}
#endif
 
#ifdef APOLLO
int API_ROUTINE gds__termtype (void)
{
/**************************************
 *
 *	g d s _ $ t e r m t y p e
 *
 **************************************
 *
 * Functional description
 *	Determine class of terminal.
 *
 **************************************/
SLONG			input_mask, error_mask, ios_mask;
status_$t		status;
stream_$ir_rec_t	data;

if ((ios_$inq_conn_flags (stream_$errout, status) & ios_$cf_vt_mask) ||
    (ios_$inq_conn_flags (stream_$ib_stdin, status) & ios_$cf_vt_mask) ||
    (ios_$inq_conn_flags (stream_$ib_stdout, status) & ios_$cf_vt_mask))
    return TERM_apollo_dm;

input_mask = (1 << 22);
data.strid = stream_$ib_stdout;
stream_$inquire (input_mask, stream_$use_strid,	data, error_mask, status);

if (status.all)
    return 0;

if (data.otype == pad_$uid)
    return TERM_apollo_dm;

if (data.otype == pty_$slave_uid)
    return TERM_rlogin;

if (data.otype == pty_$uid)
    return TERM_telnet;

if (data.otype == mbx_$uid)
    return TERM_crp;

if (data.otype == sio_$uid)
    return TERM_sio;

/*
if (ios_mask & ios_$cf_tty_mask)
    return TERM_sio;
*/

if (data.otype == uasc_$uid)
    return TERM_file;

if (data.otype == nulldev_$uid)
    return TERM_server;

return 0;
}
#endif

void API_ROUTINE gds__unregister_cleanup (
    void	(*routine)(),
    void	*arg)
{
/**************************************
 *
 *	g d s _ u n r e g i s t e r _ c l e a n u p
 *
 **************************************
 *
 * Functional description
 *	Unregister a cleanup handler.
 *
 **************************************/
CLEAN	*clean_ptr, clean;

for (clean_ptr = &cleanup_handlers; clean = *clean_ptr; clean_ptr = &clean->clean_next)
    if (clean->clean_routine == routine && clean->clean_arg == arg)
	{
	*clean_ptr = clean->clean_next;
	FREE_LIB_MEMORY (clean);
	break;
	}
}

#ifndef VMS
BOOLEAN API_ROUTINE gds__validate_lib_path (
    TEXT	*module,
    TEXT	*ib_env_var,
    TEXT	*resolved_module,
    SLONG	length)
{
/**************************************
 *
 *	g d s _ $ v a l i d a t e _ l i b _ p a t h	( n o n - V M S )
 *
 **************************************
 *
 * Functional description
 *	Find the InterBase external library path variable.
 *	Validate that the path to the library module name 
 *	in the path specified.  If the external lib path
 *	is not defined then accept any path, and return 
 *	TRUE. If the module is in the path then return TRUE 
 * 	else, if the module is not in the path return FALSE.
 *
 **************************************/
TEXT	*p, *q;
TEXT	*token;
TEXT	abs_module[MAXPATHLEN];
TEXT	abs_module_path[MAXPATHLEN];
TEXT	abs_path[MAXPATHLEN];
TEXT	path[MAXPATHLEN];
TEXT	temp_ib_lib_path[MAXPATHLEN];
TEXT 	*ib_ext_lib_path = NULL_PTR;

if (!(ib_ext_lib_path = getenv (ib_env_var)))
    {
#ifdef WIN_NT
    ib_ext_lib_path = temp_ib_lib_path;
    if (ISC_get_registry_var (ib_env_var, ib_ext_lib_path, MAXPATHLEN, NULL_PTR) <= 0)
#endif /* WIN_NT */
	{
	strncpy (resolved_module, module, length);
	return TRUE;	/* The variable is not defined.  Retrun TRUE */
	}
    }

if (EXPAND_PATH (module, abs_module))
    {
    /* Extract the path from the absolute module name */
    for (p = abs_module, q = NULL; *p; p++)
	if ((*p == '\\') || (*p == '/'))
	    q = p;

    memset (abs_module_path, 0, MAXPATHLEN);
    strncpy (abs_module_path, abs_module, q - abs_module);

    /* Check to see if the module path is in the lib path
       if it is return TURE.  If it does not find it, then
       the module path is not valid so return FALSE */
    token = strtok (ib_ext_lib_path, ";");
    while (token != NULL)
	{
	strcpy (path, token);
	/* make sure that there is no traing slash on the path */
	p = path + strlen (path);
	if ((p != path) && ((p [-1] == '/') || (p [-1] == '\\')))
	    p [-1] = 0;
	if ((EXPAND_PATH (path, abs_path)) && (!COMPARE_PATH (abs_path, abs_module_path)))
	    {
	    strncpy (resolved_module, abs_module, length);
	    return TRUE;
	    }
	token = strtok (NULL, ";");
	}
    }
return FALSE;
}
#endif

#ifdef VMS
BOOLEAN API_ROUTINE gds__validate_lib_path (
    TEXT	*module,
    TEXT	*ib_env_var,
    TEXT	*resolved_module,
    SLONG	length)
{
/**************************************
 *
 *	g d s _ $ v a l i d a t e _ l i b _ p a t h	( V M S )
 *
 **************************************
 *
 * Functional description
 *	Find the InterBase external library path variable.
 *	Validate that the path to the library module name 
 *	in the path specified.  If the external lib path
 *	is not defined then accept any path, and return true.
 * 	If the module is not in the path return FALSE.
 *
 **************************************/
TEXT	*p, *q;
TEXT	path[MAXPATHLEN];
TEXT	abs_module_path[MAXPATHLEN];
TEXT	abs_module[MAXPATHLEN];

/* Check for the existence of an InterBase logical name.  If there is 
   one use it, otherwise use the system directories. */

COMPILER ERROR!  BEFORE DOING A VMS POST PLEASE
MAKE SURE THAT THIS FUNCTION WORKS THE SAME WAY
AS THE NON-VMS ONE DOES.

if (!(ISC_expand_logical_once (ib_env_var, strlen (ib_env_var) - 2, path)))
    return TRUE;

if (ISC_expand_logical_once (module, strlen (module) - 2, abs_module))
    {
    /* Extract the path from the module name */
    for (p = abs_module, q = NULL; *p; p++)
	if (*p == ']')
	    q = p;

    memset (abs_module_path, 0, MAXPATHLEN); 
    strncpy (abs_module_path, abs_module, q - abs_module + 1);

    /* Check to see if the module path is the same as lib path
       if it is return TURE.  If not then the module path is 
       not valid so return FALSE */
    if (!strcmp (path, abs_module_path))
	return TRUE;
    }
return FALSE;
}
#endif

SLONG API_ROUTINE gds__vax_integer (
    UCHAR	*ptr,
    SSHORT	length)
{
/**************************************
 *
 *	g d s _ $ v a x _ i n t e g e r
 *
 **************************************
 *
 * Functional description
 *	Pick up (and convert) a VAX style integer of length 1, 2, or 4
 *	bytes.
 *
 **************************************/
SLONG	value;
SSHORT	shift;

value = shift = 0;

while (--length >= 0)
    {
    value += ((SLONG) *ptr++) << shift;
    shift += 8;
    }

return value;
}

void API_ROUTINE gds__vtof (
    register SCHAR	*string,
    register SCHAR	*field,
    register USHORT	length)
{
/**************************************
 *
 *	g d s _ v t o f
 *
 **************************************
 *
 * Functional description
 *	Move a null terminated string to a fixed length
 *	field.  The call is primarily generated  by the
 *	preprocessor.
 *
 **************************************/

while (*string)
    {
    *field++ = *string++;
    if (--length <= 0)
	return;
    }

if (length > 0)
    do *field++ = ' '; while (--length);
}

void API_ROUTINE gds__vtov (
    register SCHAR	*string,
    register SCHAR	*field,
    register SSHORT	length)
{
/**************************************
 *
 *	g d s _ v t o v
 *
 **************************************
 *
 * Functional description
 *	Move a null terminated string to a fixed length
 *	field.  The call is primarily generated  by the
 *	preprocessor.  Until gds__vtof, the target string
 *	is null terminated.
 *
 **************************************/

--length;

while ((*field++ = *string++) != NULL)
    if (--length <= 0)
	{
	*field = 0;
	return;
	}
}

void API_ROUTINE isc_print_sqlerror (
    SSHORT	sqlcode,
    STATUS	*status)
{
/**************************************
 *
 *	i s c _ p r i n t _ s q l e r r o r
 *
 **************************************
 *
 * Functional description
 *	Given an sqlcode, give as much info as possible.
 *      Decide whether status is worth mentioning.
 *
 **************************************/
TEXT	error_buffer [192], *p;

sprintf (error_buffer, "SQLCODE: %d\nSQL ERROR:\n", sqlcode); 
for (p = error_buffer; *p;)
    p++;
isc_sql_interprete (sqlcode, p, sizeof (error_buffer) - (p - error_buffer) - 2);
while (*p)
    p++;
*p++ = '\n';
*p = 0;
gds__put_error (error_buffer);

if (status && status [1])
    {
    gds__put_error ("ISC STATUS: ");
    gds__print_status (status);
    }
}

void API_ROUTINE isc_sql_interprete (
    SSHORT	sqlcode,
    TEXT	*buffer,
    SSHORT	length)
{
/**************************************
 *
 *	i s c _ s q l _ i n t e r p r e t e
 *
 **************************************
 *
 * Functional description
 *	Given an sqlcode, return a buffer full of message
 *	This is very simple because all we have is the
 *      message number, and the sqlmessages have as yet no
 *	arguments
 *
 *	NOTE: As of 21-APR-1999, sqlmessages HAVE arguments hence use
 *	      an empty string instead of NULLs.
 *	      
 **************************************/
TEXT	*str = "";

if (sqlcode < 0)
    gds__msg_format (NULL_PTR, 13, (1000 + sqlcode), length, buffer,
		     str, str, str, str, str);
else
    gds__msg_format (NULL_PTR, 14, sqlcode, length, buffer,
		     str, str, str, str, str);
}

#ifdef NETWARE_386
int gettimeofday (
    struct timeval	*tp,
    struct timezon	*tzp)
{
/**************************************
 *
 *      g e t t i m e o f d a y		( N E T W A R E )
 *
 **************************************
 *
 * Functional description
 *      Compute time of day on NETWARE.
 *
 **************************************/
time_t    buffer;

time (&buffer);

if (tp)
    {
    tp->tv_sec = (SLONG) buffer;		/* since 1/1/1970 */
    tp->tv_usec =  0L;
    }
if (tzp)
    {
    tzset();
    tzp->tz_minuteswest = timezone/60;		/* Netware in seconds */
    tzp->tz_dsttime = daylight;
    }

return 0;
}
#endif

#ifdef VMS
int unlink (
    SCHAR	*file)
{
/**************************************
 *
 *	u n l i n k
 *
 **************************************
 *
 * Functional description
 *	Get rid of a file and all of its versions.
 *
 **************************************/
int			status;
struct dsc$descriptor_s	desc;

ISC_make_desc (file, &desc, 0);

for (;;)
    {
    status = lib$delete_file (&desc);
    if (!(status & 1))
	break;
    }

if (!status)
    return 0;
else if (status != RMS$_FNF)
    return -1;
else
    return 0;
}
#endif

#ifdef  WINDOWS_ONLY
void    gdswep( void)
{
/**************************************
 *
 *      g d s w e p
 *
 **************************************
 *
 * Functional description
 *      Call cleanup_malloced_memory for WEP.
 *
 **************************************/

cleanup_malloced_memory( NULL);
}
#endif

static void blr_error (
    CTL		control,
    TEXT	*string,
    TEXT	*arg1)
{
/**************************************
 *
 *	b l r _ e r r o r
 *
 **************************************
 *
 * Functional description
 *	Report back an error message and unwind.
 *
 **************************************/
USHORT	offset;

blr_format (control, string, arg1);
offset = 0;
PRINT_LINE;
LONGJMP (env, -1);
}

static void blr_format (
    CTL		control,
    TEXT	*string,
    ...)
{
/**************************************
 *
 *	b l r _ f o r m a t
 *
 **************************************
 *
 * Functional description
 *	Format an utterance.
 *
 **************************************/
va_list	ptr;

VA_START (ptr, string);
vsprintf (control->ctl_ptr, string, ptr);
while (*control->ctl_ptr)
   control->ctl_ptr++;
}

static void blr_indent (
    CTL		control,
    SSHORT	level)
{
/**************************************
 *
 *	b l r _ i n d e n t
 *
 **************************************
 *
 * Functional description
 *	Indent for pretty printing.
 *
 **************************************/

level *= 3;
while (--level >= 0)
    PUT_BYTE (' ');
}


static void blr_print_blr (
    CTL		control,
    UCHAR	operator)
{
/**************************************
 *
 *	b l r _ p r i n t _ b l r
 *
 **************************************
 *
 * Functional description
 *	Print a blr item.
 *
 **************************************/
SCHAR	*p;

if (operator > (sizeof (blr_table) / sizeof (blr_table [0])) ||
    ! (p = blr_table [operator].blr_string))
    blr_error (control, "*** blr operator %d is undefined ***", (TEXT*) operator);

blr_format (control, "blr_%s, ", p);
}

static SCHAR blr_print_byte (
    CTL	control)
{
/**************************************
 *
 *	b l r _ p r i n t _ b y t e
 *
 **************************************
 *
 * Functional description
 *	Print a byte as a numeric value and return same.
 *
 **************************************/
UCHAR	v;

v = BLR_BYTE;
blr_format (control, (control->ctl_language) ? "chr(%d), " : "%d, ", (TEXT*) v);

return v;
}

static blr_print_char (
    CTL	control)
{
/**************************************
 *
 *	b l r _ p r i n t _ c h a r
 *
 **************************************
 *
 * Functional description
 *	Print a byte as a numeric value and return same.
 *
 **************************************/
SCHAR	c;
UCHAR	v;
SSHORT	printable;

v = c = BLR_BYTE;
printable = (c >= 'a' && c <= 'z') ||
	    (c >= 'A' && c <= 'Z') ||
	    (c >= '0' && c <= '9' ||
	     c == '$' || c == '_');

if (printable)
    blr_format (control, "'%c',", (TEXT*) c);
else if (control->ctl_language)
    blr_format (control, "chr(%d),", (TEXT*) v);
else
    blr_format (control, "%d,", (TEXT*) c);

return c;
}

static void blr_print_cond (
    CTL	control)
{
/**************************************
 *
 *	b l r _ p r i n t _ c o n d
 *
 **************************************
 *
 * Functional description
 *	Print an error condition.
 *
 **************************************/
USHORT	ctype;
SSHORT	n;

ctype = BLR_BYTE;

switch (ctype)
    {
    case blr_gds_code:
	blr_format (control, "blr_gds_code, ");
	n = PRINT_BYTE;
	while (--n >= 0)
	    PRINT_CHAR;
	break;

    case blr_exception:
	blr_format (control, "blr_exception, ");
	n = PRINT_BYTE;
	while (--n >= 0)
	    PRINT_CHAR;
	break;

    case blr_sql_code:
	blr_format (control, "blr_sql_code, ");
	PRINT_WORD;
	break;

    case blr_default_code:
	blr_format (control, "blr_default_code, ");
	break;

    default:
	blr_error (control, "*** invalid condition type ***", NULL);
	break;
    }
return;
}

static blr_print_dtype (
    CTL	control)
{
/**************************************
 *
 *	b l r _ p r i n t _ d t y p e
 *
 **************************************
 *
 * Functional description
 *	Print a datatype sequence and return the length of the
 *	data described.
 *
 **************************************/
USHORT	dtype;
SCHAR	*string;
SSHORT	length;
UCHAR	v1, v2;

dtype = BLR_BYTE;

/* Special case blob (261) to keep down the size of the
   jump table */

switch (dtype)
    {
    case blr_short:
	string = "short";
	length = 2;
	break;

    case blr_long:
	string = "long";
	length = 4;
	break;

    case blr_int64:
	string = "int64";
	length = 8;
	break;

    case blr_quad:
	string = "quad";
	length = 8;
	break;
    
    case blr_timestamp:
	string = "timestamp";
	length = 8;
	break;
    
    case blr_sql_time:
	string = "sql_time";
	length = 4;
	break;
    
    case blr_sql_date:
	string = "sql_date";
	length = 4;
	break;
    
    case blr_float:
	string = "float";
	length = 4;
	break;
    
    case blr_double:
	string = "double";

	/* for double literal, return the length of the numeric string */

	v1 = *(control->ctl_blr);
	v2 = *(control->ctl_blr + 1);
	length = ((v2 << 8) | v1) + 2;
	break;
    
    case blr_d_float:
	string = "d_float";
	length = 8;
	break;
    
    case blr_text:
	string = "text";
	break;

    case blr_cstring:
	string = "cstring";
	break;

    case blr_varying:
	string = "varying";
	break;

    case blr_text2:
	string = "text2";
	break;

    case blr_cstring2:
	string = "cstring2";
	break;

    case blr_varying2:
	string = "varying2";
	break;

    default:
	blr_error (control, "*** invalid data type ***", NULL);
	break;
    }

blr_format (control, "blr_%s, ", string);

switch (dtype)
    {
    case blr_text:
	length = PRINT_WORD;
	break;

    case blr_varying:
	length = PRINT_WORD + 2;
	break;

    case blr_short:
    case blr_long:
    case blr_quad:
    case blr_int64:
	PRINT_BYTE;
	break;

    case blr_text2:
	PRINT_WORD;
	length = PRINT_WORD;
	break;

    case blr_varying2:
	PRINT_WORD;
	length = PRINT_WORD + 2;
	break;

    case blr_cstring2:
	PRINT_WORD;
	length = PRINT_WORD;
	break;

    default:
	if (dtype == blr_cstring)
	    length = PRINT_WORD;
	break;
    }

return length;
}

static void blr_print_join (
    CTL	control)
{
/**************************************
 *
 *	b l r _ p r i n t _ j o i n
 *
 **************************************
 *
 * Functional description
 *	Print a join type.
 *
 **************************************/
USHORT	join_type;
SCHAR	*string;

join_type = BLR_BYTE;

switch (join_type)
    {
    case blr_inner:
	string = "inner";
	break;

    case blr_left:
	string = "left";
	break;

    case blr_right:
	string = "right";
	break;

    case blr_full:
	string = "full";
	break;

    default:
	blr_error (control, "*** invalid join type ***", NULL);
	break;
    }

blr_format (control, "blr_%s, ", string);
}

static SLONG blr_print_line (
    CTL		control,
    SSHORT	offset)
{
/**************************************
 *
 *	b l r _ p r i n t _ l i n e
 *
 **************************************
 *
 * Functional description
 *	Invoke callback routine to print (or do something with) a line.
 *
 **************************************/

*control->ctl_ptr = 0;
(*control->ctl_routine) (control->ctl_user_arg, offset, control->ctl_buffer);
control->ctl_ptr = control->ctl_buffer;

return control->ctl_blr - control->ctl_blr_start;
}

static void blr_print_verb (
    CTL		control,
    SSHORT	level)
{
/**************************************
 *
 *	b l r _ p r i n t _ v e r b
 *
 **************************************
 *
 * Functional description
 *	Primary recursive routine to print BLR.
 *
 **************************************/
SSHORT	n, n2;
SLONG	offset;
UCHAR    operator;
UCHAR	*ops;

offset = control->ctl_blr - control->ctl_blr_start;
blr_indent (control, level);
operator = BLR_BYTE;

if ((SCHAR) operator == (SCHAR) blr_end)
    {
    blr_format (control, "blr_end, ");
    PRINT_LINE;
    return;
    }

blr_print_blr (control, operator);
level++;
ops = blr_table [operator].blr_operators;

while (*ops)
    switch (*ops++)
	{
	case op_verb:
	    PRINT_VERB;
	    break;

	case op_line:
	    offset = PRINT_LINE;
	    break;

	case op_byte:
	    n = PRINT_BYTE;
	    break;

	case op_word:
	    n = PRINT_WORD;
	    break;

	case op_pad:
	    PUT_BYTE (' ');
	    break;

	case op_dtype:
	    n = PRINT_DTYPE;
	    break;

	case op_literal:
	    while (--n >= 0)
		PRINT_CHAR;
	    break;

	case op_join:
	    PRINT_JOIN;
	    break;

	case op_message:
	    while (--n >= 0)
		{
		blr_indent (control, level);
		PRINT_DTYPE; 
		offset = PRINT_LINE;
		}
	    break;

	case op_parameters:
	    level++;
	    while (--n >= 0)
		PRINT_VERB;
	    level--;
	    break;

	case op_error_handler:
	    while (--n >= 0)
		{
		blr_indent (control, level);
		PRINT_COND; 
		offset = PRINT_LINE;
		}
	    break;

	case op_set_error:
	    PRINT_COND; 
	    break;

	case op_indent:
	    blr_indent (control, level);
	    break;

	case op_begin:
	    while ((SCHAR) *(control->ctl_blr) != (SCHAR) blr_end)
		PRINT_VERB;
	    break;

	case op_map:
	    while (--n >= 0)
	 	{
		blr_indent (control, level);
		PRINT_WORD;
		offset = PRINT_LINE;
		PRINT_VERB;
		}
	    break;

	case op_args:
	    while (--n >= 0)
		PRINT_VERB;
	    break;

	case op_literals:
	    while (--n >= 0)
		{
		blr_indent (control, level);
		n2 = PRINT_BYTE;
		while (--n2 >= 0)
		    PRINT_CHAR;
		offset = PRINT_LINE;
		}
	    break;

	case op_union:
	    while (--n >= 0)
		{
		PRINT_VERB; 
		PRINT_VERB;
		}
	    break;

	case op_relation:
	    operator = BLR_BYTE;
	    blr_print_blr (control, operator);
	    if (operator != blr_relation &&
		operator != blr_rid)
		blr_error (control, "*** blr_relation or blr_rid must be object of blr_lock_relation, %d found ***", (TEXT*) operator);

	    if (operator == blr_relation)
		{
		n = PRINT_BYTE;
		while (--n >= 0)
		    PRINT_CHAR;
		}
	    else	
	        PRINT_WORD;
	    break;

	default:
	    assert (FALSE);
	    break;
	}
}

static int blr_print_word (
    CTL	control)
{
/**************************************
 *
 *	b l r _ p r i n t _ w o r d
 *
 **************************************
 *
 * Functional description
 *	Print a VAX word as a numeric value an return same.
 *
 **************************************/
UCHAR	v1, v2;

v1 = BLR_BYTE;
v2 = BLR_BYTE;
blr_format (control,
   (control->ctl_language) ? "chr(%d),chr(%d), " : "%d,%d, ", (TEXT*) v1, (TEXT*) v2);

return (v2 << 8) | v1;
}

#ifdef OS2_ONLY
static void APIENTRY gds__cleanup (
    ULONG	code)

#else

void gds__cleanup (void)
#endif
{
/**************************************
 *
 *	c l e a n u p
 *
 **************************************
 *
 * Functional description
 *	Exit handler for image exit.
 *
 **************************************/
CLEAN		clean;
FPTR_VOID	routine;
void		*arg;

#ifdef UNIX
if (gds_pid != getpid()) return;
#endif

gds__msg_close (NULL);

while (clean = cleanup_handlers)
    {
    cleanup_handlers = clean->clean_next;
    routine = clean->clean_routine;
    arg = clean->clean_arg;

    /* We must free the handler before calling it because there
       may be a handler (and is) that frees all memory that has
       been allocated. */

    FREE_LIB_MEMORY (clean);
    (*routine) (arg);
    }

V4_MUTEX_DESTROY (&alloc_mutex);
/* V4_DESTROY; */
initialized = 0;

#ifdef OS2_ONLY
DosExitList (EXLST_EXIT, cleanup);
#endif
}

static void cleanup_malloced_memory (
    void	*arg)
{
/**************************************
 *
 *	c l e a n u p _ m a l l o c e d _ m e m o r y
 *
 **************************************
 *
 * Functional description
 *	Return the real memory that was acquired in gds__alloc
 *	to the system.  Refer to gds__alloc for the cases when
 *	the malloc pointer is saved and when the malloc pointer
 *	is not saved.
 *	This substantive work of this function is only turned on
 *	for Novell.
 *
 **************************************/
void	*ptr;

#ifdef NETWARE_386
#ifdef DEBUG_GDS_ALLOC
gds_alloc_report (ALLOC_verbose, __FILE__, __LINE__);
#endif
#endif

#if (defined (NETWARE_386) || defined (WINDOWS_ONLY) || defined (WIN_NT) || defined (UNIX))
while (ptr = malloced_memory)
    {
    malloced_memory = *malloced_memory;
    free (ptr);
    }
#endif

pool = NULL;

#if (defined SOLARIS && defined SUPERSERVER)

/* Close the file used as the basis for generating
   anonymous virtual memory. Since it is created
   pre-deleted there's no need to unlink it here. */

close (anon_fd);
anon_fd = -1;
#endif
}

static ULONG free_memory (
    void	*blk)
{
/**************************************
 *
 *	f r e e _ m e m o r y
 *
 **************************************
 *
 * Functional description
 *	Release a block allocated by gds__alloc.  Return number
 *	of bytes released.
 *
 **************************************/
register FREE HUGE_PTR	*ptr, prior, free_blk, block;
SLONG			length;

#ifdef DEV_BUILD
if (!blk)
    {
    DEV_REPORT ("gds__free: Tried to free NULL\n");
    BREAKPOINT (__LINE__);
    return 0;
    }
#endif

block = (FREE) ((UCHAR*) blk - ALLOC_HEADER_SIZE);
blk = block;

#ifdef DEV_BUILD
gds_delta_alloc -= *((ULONG *)blk);
#endif
length = block->free_length = *((ULONG *)blk);

#ifdef DEBUG_GDS_ALLOC
block->free_count = gds_free_call_count;
if (gds_bug_free_count && (gds_free_call_count == gds_bug_free_count))
    {
    PRINTF ("gds__free call %d reached, address=%lx\n", gds_bug_free_count, blk);
    BREAKPOINT (__LINE__);
    }
#endif

#ifdef DEV_BUILD
if (length <= ALLOC_OVERHEAD)
    {
    DEV_REPORT ("gds__free: attempt to release bad block (too small)\n");    /* TXNN */
    BREAKPOINT (__LINE__);
    return 0;
    }
#endif

#ifdef NETWARE_386
if (regular_malloc)
    {
    free (blk);
    return length - ALLOC_OVERHEAD;
    }
#endif
         
#ifdef WINDOWS_ONLY
{
free (blk);
return length - ALLOC_OVERHEAD;
}
#else
#ifdef DOS_ONLY
if (InProtectMode()) 
    {
    free (blk);
    return length - ALLOC_OVERHEAD;
    }
#endif  /* DOS_ONLY */

/* Find the logical place in the free block list for the returning
   block.  If it adjoins existing blocks, merge it (them). */

prior = NULL;

for (ptr = &pool; (free_blk = *ptr) != NULL; prior = free_blk, ptr = &free_blk->free_next)
    {
    if (free_blk->free_next && (UCHAR HUGE_PTR*) free_blk >= (UCHAR HUGE_PTR*) free_blk->free_next)
	{
	DEV_REPORT ("gds__free: pool corrupted");	/* TXNN */
	BREAKPOINT (__LINE__);
	free_blk = *ptr = NULL;
	break;
	}
    if ((UCHAR HUGE_PTR*) block < (UCHAR HUGE_PTR*) free_blk)
	break;
    }

/* Make sure everything is completely kosher */

if (block->free_length <= 0 ||
    (prior && (UCHAR HUGE_PTR*) block < (UCHAR HUGE_PTR*) prior + prior->free_length) ||
    (free_blk && (UCHAR HUGE_PTR*) block + block->free_length > (UCHAR HUGE_PTR*) free_blk))
    {
    DEV_REPORT ("gds__free: attempt to release bad block\n");    /* TXNN */
    BREAKPOINT (__LINE__);
    return 0;
    }

/* Start by linking block into chain */

block->free_next = free_blk;
*ptr = block;

/* Try to merge the free block with the next block */

if (free_blk && (UCHAR HUGE_PTR*) block + block->free_length == (UCHAR HUGE_PTR*) free_blk)
    {
    block->free_length += free_blk->free_length;
    block->free_next = free_blk->free_next;
#ifdef DEBUG_GDS_ALLOC
    /* We're making a larger free chuck - be sure to color the header
     * of the "merged" block as freed space 
     */
    memset (free_blk, ALLOC_FREED_PATTERN, sizeof (struct free));
#endif
    }

/* Next, try to merge the free block with the prior block */

if (prior && (UCHAR HUGE_PTR*) prior + prior->free_length == (UCHAR HUGE_PTR*) block)
    {
    prior->free_length += block->free_length;
    prior->free_next = block->free_next;
#ifdef DEBUG_GDS_ALLOC
    /* We're making a larger free chuck - be sure to color the header
     * of the "merged" block as freed space 
     */
    memset (block, ALLOC_FREED_PATTERN, sizeof (struct free));
#endif
    }

#ifdef DEBUG_GDS_ALLOC
if (gds_bug_free_count && (gds_free_call_count >= gds_bug_free_count))
    gds_alloc_report (ALLOC_silent, __FILE__, __LINE__);
#endif

/* The number of bytes freed from the user's point
   of view - eg: the block length, less the malloc overhead */

if ( length%(1024+ALLOC_OVERHEAD) == 0 )
   return (length - ALLOC_OVERHEAD*(length/(1024+ALLOC_OVERHEAD)));
else
    return length - ALLOC_OVERHEAD;
#endif  /* WINDOWS_ONLY */
}

static void init (void)
{
/**************************************
 *
 *	i n i t
 *
 **************************************
 *
 * Functional description
 *	Do anything necessary to initialize module/system.
 *
 **************************************/
STATUS	status;

if (initialized)
    return;

#ifdef DEBUG_GDS_ALLOC
{
UCHAR	*t;
IB_FILE	*file;
if ((t = (UCHAR *) getenv ("DEBUG_GDS_ALLOC")) != NULL)
    {
    /* Set which call to gds__alloc starts full memory debugging */
    if (*t >= '0' && *t <= '9')
	gds_alloc_state = atoi (t);
    else
	gds_alloc_state = 0;
    }
if ((t = (UCHAR *) getenv ("DEBUG_GDS_ALLOC_NOMEM")) != NULL)
    {
    /* Set which call to gds__alloc "exhausts" available memory */
    if (*t >= '0' && *t <= '9')
	gds_alloc_nomem = atoi (t);
    else
	gds_alloc_nomem = 0;
    }
/* Open the memory debugging configuration file, if any. */

if (file = ib_fopen (DBG_MEM_FILE, "r"))
    {
    ib_fscanf (file, "%ld", &gds_bug_alloc_count);
    ib_fscanf (file, "%ld", &gds_bug_free_count);
    PRINTF ("gds_bug_alloc_count=%d, gds_bug_free_count=%d\n",
           gds_bug_alloc_count, gds_bug_free_count);
    ib_fclose (file);
    }
}
BREAKPOINT (__LINE__);
#endif DEBUG_GDS_ALLOC

/* V4_INIT; */
/* V4_GLOBAL_MUTEX_LOCK; */
if (initialized)
    {
/*  V4_GLOBAL_MUTEX_UNLOCK; */
    return;
    }

V4_MUTEX_INIT (&alloc_mutex);

#ifdef APOLLO
pfm_$static_cleanup (cleanup, status);
#endif

#ifdef OS2_ONLY
DosExitList (EXLST_ADD | 0x0000FF00, cleanup);
#endif

#ifdef VMS
exit_description.exit_handler = cleanup;
exit_description.args = 1;
exit_description.arg [0] = &exit_status;
status = sys$dclexh (&exit_description);
#endif
 
#ifdef UNIX
gds_pid = getpid();
#ifdef SUPERSERVER
#if (defined SOLARIS || defined HP10 || defined LINUX)
{
/* Increase max open files to hard limit for Unix
   platforms which are known to have low soft limits. */

struct rlimit	old, new;

if (!getrlimit (RLIMIT_NOFILE, &old) &&
    old.rlim_cur < old.rlim_max)
    {
    new.rlim_cur = new.rlim_max = old.rlim_max;
    if (!setrlimit (RLIMIT_NOFILE, &new))
	gds__log ("Open file limit increased from %d to %d",
		  old.rlim_cur, new.rlim_cur);
    }
}
#endif
#endif /* SUPERSERVER */
#endif /* UNIX */

#ifdef ATEXIT
ATEXIT (gds__cleanup);
#endif

initialized = 1;

gds__register_cleanup (cleanup_malloced_memory, NULL_PTR);
#ifndef REQUESTER
ISC_signal_init();
#endif

/* V4_GLOBAL_MUTEX_UNLOCK; */
}

static int yday (
    struct tm	*times)
{
/**************************************
 *
 *	y d a y
 *
 **************************************
 *
 * Functional description
 *	Convert a calendar date to the day-of-year.
 *
 *	The unix time structure considers
 *	january 1 to be Year day 0, although it
 *	is day 1 of the month.   (Note that QLI,
 *	when printing Year days takes the other
 *	view.)   
 *
 **************************************/
SSHORT	day, month, year;
CONST BYTE	*days;
static CONST BYTE	month_days [] = {31,28,31,30,31,30,31,31,30,31,30,31};

day = times->tm_mday;
month = times->tm_mon;
year = times->tm_year + 1900;

--day;

for (days = month_days; days < month_days + month; days++)
    day += *days;

if (month < 2)
    return day;

/* Add a day as we're past the leap-day */
if (! (year % 4))
    day++;

/* Ooops - year's divisible by 100 aren't leap years */
if (! (year % 100))
    day--;

/* Unless they are also divisible by 400! */
if (! (year % 400))
    day++;

return day;
}

#if (defined SOLARIS && defined SUPERSERVER)
static UCHAR *mmap_anon (
    SLONG	size)
{
/**************************************
 *
 *	m m a p _ a n o n			(S O L A R I S)
 *
 **************************************
 *
 * Functional description
 *	Allocate anonymous virtual memory for Solaris.
 *	Use an arbitrary file descriptor as a basis
 *	for file mapping.
 *
 **************************************/
UCHAR	*memory, *va, *va_end, *va1;
ULONG	chunk, errno1;

/* Choose SYS_ALLOC_CHUNK such that it is always valid for the
   underlying mapped file and is a multiple of any conceivable
   memory page size that a hardware platform might support. */

#define SYS_ALLOC_CHUNK	0x20000	/* 131,072 bytes */

/* All virtual addresses generated will be remapped or wrapped around
   to the start of the mapped file. The mapped file must be at least
   SYS_ALLOC_CHUCK large to guarantee valid memory references. */

if (anon_fd == -1)
    {
    anon_fd = (int) gds__temp_file (FALSE, "gds_anon", NULL);
    while (ftruncate (anon_fd, SYS_ALLOC_CHUNK) == -1)
	if (!SYSCALL_INTERRUPTED(errno))
	    {
	    close (anon_fd);
	    anon_fd = -1;
	    ERR_post (isc_sys_request, isc_arg_string, "ftruncate", isc_arg_unix, errno, 0);
	    }
    }

/* Get memory page size for virtual memory checking. */

if (page_size == -1 && (page_size = sysconf (_SC_PAGESIZE)) == -1)
    ERR_post (isc_sys_request, isc_arg_string, "sysconf", isc_arg_unix, errno, 0);

/* Generate a virtually contiguous address space for size requested.
   MAP_PRIVATE is critical here, otherwise the mapped file turns to jelly. */

memory = mmap (0, size, (PROT_READ | PROT_WRITE), MAP_PRIVATE, anon_fd, 0);
if (memory == MAP_FAILED)
    {
    /* If no swap space or memory available maybe caller can ask for less.
       Punt on unexpected errors. */

    if (errno == EAGAIN || errno == ENOMEM)
    	return NULL;
    else
	ERR_post (isc_sys_request, isc_arg_string, "mmap", isc_arg_unix, errno, 0);
    }

/* Memory returned from mmap should always be page-aligned. */

assert (((ULONG) memory & (page_size - 1)) == 0);

/* Some of this virtual address space may be invalid because the
   mapped file isn't large enough, remap all virtual addresses to
   the beginning of the file which will validate the illegal memory
   references. The use of MAP_FIXED guarantees that the virtual
   address space remains contiguous during this legerdemain. */

va = memory;
va_end = memory + size;

for (va += SYS_ALLOC_CHUNK; va < va_end; va += chunk) 
    {
    chunk = MIN (va_end - va, SYS_ALLOC_CHUNK);
    va1 = mmap (va, chunk, (PROT_READ | PROT_WRITE),
		(MAP_PRIVATE | MAP_FIXED), anon_fd, 0);
    if (va1 != va)
	{
	assert (FALSE);
	errno1 = errno;
	munmap (memory, size);
	ERR_post (isc_sys_request, isc_arg_string, "mmap", isc_arg_unix, errno1, 0);
	}
    }

/* All modfiy references will transparently revector from the mapped
   file to private memory backed by the swap file. However, since the
   database buffer pool uses this allocation mechanism, make the
   revectoring happen now by touching each memory page. This protects
   against DMA (Direct Memory Access) transfers used by "raw" I/O
   device interfaces which may bypass the virtual memory manager. */

for (va = memory; va < va_end; va += page_size) 
    *(ULONG*) va = (ULONG) va;

#ifdef DEV_BUILD
/* Since the mapped object is mapped repeatedly by different
   chunks of the virtual address space, check the same physical
   memory pages aren't being shared by labeling each virtual page
   with its address. Verify every page is still labeled correctly .*/ 

for (va = memory; va < va_end; va += page_size) 
    assert (*(ULONG*) va == (ULONG) va);
#endif

return memory;
}
#endif

static void ndate (
    SLONG	nday,
    struct tm	*times)
{
/**************************************
 *
 *	n d a t e
 *
 **************************************
 *
 * Functional description
 *	Convert a numeric day to [day, month, year].
 *
 * Calenders are divided into 4 year cycles.
 * 3 Non-Leap years, and 1 leap year.
 * Each cycle takes 365*4 + 1 == 1461 days.
 * There is a further cycle of 100 4 year cycles.
 * Every 100 years, the normally expected leap year
 * is not present.  Every 400 years it is.
 * This cycle takes 100 * 1461 - 3 == 146097 days
 * The origin of the constant 2400001 is unknown.
 * The origin of the constant 1721119 is unknown.
 * The difference between 2400001 and 1721119 is the
 * number of days From 0/0/0000 to our base date of
 * 11/xx/1858. (678882)
 * The origin of the constant 153 is unknown.
 *
 * This whole routine has problems with ndates
 * less than -678882 (Approx 2/1/0000).
 *
 **************************************/
SLONG	year, month, day;
SLONG	century;

nday -= 1721119 - 2400001;
century = (4 * nday - 1) / 146097;
nday = 4 * nday - 1 - 146097 * century;
day = nday / 4;

nday = (4 * day + 3) / 1461;
day = 4 * day + 3 - 1461 * nday;
day = (day + 4) / 4;

month = (5 * day - 3) / 153;
day = 5 * day - 3 - 153 * month;
day = (day + 5) / 5;

year = 100 * century + nday;

if (month < 10)
    month += 3;
else
    {
    month -= 9;
    year += 1;
    }

times->tm_mday = (int) day;
times->tm_mon = (int) month - 1;
times->tm_year = (int) year - 1900;
}

static GDS_DATE nday (
    struct tm	*times)
{
/**************************************
 *
 *	n d a y
 *
 **************************************
 *
 * Functional description
 *	Convert a calendar date to a numeric day
 *	(the number of days since the base date).
 *
 **************************************/
SSHORT	day, month, year;
SLONG	c, ya;

day = times->tm_mday;
month = times->tm_mon + 1;
year = times->tm_year + 1900;

if (month > 2)
    month -= 3;
else
    {
    month += 9;
    year -= 1;
    }

c = year / 100;
ya = year - 100 * c;

return (GDS_DATE) (((SINT64) 146097 * c) / 4 + 
    (1461 * ya) / 4 + 
    (153 * month + 2) / 5 + 
    day + 1721119 - 2400001);
}

static void sanitize (
    TEXT        *locale)
{
/**************************************
 *
 *	s a n i t i z e
 *
 **************************************
 *
 * Functional description
 *	Clean up a locale to make it acceptable for use in file names
 *      for Windows NT, PC, and mpexl: remove any '.' or '_' for mpexl,
 *	replace any period with '_' for NT or PC.
 *
 **************************************/
TEXT	ch, *p;

while (*locale)
    {
    ch = *locale;
#ifdef mpexl
    if (ch == '.' || ch == '_')
	{
	for (p = locale; *p = *++p;)
	    ;
	}
#else
    if (ch == '.')
        *locale = '_';
    locale++;
#endif
    }
}

#ifdef DEBUG_GDS_ALLOC
#undef gds__alloc

UCHAR * API_ROUTINE gds__alloc (
    SLONG	size)
{
/**************************************
 *
 *	g d s _ $ a l l o c     (alternate debug entrypoint)
 *
 **************************************
 *
 * Functional description
 *
 *	NOTE: This function should be the last in the file due to
 *	the undef of gds__alloc above.
 *
 *	For modules not recompiled with DEBUG_GDS_ALLOC, this provides
 *	an entry point to malloc again.
 *
 **************************************/
return gds__alloc_debug (size, "-- Unknown --", 0);
}
#endif

#ifdef SUPERSERVER
static void freemap(int cod)
{
FREE	*next, free_blk;
ULONG   free_list_count=0;
ULONG   total_free=0;

ib_printf("\nFree Start\t\tFree Length\t\tFree End\n");
for (next = &pool; (free_blk = *next) != NULL; next = &(*next)->free_next)
    {
    ++free_list_count;
    total_free += free_blk->free_length;
    ib_printf("%x\t\t\t%d\t\t\t%x\n", (UCHAR HUGE_PTR*) free_blk,
			free_blk->free_length,
			(UCHAR HUGE_PTR*)(free_blk + free_blk->free_length));
    }
    ib_printf("Stats after %s\n", (cod) ? "Merging":"Allocating");
    ib_printf("Free list count    = %d\n", free_list_count);
    ib_printf("Total Free         = %d\n", total_free);
    gds_print_delta_counters(ib_stdout);
}

void gds_print_delta_counters(IB_FILE *fptr)
{
#ifdef DEV_BUILD
    ib_fprintf(fptr, "gds_delta_alloc    = %d\n", gds_delta_alloc);
    ib_fprintf(fptr, "gds_max_alloc      = %d\n", gds_max_alloc);
#endif
    ib_fprintf(fptr, "allr_delta_alloc   = %d\n", allr_delta_alloc);
    ib_fprintf(fptr, "alld_delta_alloc   = %d\n", alld_delta_alloc);
    ib_fprintf(fptr, "all_delta_alloc    = %d\n", all_delta_alloc);
}
#endif


