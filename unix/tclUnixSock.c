/*
 * tclUnixSock.c --
 *
 *	This file contains Unix-specific socket related code.
 *
 * Copyright © 1995 Sun Microsystems, Inc.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tclInt.h"
#include <netinet/tcp.h>

/*
 * Helper macros to make parts of this file clearer. The macros do exactly
 * what they say on the tin. :-) They also only ever refer to their arguments
 * once, and so can be used without regard to side effects.
 */

#define SET_BITS(var, bits)	((var) |= (bits))
#define CLEAR_BITS(var, bits)	((var) &= ~(bits))
#define GOT_BITS(var, bits)     (((var) & (bits)) != 0)

/* "sock" + a pointer in hex + \0 */
#define SOCK_CHAN_LENGTH        (4 + sizeof(void *) * 2 + 1)
#define SOCK_TEMPLATE           "sock%" TCL_Z_MODIFIER "x"

#undef SOCKET   /* Possible conflict with win32 SOCKET */

/*
 * This is needed to comply with the strict aliasing rules of GCC, but it also
 * simplifies casting between the different sockaddr types.
 */

typedef union {
    struct sockaddr sa;
    struct sockaddr_in sa4;
    struct sockaddr_in6 sa6;
    struct sockaddr_storage sas;
} address;

/*
 * This structure describes per-instance state of a tcp-based channel.
 */

typedef struct TcpState TcpState;

typedef struct TcpFdList {
    TcpState *statePtr;
    int fd;
    struct TcpFdList *next;
} TcpFdList;

struct TcpState {
    Tcl_Channel channel;	/* Channel associated with this file. */
    int flags;			/* OR'ed combination of the bitfields defined
				 * below. */
    TcpFdList fds;		/* The file descriptors of the sockets. */
    int interest;		/* Event types of interest */

    /*
     * Only needed for server sockets
     */

    Tcl_TcpAcceptProc *acceptProc; /* Proc to call on accept. */
    void *acceptProcData;	/* The data for the accept proc. */

    /*
     * Only needed for client sockets
     */

    struct addrinfo *addrlist;	/* Addresses to connect to. */
    struct addrinfo *addr;	/* Iterator over addrlist. */
    struct addrinfo *myaddrlist;/* Local address. */
    struct addrinfo *myaddr;	/* Iterator over myaddrlist. */
    int filehandlers;		/* Caches FileHandlers that get set up while
				 * an async socket is not yet connected. */
    int connectError;		/* Cache SO_ERROR of async socket. */
    int cachedBlocking;		/* Cache blocking mode of async socket. */
};

/*
 * These bits may be OR'ed together into the "flags" field of a TcpState
 * structure.
 */

#define TCP_NONBLOCKING		(1<<0)	/* Socket with non-blocking I/O */
#define TCP_ASYNC_CONNECT	(1<<1)	/* Async connect in progress. */
#define TCP_ASYNC_PENDING	(1<<4)	/* TcpConnect was called to
					 * process an async connect. This
					 * flag indicates that reentry is
					 * still pending */
#define TCP_ASYNC_FAILED	(1<<5)	/* An async connect finally failed */

#define TCP_ASYNC_TEST_MODE	(1<<8)	/* Async testing activated.  Do not
					 * automatically continue connection
					 * process. */

/*
 * The following defines the maximum length of the listen queue. This is the
 * number of outstanding yet-to-be-serviced requests for a connection on a
 * server socket, more than this number of outstanding requests and the
 * connection request will fail.
 */

#ifndef SOMAXCONN
#   define SOMAXCONN	100
#elif (SOMAXCONN < 100)
#   undef  SOMAXCONN
#   define SOMAXCONN	100
#endif /* SOMAXCONN < 100 */

/*
 * The following defines how much buffer space the kernel should maintain for
 * a socket.
 */

#define SOCKET_BUFSIZE	4096

/*
 * Static routines for this file:
 */

static void		TcpAsyncCallback(void *clientData, int mask);
static int		TcpConnect(Tcl_Interp *interp, TcpState *state);
static void		TcpAccept(void *data, int mask);
static int		TcpBlockModeProc(void *data, int mode);
static int		TcpCloseProc(void *instanceData,
			    Tcl_Interp *interp);
static int		TcpClose2Proc(void *instanceData,
			    Tcl_Interp *interp, int flags);
static int		TcpGetHandleProc(void *instanceData,
			    int direction, void **handlePtr);
static int		TcpGetOptionProc(void *instanceData,
			    Tcl_Interp *interp, const char *optionName,
			    Tcl_DString *dsPtr);
static int		TcpInputProc(void *instanceData, char *buf,
			    int toRead, int *errorCode);
static int		TcpOutputProc(void *instanceData,
			    const char *buf, int toWrite, int *errorCode);
static int		TcpSetOptionProc(void *instanceData,
			    Tcl_Interp *interp, const char *optionName,
			    const char *value);
static void		TcpThreadActionProc(void *instanceData, int action);
static void		TcpWatchProc(void *instanceData, int mask);
static int		WaitForConnect(TcpState *statePtr, int *errorCodePtr);
static Tcl_FileProc	WrapNotify;

/*
 * This structure describes the channel type structure for TCP socket
 * based IO:
 */

static const Tcl_ChannelType tcpChannelType = {
    "tcp",
    TCL_CHANNEL_VERSION_5,
    NULL,			/* Deprecated. */
    TcpInputProc,
    TcpOutputProc,
    NULL,			/* Deprecated. */
    TcpSetOptionProc,
    TcpGetOptionProc,
    TcpWatchProc,
    TcpGetHandleProc,
    TcpClose2Proc,
    TcpBlockModeProc,
    NULL,			/* Flush proc. */
    NULL,			/* Bubbled event handler proc. */
    NULL,			/* Seek proc. */
    TcpThreadActionProc,
    NULL			/* Truncate proc. */
};

/*
 * The following variable holds the network name of this host.
 */

static TclInitProcessGlobalValueProc InitializeHostName;
static ProcessGlobalValue hostName =
	{0, 0, NULL, NULL, InitializeHostName, NULL, NULL};

/*
 * ----------------------------------------------------------------------
 *
 * InitializeHostName --
 *
 *	This routine sets the process global value of the name of the local
 *	host on which the process is running.
 *
 * Results:
 *	None.
 *
 * ----------------------------------------------------------------------
 */

static void
InitializeHostName(
    char **valuePtr,
    size_t *lengthPtr,
    Tcl_Encoding *encodingPtr)
{
    const char *native = NULL;

#ifndef NO_UNAME
    struct utsname u;
    struct hostent *hp;

    memset(&u, (int) 0, sizeof(struct utsname));
    if (uname(&u) >= 0) {				/* INTL: Native. */
	hp = TclpGetHostByName(u.nodename);		/* INTL: Native. */
	if (hp == NULL) {
	    /*
	     * Sometimes the nodename is fully qualified, but gets truncated
	     * as it exceeds SYS_NMLN. See if we can just get the immediate
	     * nodename and get a proper answer that way.
	     */

	    char *dot = strchr(u.nodename, '.');

	    if (dot != NULL) {
		char *node = (char *)Tcl_Alloc(dot - u.nodename + 1);

		memcpy(node, u.nodename, dot - u.nodename);
		node[dot - u.nodename] = '\0';
		hp = TclpGetHostByName(node);
		Tcl_Free(node);
	    }
	}
	if (hp != NULL) {
	    native = hp->h_name;
	} else {
	    native = u.nodename;
	}
    }
#else /* !NO_UNAME */
    /*
     * Uname doesn't exist; try gethostname instead.
     *
     * There is no portable macro for the maximum length of host names
     * returned by gethostbyname(). We should only trust SYS_NMLN if it is at
     * least 255 + 1 bytes to comply with DNS host name limits.
     *
     * Note: SYS_NMLN is a restriction on "uname" not on gethostbyname!
     *
     * For example HP-UX 10.20 has SYS_NMLN == 9, while gethostbyname() can
     * return a fully qualified name from DNS of up to 255 bytes.
     *
     * Fix suggested by Viktor Dukhovni (viktor@esm.com)
     */

#    if defined(SYS_NMLN) && (SYS_NMLEN >= 256)
    char buffer[SYS_NMLEN];
#    else
    char buffer[256];
#    endif

    if (gethostname(buffer, sizeof(buffer)) >= 0) {	/* INTL: Native. */
	native = buffer;
    }
#endif /* NO_UNAME */

    *encodingPtr = Tcl_GetEncoding(NULL, NULL);
    if (native) {
	*lengthPtr = strlen(native);
	*valuePtr = (char *)Tcl_Alloc(*lengthPtr + 1);
	memcpy(*valuePtr, native, *lengthPtr + 1);
    } else {
	*lengthPtr = 0;
	*valuePtr = (char *)Tcl_Alloc(1);
	*valuePtr[0] = '\0';
    }
}

/*
 * ----------------------------------------------------------------------
 *
 * Tcl_GetHostName --
 *
 *	Returns the name of the local host.
 *
 * Results:
 *	A string containing the network name for this machine, or an empty
 *	string if we can't figure out the name. The caller must not modify or
 *	free this string.
 *
 * Side effects:
 *	Caches the name to return for future calls.
 *
 * ----------------------------------------------------------------------
 */

const char *
Tcl_GetHostName(void)
{
    Tcl_Obj *tclObj = TclGetProcessGlobalValue(&hostName);
    return TclGetString(tclObj);
}

/*
 * ----------------------------------------------------------------------
 *
 * TclpFinalizeSockets --
 *
 *	Performs per-thread socket subsystem finalization.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 * ----------------------------------------------------------------------
 */

void
TclpFinalizeSockets(void)
{
    return;
}

/*
 * ----------------------------------------------------------------------
 *
 * TcpBlockModeProc --
 *
 *	This function is invoked by the generic IO level to set blocking and
 *	nonblocking mode on a TCP socket based channel.
 *
 * Results:
 *	0 if successful, errno when failed.
 *
 * Side effects:
 *	Sets the device into blocking or nonblocking mode.
 *
 * ----------------------------------------------------------------------
 */

static int
TcpBlockModeProc(
    void *instanceData,		/* Socket state. */
    int mode)			/* The mode to set. Can be one of
				 * TCL_MODE_BLOCKING or
				 * TCL_MODE_NONBLOCKING. */
{
    TcpState *statePtr = (TcpState *)instanceData;

    if (mode == TCL_MODE_BLOCKING) {
	CLEAR_BITS(statePtr->flags, TCP_NONBLOCKING);
    } else {
	SET_BITS(statePtr->flags, TCP_NONBLOCKING);
    }
    if (GOT_BITS(statePtr->flags, TCP_ASYNC_CONNECT)) {
	statePtr->cachedBlocking = mode;
	return 0;
    }
    if (TclUnixSetBlockingMode(statePtr->fds.fd, mode) < 0) {
	return errno;
    }
    return 0;
}

/*
 * ----------------------------------------------------------------------
 *
 * WaitForConnect --
 *
 *	Check the state of an async connect process. If a connection attempt
 *	terminated, process it, which may finalize it or may start the next
 *	attempt. If a connect error occurs, it is saved in
 *	statePtr->connectError to be reported by 'fconfigure -error'.
 *
 *	There are two modes of operation, defined by errorCodePtr:
 *	 *  non-NULL: Called by explicit read/write command. Blocks if the
 *	    socket is blocking.
 *	    May return two error codes:
 *	     *	EWOULDBLOCK: if connect is still in progress
 *	     *	ENOTCONN: if connect failed. This would be the error message
 *		of a recv or sendto syscall so this is emulated here.
 *	 *  NULL: Called by a background operation. Do not block and do not
 *	    return any error code.
 *
 * Results:
 *	0 if the connection has completed, -1 if still in progress or there is
 *	an error.
 *
 * Side effects:
 *	Processes socket events off the system queue. May process
 *	asynchronous connects.
 *
 *----------------------------------------------------------------------
 */

static int
WaitForConnect(
    TcpState *statePtr,		/* State of the socket. */
    int *errorCodePtr)
{
    int timeout;

    /*
     * Check if an async connect failed already and error reporting is
     * demanded, return the error ENOTCONN
     */

    if (errorCodePtr != NULL && GOT_BITS(statePtr->flags, TCP_ASYNC_FAILED)) {
	*errorCodePtr = ENOTCONN;
	return -1;
    }

    /*
     * Check if an async connect is running. If not return ok.
     */

    if (!GOT_BITS(statePtr->flags, TCP_ASYNC_PENDING)) {
	return 0;
    }

    /*
     * In socket test mode do not continue with the connect.
     * Exceptions are:
     * - Call by recv/send and blocking socket
     *   (errorCodePtr != NULL && !GOT_BITS(flags, TCP_NONBLOCKING))
     */

    if (GOT_BITS(statePtr->flags, TCP_ASYNC_TEST_MODE)
	    && !(errorCodePtr != NULL
		    && !GOT_BITS(statePtr->flags, TCP_NONBLOCKING))) {
	*errorCodePtr = EWOULDBLOCK;
	return -1;
    }

    if (errorCodePtr == NULL || GOT_BITS(statePtr->flags, TCP_NONBLOCKING)) {
	timeout = 0;
    } else {
	timeout = -1;
    }
    do {
	if (TclUnixWaitForFile(statePtr->fds.fd,
		TCL_WRITABLE | TCL_EXCEPTION, timeout) != 0) {
	    TcpConnect(NULL, statePtr);
	}

	/*
	 * Do this only once in the nonblocking case and repeat it until the
	 * socket is final when blocking.
	 */
    } while (timeout == -1 && GOT_BITS(statePtr->flags, TCP_ASYNC_CONNECT));

    if (errorCodePtr != NULL) {
	if (GOT_BITS(statePtr->flags, TCP_ASYNC_PENDING)) {
	    *errorCodePtr = EAGAIN;
	    return -1;
	} else if (statePtr->connectError != 0) {
	    *errorCodePtr = ENOTCONN;
	    return -1;
	}
    }
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * TcpInputProc --
 *
 *	This function is invoked by the generic IO level to read input from a
 *	TCP socket based channel.
 *
 *	NOTE: We cannot share code with FilePipeInputProc because here we must
 *	use recv to obtain the input from the channel, not read.
 *
 * Results:
 *	The number of bytes read is returned or -1 on error. An output
 *	argument contains the POSIX error code on error, or zero if no error
 *	occurred.
 *
 * Side effects:
 *	Reads input from the input device of the channel.
 *
 *----------------------------------------------------------------------
 */

static int
TcpInputProc(
    void *instanceData,		/* Socket state. */
    char *buf,			/* Where to store data read. */
    int bufSize,		/* How much space is available in the
				 * buffer? */
    int *errorCodePtr)		/* Where to store error code. */
{
    TcpState *statePtr = (TcpState *)instanceData;
    ssize_t bytesRead;

    *errorCodePtr = 0;
    if (WaitForConnect(statePtr, errorCodePtr) != 0) {
	return -1;
    }
    bytesRead = recv(statePtr->fds.fd, buf, bufSize, 0);
    if (bytesRead >= 0) {
	return (int)bytesRead;
    }
    if (errno == ECONNRESET) {
	/*
	 * Turn ECONNRESET into a soft EOF condition.
	 */

	return 0;
    }
    *errorCodePtr = errno;
    return -1;
}

/*
 *----------------------------------------------------------------------
 *
 * TcpOutputProc --
 *
 *	This function is invoked by the generic IO level to write output to a
 *	TCP socket based channel.
 *
 *	NOTE: We cannot share code with FilePipeOutputProc because here we
 *	must use send, not write, to get reliable error reporting.
 *
 * Results:
 *	The number of bytes written is returned. An output argument is set to
 *	a POSIX error code if an error occurred, or zero.
 *
 * Side effects:
 *	Writes output on the output device of the channel.
 *
 *----------------------------------------------------------------------
 */

static int
TcpOutputProc(
    void *instanceData,		/* Socket state. */
    const char *buf,		/* The data buffer. */
    int toWrite,		/* How many bytes to write? */
    int *errorCodePtr)		/* Where to store error code. */
{
    TcpState *statePtr = (TcpState *)instanceData;
    ssize_t written;

    *errorCodePtr = 0;
    if (WaitForConnect(statePtr, errorCodePtr) != 0) {
	return -1;
    }
    written = send(statePtr->fds.fd, buf, toWrite, 0);

    if (written >= 0) {
	return (int)written;
    }
    *errorCodePtr = errno;
    return -1;
}

/*
 *----------------------------------------------------------------------
 *
 * TcpCloseProc --
 *
 *	This function is invoked by the generic IO level to perform
 *	channel-type-specific cleanup when a TCP socket based channel is
 *	closed.
 *
 * Results:
 *	0 if successful, the value of errno if failed.
 *
 * Side effects:
 *	Closes the socket of the channel.
 *
 *----------------------------------------------------------------------
 */

static int
TcpCloseProc(
    void *instanceData,		/* The socket to close. */
    TCL_UNUSED(Tcl_Interp *))
{
    TcpState *statePtr = (TcpState *)instanceData;
    int errorCode = 0;
    TcpFdList *fds;

    /*
     * Delete a file handler that may be active for this socket if this is a
     * server socket - the file handler was created automatically by Tcl as
     * part of the mechanism to accept new client connections. Channel
     * handlers are already deleted in the generic IO channel closing code
     * that called this function, so we do not have to delete them here.
     */

    for (fds = &statePtr->fds; fds != NULL; fds = fds->next) {
	if (fds->fd < 0) {
	    continue;
	}
	Tcl_DeleteFileHandler(fds->fd);
	if (close(fds->fd) < 0) {
	    errorCode = errno;
	}

    }
    fds = statePtr->fds.next;
    while (fds != NULL) {
	TcpFdList *next = fds->next;

	Tcl_Free(fds);
	fds = next;
    }
    if (statePtr->addrlist != NULL) {
	freeaddrinfo(statePtr->addrlist);
    }
    if (statePtr->myaddrlist != NULL) {
	freeaddrinfo(statePtr->myaddrlist);
    }
    Tcl_Free(statePtr);
    return errorCode;
}

/*
 *----------------------------------------------------------------------
 *
 * TcpClose2Proc --
 *
 *	This function is called by the generic IO level to perform the channel
 *	type specific part of a half-close: namely, a shutdown() on a socket.
 *
 * Results:
 *	0 if successful, the value of errno if failed.
 *
 * Side effects:
 *	Shuts down one side of the socket.
 *
 *----------------------------------------------------------------------
 */

static int
TcpClose2Proc(
    void *instanceData,		/* The socket to close. */
    TCL_UNUSED(Tcl_Interp *),
    int flags)			/* Flags that indicate which side to close. */
{
    TcpState *statePtr = (TcpState *)instanceData;
    int readError = 0;
    int writeError = 0;

    /*
     * Shutdown the OS socket handle.
     */
    if ((flags & (TCL_CLOSE_READ|TCL_CLOSE_WRITE)) == 0) {
	return TcpCloseProc(instanceData, NULL);
    }
    if ((flags & TCL_CLOSE_READ) && (shutdown(statePtr->fds.fd, SHUT_RD) < 0)) {
	readError = errno;
    }
    if ((flags & TCL_CLOSE_WRITE) && (shutdown(statePtr->fds.fd, SHUT_WR) < 0)) {
	writeError = errno;
    }
    return (readError != 0) ? readError : writeError;
}

/*
 *----------------------------------------------------------------------
 *
 * TcpHostPortList --
 *
 *	This function is called by the -gethostname and -getpeername switches
 *	of TcpGetOptionProc() to add three list elements with the textual
 *	representation of the given address to the given DString.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Adds three elements do dsPtr
 *
 *----------------------------------------------------------------------
 */

#ifndef NEED_FAKE_RFC2553
#if defined (__clang__) || ((__GNUC__)  && ((__GNUC__ > 4) || ((__GNUC__ == 4) && (__GNUC_MINOR__ > 5))))
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#endif
static inline int
IPv6AddressNeedsNumericRendering(
    struct in6_addr addr)
{
    if (IN6_ARE_ADDR_EQUAL(&addr, &in6addr_any)) {
	return 1;
    }

    /*
     * The IN6_IS_ADDR_V4MAPPED macro has a problem with aliasing warnings on
     * at least some versions of OSX.
     */

    if (!IN6_IS_ADDR_V4MAPPED(&addr)) {
	return 0;
    }

    return (addr.s6_addr[12] == 0 && addr.s6_addr[13] == 0
	    && addr.s6_addr[14] == 0 && addr.s6_addr[15] == 0);
}
#if defined (__clang__) || ((__GNUC__)  && ((__GNUC__ > 4) || ((__GNUC__ == 4) && (__GNUC_MINOR__ > 5))))
#pragma GCC diagnostic pop
#endif
#endif /* NEED_FAKE_RFC2553 */

static void
TcpHostPortList(
    Tcl_Interp *interp,
    Tcl_DString *dsPtr,
    address addr,
    socklen_t salen)
{
#define SUPPRESS_RDNS_VAR "::tcl::unsupported::noReverseDNS"
    char host[NI_MAXHOST], nhost[NI_MAXHOST], nport[NI_MAXSERV];
    int flags = 0;

    getnameinfo(&addr.sa, salen, nhost, sizeof(nhost), nport, sizeof(nport),
	    NI_NUMERICHOST | NI_NUMERICSERV);
    Tcl_DStringAppendElement(dsPtr, nhost);

    /*
     * We don't want to resolve INADDR_ANY and sin6addr_any; they can
     * sometimes cause problems (and never have a name).
     */

    if (addr.sa.sa_family == AF_INET) {
	if (addr.sa4.sin_addr.s_addr == INADDR_ANY) {
	    flags |= NI_NUMERICHOST;
	}
#ifndef NEED_FAKE_RFC2553
    } else if (addr.sa.sa_family == AF_INET6) {
	if (IPv6AddressNeedsNumericRendering(addr.sa6.sin6_addr)) {
	    flags |= NI_NUMERICHOST;
	}
#endif /* NEED_FAKE_RFC2553 */
    }

    /*
     * Check if reverse DNS has been switched off globally.
     */

    if (interp != NULL &&
	    Tcl_GetVar2(interp, SUPPRESS_RDNS_VAR, NULL, 0) != NULL) {
	flags |= NI_NUMERICHOST;
    }
    if (getnameinfo(&addr.sa, salen, host, sizeof(host), NULL, 0,
	    flags) == 0) {
	/*
	 * Reverse mapping worked.
	 */

	Tcl_DStringAppendElement(dsPtr, host);
    } else {
	/*
	 * Reverse mapping failed - use the numeric rep once more.
	 */

	Tcl_DStringAppendElement(dsPtr, nhost);
    }
    Tcl_DStringAppendElement(dsPtr, nport);
}

/*
 *----------------------------------------------------------------------
 *
 * TcpSetOptionProc --
 *
 *	Sets TCP channel specific options.
 *
 * Results:
 *	None, unless an error happens.
 *
 * Side effects:
 *	Changes attributes of the socket at the system level.
 *
 *----------------------------------------------------------------------
 */

static int
TcpSetOptionProc(
    void *instanceData,		/* Socket state. */
    Tcl_Interp *interp,		/* For error reporting - can be NULL. */
    const char *optionName,	/* Name of the option to set. */
    const char *value)		/* New value for option. */
{
    TcpState *statePtr = (TcpState *)instanceData;
    size_t len = 0;

    if (optionName != NULL) {
	len = strlen(optionName);
    }

    if ((len > 1) && (optionName[1] == 'k') &&
	    (strncmp(optionName, "-keepalive", len) == 0)) {
	int val = 0, ret;

	if (Tcl_GetBoolean(interp, value, &val) != TCL_OK) {
	    return TCL_ERROR;
	}
#if defined(SO_KEEPALIVE)
	ret = setsockopt(statePtr->fds.fd, SOL_SOCKET, SO_KEEPALIVE,
		(const char *) &val, sizeof(int));
#else
	ret = -1;
	Tcl_SetErrno(ENOTSUP);
#endif
	if (ret < 0) {
	    if (interp) {
		Tcl_SetObjResult(interp, Tcl_ObjPrintf(
			"couldn't set socket option: %s",
			Tcl_PosixError(interp)));
	    }
	    return TCL_ERROR;
	}
	return TCL_OK;
    }
    if ((len > 1) && (optionName[1] == 'n') &&
	    (strncmp(optionName, "-nodelay", len) == 0)) {
	int val = 0, ret;

	if (Tcl_GetBoolean(interp, value, &val) != TCL_OK) {
	    return TCL_ERROR;
	}
#if defined(SOL_TCP) && defined(TCP_NODELAY)
	ret = setsockopt(statePtr->fds.fd, SOL_TCP, TCP_NODELAY,
		(const char *) &val, sizeof(int));
#else
	ret = -1;
	Tcl_SetErrno(ENOTSUP);
#endif
	if (ret < 0) {
	    if (interp) {
		Tcl_SetObjResult(interp, Tcl_ObjPrintf(
			"couldn't set socket option: %s",
			Tcl_PosixError(interp)));
	    }
	    return TCL_ERROR;
	}
	return TCL_OK;
    }
    return Tcl_BadChannelOption(interp, optionName, "keepalive nodelay");
}

/*
 *----------------------------------------------------------------------
 *
 * TcpGetOptionProc --
 *
 *	Computes an option value for a TCP socket based channel, or a list of
 *	all options and their values.
 *
 *	Note: This code is based on code contributed by John Haxby.
 *
 * Results:
 *	A standard Tcl result. The value of the specified option or a list of
 *	all options and their values is returned in the supplied DString. Sets
 *	Error message if needed.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
TcpGetOptionProc(
    void *instanceData,		/* Socket state. */
    Tcl_Interp *interp,		/* For error reporting - can be NULL. */
    const char *optionName,	/* Name of the option to retrieve the value
				 * for, or NULL to get all options and their
				 * values. */
    Tcl_DString *dsPtr)		/* Where to store the computed value;
				 * initialized by caller. */
{
    TcpState *statePtr = (TcpState *)instanceData;
    size_t len = 0;

    if (optionName != NULL) {
	len = strlen(optionName);
    }

    if ((len > 1) && (optionName[1] == 'e') &&
	    (strncmp(optionName, "-error", len) == 0)) {
	socklen_t optlen = sizeof(int);

	WaitForConnect(statePtr, NULL);
	if (GOT_BITS(statePtr->flags, TCP_ASYNC_CONNECT)) {
	    /*
	     * Suppress errors as long as we are not done.
	     */

	    errno = 0;
	} else if (statePtr->connectError != 0) {
	    errno = statePtr->connectError;
	    statePtr->connectError = 0;
	} else {
	    int err;

	    getsockopt(statePtr->fds.fd, SOL_SOCKET, SO_ERROR, (char *) &err,
		    &optlen);
	    errno = err;
	}
	if (errno != 0) {
	    Tcl_DStringAppend(dsPtr, Tcl_ErrnoMsg(errno), TCL_INDEX_NONE);
	}
	return TCL_OK;
    }

    if ((len > 1) && (optionName[1] == 'c') &&
	    (strncmp(optionName, "-connecting", len) == 0)) {
	WaitForConnect(statePtr, NULL);
	Tcl_DStringAppend(dsPtr,
		GOT_BITS(statePtr->flags, TCP_ASYNC_CONNECT) ? "1" : "0", TCL_INDEX_NONE);
	return TCL_OK;
    }

    if ((len == 0) || ((len > 1) && (optionName[1] == 'p') &&
	    (strncmp(optionName, "-peername", len) == 0))) {
	address peername;
	socklen_t size = sizeof(peername);

	WaitForConnect(statePtr, NULL);
	if (GOT_BITS(statePtr->flags, TCP_ASYNC_CONNECT)) {
	    /*
	     * In async connect output an empty string
	     */

	    if (len == 0) {
		Tcl_DStringAppendElement(dsPtr, "-peername");
		Tcl_DStringAppendElement(dsPtr, "");
	    } else {
		return TCL_OK;
	    }
	} else if (getpeername(statePtr->fds.fd, &peername.sa, &size) >= 0) {
	    /*
	     * Peername fetch succeeded - output list
	     */

	    if (len == 0) {
		Tcl_DStringAppendElement(dsPtr, "-peername");
		Tcl_DStringStartSublist(dsPtr);
	    }
	    TcpHostPortList(interp, dsPtr, peername, size);
	    if (len) {
		return TCL_OK;
	    }
	    Tcl_DStringEndSublist(dsPtr);
	} else {
	    /*
	     * getpeername failed - but if we were asked for all the options
	     * (len==0), don't flag an error at that point because it could be
	     * an fconfigure request on a server socket (which have no peer).
	     * Same must be done on win&mac.
	     */

	    if (len) {
		if (interp) {
		    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
			    "can't get peername: %s",
			    Tcl_PosixError(interp)));
		}
		return TCL_ERROR;
	    }
	}
    }

    if ((len == 0) || ((len > 1) && (optionName[1] == 's') &&
	    (strncmp(optionName, "-sockname", len) == 0))) {
	TcpFdList *fds;
	address sockname;
	socklen_t size;
	int found = 0;

	WaitForConnect(statePtr, NULL);
	if (len == 0) {
	    Tcl_DStringAppendElement(dsPtr, "-sockname");
	    Tcl_DStringStartSublist(dsPtr);
	}
	if (GOT_BITS(statePtr->flags, TCP_ASYNC_CONNECT)) {
	    /*
	     * In async connect output an empty string
	     */

	    found = 1;
	} else {
	    for (fds = &statePtr->fds; fds != NULL; fds = fds->next) {
		size = sizeof(sockname);
		if (getsockname(fds->fd, &(sockname.sa), &size) >= 0) {
		    found = 1;
		    TcpHostPortList(interp, dsPtr, sockname, size);
		}
	    }
	}
	if (found) {
	    if (len) {
		return TCL_OK;
	    }
	    Tcl_DStringEndSublist(dsPtr);
	} else {
	    if (interp) {
		Tcl_SetObjResult(interp, Tcl_ObjPrintf(
			"can't get sockname: %s", Tcl_PosixError(interp)));
	    }
	    return TCL_ERROR;
	}
    }

    if ((len == 0) || ((len > 1) && (optionName[1] == 'k') &&
	    (strncmp(optionName, "-keepalive", len) == 0))) {
	int opt = 0;
#if defined(SO_KEEPALIVE)
	socklen_t size = sizeof(opt);
#endif

	if (len == 0) {
	    Tcl_DStringAppendElement(dsPtr, "-keepalive");
	}
#if defined(SO_KEEPALIVE)
	getsockopt(statePtr->fds.fd, SOL_SOCKET, SO_KEEPALIVE,
		(char *) &opt, &size);
#endif
	Tcl_DStringAppendElement(dsPtr, opt ? "1" : "0");
	if (len > 0) {
	    return TCL_OK;
	}
    }

    if ((len == 0) || ((len > 1) && (optionName[1] == 'n') &&
	    (strncmp(optionName, "-nodelay", len) == 0))) {
	int opt = 0;
#if defined(SOL_TCP) && defined(TCP_NODELAY)
	socklen_t size = sizeof(opt);
#endif

	if (len == 0) {
	    Tcl_DStringAppendElement(dsPtr, "-nodelay");
	}
#if defined(SOL_TCP) && defined(TCP_NODELAY)
	getsockopt(statePtr->fds.fd, SOL_TCP, TCP_NODELAY,
		(char *) &opt, &size);
#endif
	Tcl_DStringAppendElement(dsPtr, opt ? "1" : "0");
	if (len > 0) {
	    return TCL_OK;
	}
    }

    if (len > 0) {
	return Tcl_BadChannelOption(interp, optionName,
		"connecting keepalive nodelay peername sockname");
    }

    return TCL_OK;
}

/*
 * ----------------------------------------------------------------------
 *
 * TcpThreadActionProc --
 *
 *	Handles detach/attach for asynchronously connecting socket.
 *
 *	Reassigning the file handler associated with thread-related channel
 *	notification, responsible for callbacks (signaling that asynchronous
 *	connection attempt has succeeded or failed).
 *
 * Results:
 *	None.
 *
 * ----------------------------------------------------------------------
 */

static void
TcpThreadActionProc(
    void *instanceData,
    int action)
{
    TcpState *statePtr = (TcpState *)instanceData;

    if (GOT_BITS(statePtr->flags, TCP_ASYNC_CONNECT)) {
	/*
	 * Async-connecting socket must get reassigned handler if it have been
	 * transferred to another thread. Remove the handler if the socket is
	 * not managed by this thread anymore and create new handler (TSD related)
	 * so the callback will run in the correct thread, bug [f583715154].
	 */
	switch (action) {
	case TCL_CHANNEL_THREAD_REMOVE:
	    CLEAR_BITS(statePtr->flags, TCP_ASYNC_PENDING);
	    Tcl_DeleteFileHandler(statePtr->fds.fd);
	    break;
	case TCL_CHANNEL_THREAD_INSERT:
	    Tcl_CreateFileHandler(statePtr->fds.fd,
		    TCL_WRITABLE | TCL_EXCEPTION, TcpAsyncCallback, statePtr);
	    SET_BITS(statePtr->flags, TCP_ASYNC_PENDING);
	    break;
	}
    }
}

/*
 * ----------------------------------------------------------------------
 *
 * TcpWatchProc --
 *
 *	Initialize the notifier to watch the fd from this channel.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Sets up the notifier so that a future event on the channel will be
 *	seen by Tcl.
 *
 * ----------------------------------------------------------------------
 */

static void
WrapNotify(
    void *clientData,
    int mask)
{
    TcpState *statePtr = (TcpState *) clientData;
    int newmask = mask & statePtr->interest;

    if (newmask == 0) {
	/*
	 * There was no overlap between the states the channel is interested
	 * in notifications for, and the states that are reported present on
	 * the file descriptor by select().  The only way that can happen is
	 * when the channel is interested in a writable condition, and only a
	 * readable state is reported present (see TcpWatchProc() below).  In
	 * that case, signal back to the caller the writable state, which is
	 * really an error condition.  As an extra check on that assumption,
	 * check for a non-zero value of errno before reporting an artificial
	 * writable state.
	 */

	if (errno == 0) {
	    return;
	}
	newmask = TCL_WRITABLE;
    }
    Tcl_NotifyChannel(statePtr->channel, newmask);
}

static void
TcpWatchProc(
    void *instanceData,		/* The socket state. */
    int mask)			/* Events of interest; an OR-ed combination of
				 * TCL_READABLE, TCL_WRITABLE and
				 * TCL_EXCEPTION. */
{
    TcpState *statePtr = (TcpState *)instanceData;

    if (statePtr->acceptProc != NULL) {
	/*
	 * Make sure we don't mess with server sockets since they will never
	 * be readable or writable at the Tcl level. This keeps Tcl scripts
	 * from interfering with the -accept behavior (bug #3394732).
	 */

	return;
    }

    if (GOT_BITS(statePtr->flags, TCP_ASYNC_PENDING)) {
	/*
	 * Async sockets use a FileHandler internally while connecting, so we
	 * need to cache this request until the connection has succeeded.
	 */

	statePtr->filehandlers = mask;
    } else if (mask) {

	/*
	 * Whether it is a bug or feature or otherwise, it is a fact of life
	 * that on at least some Linux kernels select() fails to report that a
	 * socket file descriptor is writable when the other end of the socket
	 * is closed.  This is in contrast to the guarantees Tcl makes that
	 * its channels become writable and fire writable events on an error
	 * condition.  This has caused a leak of file descriptors in a state of
	 * background flushing.  See Tcl ticket 1758a0b603.
	 *
	 * As a workaround, when our caller indicates an interest in writable
	 * notifications, we must tell the notifier built around select() that
	 * we are interested in the readable state of the file descriptor as
	 * well, as that is the only reliable means to get notified of error
	 * conditions.  Then it is the task of WrapNotify() above to untangle
	 * the meaning of these channel states and report the chan events as
	 * best it can.  We save a copy of the mask passed in to assist with
	 * that.
	 */

	statePtr->interest = mask;
	Tcl_CreateFileHandler(statePtr->fds.fd, mask|TCL_READABLE,
		WrapNotify, statePtr);
    } else {
	Tcl_DeleteFileHandler(statePtr->fds.fd);
    }
}

/*
 * ----------------------------------------------------------------------
 *
 * TcpGetHandleProc --
 *
 *	Called from Tcl_GetChannelHandle to retrieve OS handles from inside a
 *	TCP socket based channel.
 *
 * Results:
 *	Returns TCL_OK with the fd in handlePtr, or TCL_ERROR if there is no
 *	handle for the specified direction.
 *
 * Side effects:
 *	None.
 *
 * ----------------------------------------------------------------------
 */

static int
TcpGetHandleProc(
    void *instanceData,		/* The socket state. */
    TCL_UNUSED(int) /*direction*/,
    void **handlePtr)		/* Where to store the handle. */
{
    TcpState *statePtr = (TcpState *)instanceData;

    *handlePtr = INT2PTR(statePtr->fds.fd);
    return TCL_OK;
}

/*
 * ----------------------------------------------------------------------
 *
 * TcpAsyncCallback --
 *
 *	Called by the event handler that TcpConnect sets up internally for
 *	[socket -async] to get notified when the asynchronous connection
 *	attempt has succeeded or failed.
 *
 * ----------------------------------------------------------------------
 */

static void
TcpAsyncCallback(
    void *clientData,		/* The socket state. */
    TCL_UNUSED(int) /*mask*/)
{
    TcpConnect(NULL, (TcpState *)clientData);
}

/*
 * ----------------------------------------------------------------------
 *
 * TcpConnect --
 *
 *	This function opens a new socket in client mode.
 *
 * Results:
 *      TCL_OK, if the socket was successfully connected or an asynchronous
 *      connection is in progress. If an error occurs, TCL_ERROR is returned
 *      and an error message is left in interp.
 *
 * Side effects:
 *	Opens a socket.
 *
 * Remarks:
 *	A single host name may resolve to more than one IP address, e.g. for
 *	an IPv4/IPv6 dual stack host. For handling asynchronously connecting
 *	sockets in the background for such hosts, this function can act as a
 *	coroutine. On the first call, it sets up the control variables for the
 *	two nested loops over the local and remote addresses. Once the first
 *	connection attempt is in progress, it sets up itself as a writable
 *	event handler for that socket, and returns. When the callback occurs,
 *	control is transferred to the "reenter" label, right after the initial
 *	return and the loops resume as if they had never been interrupted.
 *	For synchronously connecting sockets, the loops work the usual way.
 *
 * ----------------------------------------------------------------------
 */

static int
TcpConnect(
    Tcl_Interp *interp,		/* For error reporting; can be NULL. */
    TcpState *statePtr)
{
    socklen_t optlen;
    int async_callback = GOT_BITS(statePtr->flags, TCP_ASYNC_PENDING);
    int ret = -1, error = EHOSTUNREACH;
    int async = GOT_BITS(statePtr->flags, TCP_ASYNC_CONNECT);
    static const int reuseaddr = 1;

    if (async_callback) {
	goto reenter;
    }

    for (statePtr->addr = statePtr->addrlist; statePtr->addr != NULL;
	    statePtr->addr = statePtr->addr->ai_next) {
	for (statePtr->myaddr = statePtr->myaddrlist;
		statePtr->myaddr != NULL;
		statePtr->myaddr = statePtr->myaddr->ai_next) {
	    /*
	     * No need to try combinations of local and remote addresses of
	     * different families.
	     */

	    if (statePtr->myaddr->ai_family != statePtr->addr->ai_family) {
		continue;
	    }

	    /*
	     * Close the socket if it is still open from the last unsuccessful
	     * iteration.
	     */

	    if (statePtr->fds.fd >= 0) {
		close(statePtr->fds.fd);
		statePtr->fds.fd = -1;
		errno = 0;
	    }

	    statePtr->fds.fd = socket(statePtr->addr->ai_family, SOCK_STREAM,
		    0);
	    if (statePtr->fds.fd < 0) {
		continue;
	    }

	    /*
	     * Set the close-on-exec flag so that the socket will not get
	     * inherited by child processes.
	     */

	    fcntl(statePtr->fds.fd, F_SETFD, FD_CLOEXEC);

	    /*
	     * Set kernel space buffering
	     */

	    TclSockMinimumBuffers(INT2PTR(statePtr->fds.fd), SOCKET_BUFSIZE);

	    if (async) {
		ret = TclUnixSetBlockingMode(statePtr->fds.fd,
			TCL_MODE_NONBLOCKING);
		if (ret < 0) {
		    continue;
		}
	    }

	    /*
	     * Must reset the error variable here, before we use it for the
	     * first time in this iteration.
	     */

	    error = 0;

	    (void) setsockopt(statePtr->fds.fd, SOL_SOCKET, SO_REUSEADDR,
		    (char *) &reuseaddr, sizeof(reuseaddr));
	    ret = bind(statePtr->fds.fd, statePtr->myaddr->ai_addr,
		    statePtr->myaddr->ai_addrlen);
	    if (ret < 0) {
		error = errno;
		continue;
	    }

	    /*
	     * Attempt to connect. The connect may fail at present with an
	     * EINPROGRESS but at a later time it will complete. The caller
	     * will set up a file handler on the socket if she is interested
	     * in being informed when the connect completes.
	     */

	    ret = connect(statePtr->fds.fd, statePtr->addr->ai_addr,
			statePtr->addr->ai_addrlen);
	    if (ret < 0) {
		error = errno;
	    }
	    if (ret < 0 && errno == EINPROGRESS) {
		Tcl_CreateFileHandler(statePtr->fds.fd,
			TCL_WRITABLE | TCL_EXCEPTION, TcpAsyncCallback,
			statePtr);
		errno = EWOULDBLOCK;
		SET_BITS(statePtr->flags, TCP_ASYNC_PENDING);
		return TCL_OK;

	    reenter:
		CLEAR_BITS(statePtr->flags, TCP_ASYNC_PENDING);
		Tcl_DeleteFileHandler(statePtr->fds.fd);

		/*
		 * Read the error state from the socket to see if the async
		 * connection has succeeded or failed. As this clears the
		 * error condition, we cache the status in the socket state
		 * struct for later retrieval by [fconfigure -error].
		 */

		optlen = sizeof(int);

		getsockopt(statePtr->fds.fd, SOL_SOCKET, SO_ERROR,
			(char *) &error, &optlen);
		errno = error;
	    }
	    if (error == 0) {
		goto out;
	    }
	}
    }

  out:
    statePtr->connectError = error;
    CLEAR_BITS(statePtr->flags, TCP_ASYNC_CONNECT);
    if (async_callback) {
	/*
	 * An asynchonous connection has finally succeeded or failed.
	 */

	TcpWatchProc(statePtr, statePtr->filehandlers);
	TclUnixSetBlockingMode(statePtr->fds.fd, statePtr->cachedBlocking);

	if (error != 0) {
	    SET_BITS(statePtr->flags, TCP_ASYNC_FAILED);
	}

	/*
	 * We need to forward the writable event that brought us here, because
	 * upon reading of getsockopt(SO_ERROR), at least some OSes clear the
	 * writable state from the socket, and so a subsequent select() on
	 * behalf of a script level [fileevent] would not fire. It doesn't
	 * hurt that this is also called in the successful case and will save
	 * the event mechanism one roundtrip through select().
	 */

	if (statePtr->cachedBlocking == TCL_MODE_NONBLOCKING) {
	    Tcl_NotifyChannel(statePtr->channel, TCL_WRITABLE);
	}
    }
    if (error != 0) {
	/*
	 * Failure for either a synchronous connection, or an async one that
	 * failed before it could enter background mode, e.g. because an
	 * invalid -myaddr was given.
	 */

	if (interp != NULL) {
	    errno = error;
	    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		    "couldn't open socket: %s", Tcl_PosixError(interp)));
	}
	return TCL_ERROR;
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_OpenTcpClient --
 *
 *	Opens a TCP client socket and creates a channel around it.
 *
 * Results:
 *	The channel or NULL if failed. An error message is returned in the
 *	interpreter on failure.
 *
 * Side effects:
 *	Opens a client socket and creates a new channel.
 *
 *----------------------------------------------------------------------
 */

Tcl_Channel
Tcl_OpenTcpClient(
    Tcl_Interp *interp,		/* For error reporting; can be NULL. */
    int port,			/* Port number to open. */
    const char *host,		/* Host on which to open port. */
    const char *myaddr,		/* Client-side address */
    int myport,			/* Client-side port */
    int async)			/* If nonzero, attempt to do an asynchronous
				 * connect. Otherwise we do a blocking
				 * connect. */
{
    TcpState *statePtr;
    const char *errorMsg = NULL;
    struct addrinfo *addrlist = NULL, *myaddrlist = NULL;
    char channelName[SOCK_CHAN_LENGTH];

    /*
     * Do the name lookups for the local and remote addresses.
     */

    if (!TclCreateSocketAddress(interp, &addrlist, host, port, 0, &errorMsg)
	    || !TclCreateSocketAddress(interp, &myaddrlist, myaddr, myport, 1,
		    &errorMsg)) {
	if (addrlist != NULL) {
	    freeaddrinfo(addrlist);
	}
	if (interp != NULL) {
	    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		    "couldn't open socket: %s", errorMsg));
	}
	return NULL;
    }

    /*
     * Allocate a new TcpState for this socket.
     */

    statePtr = (TcpState *)Tcl_Alloc(sizeof(TcpState));
    memset(statePtr, 0, sizeof(TcpState));
    statePtr->flags = async ? TCP_ASYNC_CONNECT : 0;
    statePtr->cachedBlocking = TCL_MODE_BLOCKING;
    statePtr->addrlist = addrlist;
    statePtr->myaddrlist = myaddrlist;
    statePtr->fds.fd = -1;

    /*
     * Create a new client socket and wrap it in a channel.
     */

    if (TcpConnect(interp, statePtr) != TCL_OK) {
	TcpCloseProc(statePtr, NULL);
	return NULL;
    }

    snprintf(channelName, sizeof(channelName), SOCK_TEMPLATE, PTR2INT(statePtr));

    statePtr->channel = Tcl_CreateChannel(&tcpChannelType, channelName,
	    statePtr, TCL_READABLE | TCL_WRITABLE);
    if (Tcl_SetChannelOption(interp, statePtr->channel, "-translation",
	    "auto crlf") == TCL_ERROR) {
	Tcl_CloseEx(NULL, statePtr->channel, 0);
	return NULL;
    }
    return statePtr->channel;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_MakeTcpClientChannel --
 *
 *	Creates a Tcl_Channel from an existing client TCP socket.
 *
 * Results:
 *	The Tcl_Channel wrapped around the preexisting TCP socket.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_Channel
Tcl_MakeTcpClientChannel(
    void *sock)			/* The socket to wrap up into a channel. */
{
    return (Tcl_Channel) TclpMakeTcpClientChannelMode(sock,
	    TCL_READABLE | TCL_WRITABLE);
}

/*
 *----------------------------------------------------------------------
 *
 * TclpMakeTcpClientChannelMode --
 *
 *	Creates a Tcl_Channel from an existing client TCP socket
 *	with given mode.
 *
 * Results:
 *	The Tcl_Channel wrapped around the preexisting TCP socket.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

void *
TclpMakeTcpClientChannelMode(
    void *sock,			/* The socket to wrap up into a channel. */
    int mode)			/* OR'ed combination of TCL_READABLE and
				 * TCL_WRITABLE to indicate file mode. */
{
    TcpState *statePtr;
    char channelName[SOCK_CHAN_LENGTH];

    statePtr = (TcpState *)Tcl_Alloc(sizeof(TcpState));
    memset(statePtr, 0, sizeof(TcpState));
    statePtr->fds.fd = (int)PTR2INT(sock);
    statePtr->flags = 0;

    snprintf(channelName, sizeof(channelName), SOCK_TEMPLATE, PTR2INT(statePtr));

    statePtr->channel = Tcl_CreateChannel(&tcpChannelType, channelName,
	    statePtr, mode);
    if (Tcl_SetChannelOption(NULL, statePtr->channel, "-translation",
	    "auto crlf") == TCL_ERROR) {
	Tcl_CloseEx(NULL, statePtr->channel, 0);
	return NULL;
    }
    return statePtr->channel;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_OpenTcpServerEx --
 *
 *	Opens a TCP server socket and creates a channel around it.
 *
 * Results:
 *	The channel or NULL if failed. If an error occurred, an error message
 *	is left in the interp's result if interp is not NULL.
 *
 * Side effects:
 *	Opens a server socket and creates a new channel.
 *
 *----------------------------------------------------------------------
 */

Tcl_Channel
Tcl_OpenTcpServerEx(
    Tcl_Interp *interp,		/* For error reporting - may be NULL. */
    const char *service,	/* Port number to open. */
    const char *myHost,		/* Name of local host. */
    unsigned int flags,		/* Flags. */
    int backlog,		/* Length of OS listen backlog queue. */
    Tcl_TcpAcceptProc *acceptProc,
				/* Callback for accepting connections from new
				 * clients. */
    void *acceptProcData)	/* Data for the callback. */
{
    int status = 0, sock = -1, optvalue, port, chosenport;
    struct addrinfo *addrlist = NULL, *addrPtr;	/* socket address */
    TcpState *statePtr = NULL;
    char channelName[SOCK_CHAN_LENGTH];
    const char *errorMsg = NULL;
    TcpFdList *fds = NULL, *newfds;

    /*
     * Try to record and return the most meaningful error message, i.e. the
     * one from the first socket that went the farthest before it failed.
     */

    enum { LOOKUP, SOCKET, BIND, LISTEN } howfar = LOOKUP;
    int my_errno = 0;

    /*
     * If we were called with port 0 to listen on a random port number, we
     * copy the port number from the first member of the addrinfo list to all
     * subsequent members, so that IPv4 and IPv6 listen on the same port. This
     * might fail to bind() with EADDRINUSE if a port is free on the first
     * address family in the list but already used on the other. In this case
     * we revert everything we've done so far and start from scratch hoping
     * that next time we'll find a port number that is usable on all address
     * families. We try this at most MAXRETRY times to avoid an endless loop
     * if all ports are taken.
     */

    int retry = 0;
#define MAXRETRY 10

  repeat:
    if (retry > 0) {
	if (statePtr != NULL) {
	    TcpCloseProc(statePtr, NULL);
	    statePtr = NULL;
	}
	if (addrlist != NULL) {
	    freeaddrinfo(addrlist);
	    addrlist = NULL;
	}
	if (retry >= MAXRETRY) {
	    goto error;
	}
    }
    retry++;
    chosenport = 0;

    if (TclSockGetPort(interp, service, "tcp", &port) != TCL_OK) {
	errorMsg = "invalid port number";
	goto error;
    }

    if (!TclCreateSocketAddress(interp, &addrlist, myHost, port, 1,
	    &errorMsg)) {
	my_errno = errno;
	goto error;
    }

    for (addrPtr = addrlist; addrPtr != NULL; addrPtr = addrPtr->ai_next) {
	sock = socket(addrPtr->ai_family, addrPtr->ai_socktype,
		addrPtr->ai_protocol);
	if (sock == -1) {
	    if (howfar < SOCKET) {
		howfar = SOCKET;
		my_errno = errno;
	    }
	    continue;
	}

	/*
	 * Set the close-on-exec flag so that the socket will not get
	 * inherited by child processes.
	 */

	fcntl(sock, F_SETFD, FD_CLOEXEC);

	/*
	 * Set kernel space buffering
	 */

	TclSockMinimumBuffers(INT2PTR(sock), SOCKET_BUFSIZE);

	/*
	 * Set up to reuse server addresses and/or ports if requested.
	 */

	if (GOT_BITS(flags, TCL_TCPSERVER_REUSEADDR)) {
	    optvalue = 1;
	    (void) setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
		    (char *) &optvalue, sizeof(optvalue));
	}

	if (GOT_BITS(flags, TCL_TCPSERVER_REUSEPORT)) {
#ifndef SO_REUSEPORT
	    /*
	     * If the platform doesn't support the SO_REUSEPORT flag we can't
	     * do much beside erroring out.
	     */

	    errorMsg = "SO_REUSEPORT isn't supported by this platform";
	    goto error;
#else
	    optvalue = 1;
	    (void) setsockopt(sock, SOL_SOCKET, SO_REUSEPORT,
		    (char *) &optvalue, sizeof(optvalue));
#endif
	}

	/*
	 * Make sure we use the same port number when opening two server
	 * sockets for IPv4 and IPv6 on a random port.
	 *
	 * As sockaddr_in6 uses the same offset and size for the port member
	 * as sockaddr_in, we can handle both through the IPv4 API.
	 */

	if (port == 0 && chosenport != 0) {
	    ((struct sockaddr_in *)addrPtr->ai_addr)->sin_port =
		    htons((uint16_t)chosenport);
	}

#ifdef IPV6_V6ONLY
	/*
	 * Missing on: Solaris 2.8
	 */

	if (addrPtr->ai_family == AF_INET6) {
	    int v6only = 1;

	    (void) setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY,
		    &v6only, sizeof(v6only));
	}
#endif /* IPV6_V6ONLY */

	status = bind(sock, addrPtr->ai_addr, addrPtr->ai_addrlen);
	if (status == -1) {
	    if (howfar < BIND) {
		howfar = BIND;
		my_errno = errno;
	    }
	    close(sock);
	    sock = -1;
	    if (port == 0 && errno == EADDRINUSE) {
		goto repeat;
	    }
	    continue;
	}
	if (port == 0 && chosenport == 0) {
	    address sockname;
	    socklen_t namelen = sizeof(sockname);

	    /*
	     * Synchronize port numbers when binding to port 0 of multiple
	     * addresses.
	     */

	    if (getsockname(sock, &sockname.sa, &namelen) >= 0) {
		chosenport = ntohs(sockname.sa4.sin_port);
	    }
	}
	if (backlog < 0) {
	    backlog = SOMAXCONN;
	}
	status = listen(sock, backlog);
	if (status < 0) {
	    if (howfar < LISTEN) {
		howfar = LISTEN;
		my_errno = errno;
	    }
	    close(sock);
	    sock = -1;
	    if (port == 0 && errno == EADDRINUSE) {
		goto repeat;
	    }
	    continue;
	}
	if (statePtr == NULL) {
	    /*
	     * Allocate a new TcpState for this socket.
	     */

	    statePtr = (TcpState *)Tcl_Alloc(sizeof(TcpState));
	    memset(statePtr, 0, sizeof(TcpState));
	    statePtr->acceptProc = acceptProc;
	    statePtr->acceptProcData = acceptProcData;
	    snprintf(channelName, sizeof(channelName), SOCK_TEMPLATE, PTR2INT(statePtr));
	    newfds = &statePtr->fds;
	} else {
	    newfds = (TcpFdList *)Tcl_Alloc(sizeof(TcpFdList));
	    memset(newfds, (int) 0, sizeof(TcpFdList));
	    fds->next = newfds;
	}
	newfds->fd = sock;
	newfds->statePtr = statePtr;
	fds = newfds;

	/*
	 * Set up the callback mechanism for accepting connections from new
	 * clients.
	 */

	Tcl_CreateFileHandler(sock, TCL_READABLE, TcpAccept, fds);
    }

  error:
    if (addrlist != NULL) {
	freeaddrinfo(addrlist);
    }
    if (statePtr != NULL) {
	statePtr->channel = Tcl_CreateChannel(&tcpChannelType, channelName,
		statePtr, 0);
	return statePtr->channel;
    }
    if (interp != NULL) {
	Tcl_Obj *errorObj = Tcl_NewStringObj("couldn't open socket: ", TCL_INDEX_NONE);

	if (errorMsg == NULL) {
	    errno = my_errno;
	    Tcl_AppendToObj(errorObj, Tcl_PosixError(interp), TCL_INDEX_NONE);
	} else {
	    Tcl_AppendToObj(errorObj, errorMsg, TCL_INDEX_NONE);
	}
	Tcl_SetObjResult(interp, errorObj);
    }
    if (sock != -1) {
	close(sock);
    }
    return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * TcpAccept --
 *	Accept a TCP socket connection.	 This is called by the event loop.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Creates a new connection socket. Calls the registered callback for the
 *	connection acceptance mechanism.
 *
 *----------------------------------------------------------------------
 */

static void
TcpAccept(
    void *data,			/* Callback token. */
    TCL_UNUSED(int) /*mask*/)
{
    TcpFdList *fds = (TcpFdList *)data;	/* Client data of server socket. */
    int newsock;		/* The new client socket */
    TcpState *newSockState;	/* State for new socket. */
    address addr;		/* The remote address */
    socklen_t len;		/* For accept interface */
    char channelName[SOCK_CHAN_LENGTH];
    char host[NI_MAXHOST], port[NI_MAXSERV];

    len = sizeof(addr);
    newsock = accept(fds->fd, &addr.sa, &len);
    if (newsock < 0) {
	return;
    }

    /*
     * Set close-on-exec flag to prevent the newly accepted socket from being
     * inherited by child processes.
     */

    (void) fcntl(newsock, F_SETFD, FD_CLOEXEC);

    newSockState = (TcpState *)Tcl_Alloc(sizeof(TcpState));
    memset(newSockState, 0, sizeof(TcpState));
    newSockState->flags = 0;
    newSockState->fds.fd = newsock;

    snprintf(channelName, sizeof(channelName), SOCK_TEMPLATE, PTR2INT(newSockState));
    newSockState->channel = Tcl_CreateChannel(&tcpChannelType, channelName,
	    newSockState, TCL_READABLE | TCL_WRITABLE);

    Tcl_SetChannelOption(NULL, newSockState->channel, "-translation",
	    "auto crlf");

    if (fds->statePtr->acceptProc != NULL) {
	getnameinfo(&addr.sa, len, host, sizeof(host), port, sizeof(port),
		NI_NUMERICHOST|NI_NUMERICSERV);
	fds->statePtr->acceptProc(fds->statePtr->acceptProcData,
		newSockState->channel, host, atoi(port));
    }
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 */
