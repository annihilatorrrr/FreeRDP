/**
 * WinPR: Windows Portable Runtime
 * Serial Communication API
 *
 * Copyright 2014 Hewlett-Packard Development Company, L.P.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <winpr/config.h>

#include <winpr/assert.h>
#include <errno.h>
#include <termios.h>
#include <unistd.h>

#include <winpr/io.h>
#include <winpr/wlog.h>
#include <winpr/wtypes.h>

#include "comm.h"

BOOL _comm_set_permissive(HANDLE hDevice, BOOL permissive)
{
	WINPR_COMM* pComm = (WINPR_COMM*)hDevice;

	if (!CommIsHandled(hDevice))
		return FALSE;

	pComm->permissive = permissive;
	return TRUE;
}

/* Computes VTIME in deciseconds from Ti in milliseconds */
static UCHAR svtime(ULONG Ti)
{
	/* FIXME: look for an equivalent math function otherwise let
	 * do the compiler do the optimization */
	if (Ti == 0)
		return 0;
	else if (Ti < 100)
		return 1;
	else if (Ti > 25500)
		return 255; /* 0xFF */
	else
		return (UCHAR)(Ti / 100);
}

/**
 * ERRORS:
 *   ERROR_INVALID_HANDLE
 *   ERROR_NOT_SUPPORTED
 *   ERROR_INVALID_PARAMETER
 *   ERROR_TIMEOUT
 *   ERROR_IO_DEVICE
 *   ERROR_BAD_DEVICE
 */
BOOL CommReadFile(HANDLE hDevice, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
                  LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped)
{
	WINPR_COMM* pComm = (WINPR_COMM*)hDevice;
	int biggestFd = -1;
	fd_set read_set;
	int nbFds = 0;
	COMMTIMEOUTS* pTimeouts = NULL;
	UCHAR vmin = 0;
	UCHAR vtime = 0;
	LONGLONG Tmax = 0;
	struct timeval tmaxTimeout;
	struct timeval* pTmaxTimeout = NULL;
	struct termios currentTermios;
	EnterCriticalSection(&pComm->ReadLock); /* KISSer by the function's beginning */

	if (!CommIsHandled(hDevice))
		goto return_false;

	if (lpOverlapped != NULL)
	{
		SetLastError(ERROR_NOT_SUPPORTED);
		goto return_false;
	}

	if (lpNumberOfBytesRead == NULL)
	{
		SetLastError(ERROR_INVALID_PARAMETER); /* since we doesn't support lpOverlapped != NULL */
		goto return_false;
	}

	*lpNumberOfBytesRead = 0; /* will be adjusted if required ... */

	if (nNumberOfBytesToRead <= 0) /* N */
	{
		goto return_true; /* FIXME: or FALSE? */
	}

	if (tcgetattr(pComm->fd, &currentTermios) < 0)
	{
		SetLastError(ERROR_IO_DEVICE);
		goto return_false;
	}

	if (currentTermios.c_lflag & ICANON)
	{
		CommLog_Print(WLOG_WARN, "Canonical mode not supported"); /* the timeout could not be set */
		SetLastError(ERROR_NOT_SUPPORTED);
		goto return_false;
	}

	/* http://msdn.microsoft.com/en-us/library/hh439614%28v=vs.85%29.aspx
	 * http://msdn.microsoft.com/en-us/library/windows/hardware/hh439614%28v=vs.85%29.aspx
	 *
	 * ReadIntervalTimeout  | ReadTotalTimeoutMultiplier | ReadTotalTimeoutConstant | VMIN | VTIME |
	 * TMAX  | 0            |            0               |           0              |   N  |   0   |
	 * INDEF | Blocks for N bytes available. 0< Ti <MAXULONG	|            0               | 0 |
	 * N  |   Ti  | INDEF | Blocks on first byte, then use Ti between bytes. MAXULONG       | 0 | 0
	 * |   0  |   0   |   0   | Returns immediately with bytes available (don't block) MAXULONG |
	 * MAXULONG           |      0< Tc <MAXULONG     |   N  |   0   |   Tc  | Blocks on first byte
	 * during Tc or returns immediately with bytes available MAXULONG       |            m |
	 * MAXULONG          |                      | Invalid 0            |            m |      0< Tc
	 * <MAXULONG     |   N  |   0   |  Tmax | Blocks on first byte during Tmax or returns
	 * immediately with bytes available 0< Ti <MAXULONG    |            m               |      0<
	 * Tc <MAXULONG     |   N  |   Ti  |  Tmax | Blocks on first byte, then use Ti between bytes.
	 * Tmax is used for the whole system call.
	 */
	/* NB: timeouts are in milliseconds, VTIME are in deciseconds and is an unsigned char */
	/* FIXME: double check whether open(pComm->fd_read_event, O_NONBLOCK) doesn't conflict with
	 * above use cases */
	pTimeouts = &(pComm->timeouts);

	if ((pTimeouts->ReadIntervalTimeout == MAXULONG) &&
	    (pTimeouts->ReadTotalTimeoutConstant == MAXULONG))
	{
		CommLog_Print(
		    WLOG_WARN,
		    "ReadIntervalTimeout and ReadTotalTimeoutConstant cannot be both set to MAXULONG");
		SetLastError(ERROR_INVALID_PARAMETER);
		goto return_false;
	}

	/* VMIN */

	if ((pTimeouts->ReadIntervalTimeout == MAXULONG) &&
	    (pTimeouts->ReadTotalTimeoutMultiplier == 0) && (pTimeouts->ReadTotalTimeoutConstant == 0))
	{
		vmin = 0;
	}
	else
	{
		/* N */
		/* vmin = nNumberOfBytesToRead < 256 ? nNumberOfBytesToRead : 255;*/ /* 0xFF */
		/* NB: we might wait endlessly with vmin=N, prefer to
		 * force vmin=1 and return with bytes
		 * available. FIXME: is a feature disarded here? */
		vmin = 1;
	}

	/* VTIME */

	if ((pTimeouts->ReadIntervalTimeout > 0) && (pTimeouts->ReadIntervalTimeout < MAXULONG))
	{
		/* Ti */
		vtime = svtime(pTimeouts->ReadIntervalTimeout);
	}

	/* TMAX */
	pTmaxTimeout = &tmaxTimeout;

	if ((pTimeouts->ReadIntervalTimeout == MAXULONG) &&
	    (pTimeouts->ReadTotalTimeoutMultiplier == MAXULONG))
	{
		/* Tc */
		Tmax = pTimeouts->ReadTotalTimeoutConstant;
	}
	else
	{
		/* Tmax */
		Tmax = 1ll * nNumberOfBytesToRead * pTimeouts->ReadTotalTimeoutMultiplier +
		       1ll * pTimeouts->ReadTotalTimeoutConstant;

		/* INDEFinitely */
		if ((Tmax == 0) && (pTimeouts->ReadIntervalTimeout < MAXULONG) &&
		    (pTimeouts->ReadTotalTimeoutMultiplier == 0))
			pTmaxTimeout = NULL;
	}

	if ((currentTermios.c_cc[VMIN] != vmin) || (currentTermios.c_cc[VTIME] != vtime))
	{
		currentTermios.c_cc[VMIN] = vmin;
		currentTermios.c_cc[VTIME] = vtime;

		if (tcsetattr(pComm->fd, TCSANOW, &currentTermios) < 0)
		{
			CommLog_Print(WLOG_WARN,
			              "CommReadFile failure, could not apply new timeout values: VMIN=%" PRIu8
			              ", VTIME=%" PRIu8 "",
			              vmin, vtime);
			SetLastError(ERROR_IO_DEVICE);
			goto return_false;
		}
	}

	/* wait indefinitely if pTmaxTimeout is NULL */

	if (pTmaxTimeout != NULL)
	{
		ZeroMemory(pTmaxTimeout, sizeof(struct timeval));

		if (Tmax > 0) /* return immdiately if Tmax == 0 */
		{
			pTmaxTimeout->tv_sec = Tmax / 1000;           /* s */
			pTmaxTimeout->tv_usec = (Tmax % 1000) * 1000; /* us */
		}
	}

	/* FIXME: had expected eventfd_write() to return EAGAIN when
	 * there is no eventfd_read() but this not the case. */
	/* discard a possible and no more relevant event */
#if defined(WINPR_HAVE_SYS_EVENTFD_H)
	{
		eventfd_t val = 0;
		(void)eventfd_read(pComm->fd_read_event, &val);
	}
#endif
	biggestFd = pComm->fd_read;

	if (pComm->fd_read_event > biggestFd)
		biggestFd = pComm->fd_read_event;

	FD_ZERO(&read_set);
	WINPR_ASSERT(pComm->fd_read_event < FD_SETSIZE);
	WINPR_ASSERT(pComm->fd_read < FD_SETSIZE);
	FD_SET(pComm->fd_read_event, &read_set);
	FD_SET(pComm->fd_read, &read_set);
	nbFds = select(biggestFd + 1, &read_set, NULL, NULL, pTmaxTimeout);

	if (nbFds < 0)
	{
		char ebuffer[256] = { 0 };
		CommLog_Print(WLOG_WARN, "select() failure, errno=[%d] %s\n", errno,
		              winpr_strerror(errno, ebuffer, sizeof(ebuffer)));
		SetLastError(ERROR_IO_DEVICE);
		goto return_false;
	}

	if (nbFds == 0)
	{
		/* timeout */
		SetLastError(ERROR_TIMEOUT);
		goto return_false;
	}

	/* read_set */

	if (FD_ISSET(pComm->fd_read_event, &read_set))
	{
#if defined(WINPR_HAVE_SYS_EVENTFD_H)
		eventfd_t event = 0;

		if (eventfd_read(pComm->fd_read_event, &event) < 0)
		{
			if (errno == EAGAIN)
			{
				WINPR_ASSERT(FALSE); /* not quite sure this should ever happen */
				                     /* keep on */
			}
			else
			{
				char ebuffer[256] = { 0 };
				CommLog_Print(WLOG_WARN,
				              "unexpected error on reading fd_read_event, errno=[%d] %s\n", errno,
				              winpr_strerror(errno, ebuffer, sizeof(ebuffer)));
				/* FIXME: goto return_false ? */
			}

			WINPR_ASSERT(errno == EAGAIN);
		}

		if (event == WINPR_PURGE_RXABORT)
		{
			SetLastError(ERROR_CANCELLED);
			goto return_false;
		}

		WINPR_ASSERT(event == WINPR_PURGE_RXABORT); /* no other expected event so far */
#endif
	}

	if (FD_ISSET(pComm->fd_read, &read_set))
	{
		ssize_t nbRead = read(pComm->fd_read, lpBuffer, nNumberOfBytesToRead);

		if ((nbRead < 0) || (nbRead > nNumberOfBytesToRead))
		{
			char ebuffer[256] = { 0 };
			CommLog_Print(WLOG_WARN,
			              "CommReadFile failed, ReadIntervalTimeout=%" PRIu32
			              ", ReadTotalTimeoutMultiplier=%" PRIu32
			              ", ReadTotalTimeoutConstant=%" PRIu32 " VMIN=%u, VTIME=%u",
			              pTimeouts->ReadIntervalTimeout, pTimeouts->ReadTotalTimeoutMultiplier,
			              pTimeouts->ReadTotalTimeoutConstant, currentTermios.c_cc[VMIN],
			              currentTermios.c_cc[VTIME]);
			CommLog_Print(
			    WLOG_WARN, "CommReadFile failed, nNumberOfBytesToRead=%" PRIu32 ", errno=[%d] %s",
			    nNumberOfBytesToRead, errno, winpr_strerror(errno, ebuffer, sizeof(ebuffer)));

			if (errno == EAGAIN)
			{
				/* keep on */
				goto return_true; /* expect a read-loop to be implemented on the server side */
			}
			else if (errno == EBADF)
			{
				SetLastError(ERROR_BAD_DEVICE); /* STATUS_INVALID_DEVICE_REQUEST */
				goto return_false;
			}
			else
			{
				WINPR_ASSERT(FALSE);
				SetLastError(ERROR_IO_DEVICE);
				goto return_false;
			}
		}

		if (nbRead == 0)
		{
			/* termios timeout */
			SetLastError(ERROR_TIMEOUT);
			goto return_false;
		}

		*lpNumberOfBytesRead = WINPR_ASSERTING_INT_CAST(UINT32, nbRead);

		EnterCriticalSection(&pComm->EventsLock);
		if (pComm->PendingEvents & SERIAL_EV_WINPR_WAITING)
		{
			if (pComm->eventChar != '\0' &&
			    memchr(lpBuffer, pComm->eventChar, WINPR_ASSERTING_INT_CAST(size_t, nbRead)))
				pComm->PendingEvents |= SERIAL_EV_RXCHAR;
		}
		LeaveCriticalSection(&pComm->EventsLock);
		goto return_true;
	}

	WINPR_ASSERT(FALSE);
	*lpNumberOfBytesRead = 0;
return_false:
	LeaveCriticalSection(&pComm->ReadLock);
	return FALSE;
return_true:
	LeaveCriticalSection(&pComm->ReadLock);
	return TRUE;
}

/**
 * ERRORS:
 *   ERROR_INVALID_HANDLE
 *   ERROR_NOT_SUPPORTED
 *   ERROR_INVALID_PARAMETER
 *   ERROR_BAD_DEVICE
 */
BOOL CommWriteFile(HANDLE hDevice, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
                   LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped)
{
	WINPR_COMM* pComm = (WINPR_COMM*)hDevice;
	struct timeval tmaxTimeout;
	struct timeval* pTmaxTimeout = NULL;
	EnterCriticalSection(&pComm->WriteLock); /* KISSer by the function's beginning */

	if (!CommIsHandled(hDevice))
		goto return_false;

	if (lpOverlapped != NULL)
	{
		SetLastError(ERROR_NOT_SUPPORTED);
		goto return_false;
	}

	if (lpNumberOfBytesWritten == NULL)
	{
		SetLastError(ERROR_INVALID_PARAMETER); /* since we doesn't support lpOverlapped != NULL */
		goto return_false;
	}

	*lpNumberOfBytesWritten = 0; /* will be adjusted if required ... */

	if (nNumberOfBytesToWrite <= 0)
	{
		goto return_true; /* FIXME: or FALSE? */
	}

	/* FIXME: had expected eventfd_write() to return EAGAIN when
	 * there is no eventfd_read() but this not the case. */
	/* discard a possible and no more relevant event */

#if defined(WINPR_HAVE_SYS_EVENTFD_H)
	{
		eventfd_t val = 0;
		(void)eventfd_read(pComm->fd_write_event, &val);
	}
#endif

	/* ms */
	LONGLONG Tmax = 1ll * nNumberOfBytesToWrite * pComm->timeouts.WriteTotalTimeoutMultiplier +
	                1ll * pComm->timeouts.WriteTotalTimeoutConstant;
	/* NB: select() may update the timeout argument to indicate
	 * how much time was left. Keep the timeout variable out of
	 * the while() */
	pTmaxTimeout = &tmaxTimeout;
	ZeroMemory(pTmaxTimeout, sizeof(struct timeval));

	if (Tmax > 0)
	{
		pTmaxTimeout->tv_sec = Tmax / 1000;           /* s */
		pTmaxTimeout->tv_usec = (Tmax % 1000) * 1000; /* us */
	}
	else if ((pComm->timeouts.WriteTotalTimeoutMultiplier == 0) &&
	         (pComm->timeouts.WriteTotalTimeoutConstant == 0))
	{
		pTmaxTimeout = NULL;
	}

	/* else return immdiately */

	while (*lpNumberOfBytesWritten < nNumberOfBytesToWrite)
	{
		int biggestFd = -1;
		fd_set event_set;
		fd_set write_set;
		int nbFds = 0;
		biggestFd = pComm->fd_write;

		if (pComm->fd_write_event > biggestFd)
			biggestFd = pComm->fd_write_event;

		FD_ZERO(&event_set);
		FD_ZERO(&write_set);
		WINPR_ASSERT(pComm->fd_write_event < FD_SETSIZE);
		WINPR_ASSERT(pComm->fd_write < FD_SETSIZE);
		FD_SET(pComm->fd_write_event, &event_set);
		FD_SET(pComm->fd_write, &write_set);
		nbFds = select(biggestFd + 1, &event_set, &write_set, NULL, pTmaxTimeout);

		if (nbFds < 0)
		{
			char ebuffer[256] = { 0 };
			CommLog_Print(WLOG_WARN, "select() failure, errno=[%d] %s\n", errno,
			              winpr_strerror(errno, ebuffer, sizeof(ebuffer)));
			SetLastError(ERROR_IO_DEVICE);
			goto return_false;
		}

		if (nbFds == 0)
		{
			/* timeout */
			SetLastError(ERROR_TIMEOUT);
			goto return_false;
		}

		/* event_set */

		if (FD_ISSET(pComm->fd_write_event, &event_set))
		{
#if defined(WINPR_HAVE_SYS_EVENTFD_H)
			eventfd_t event = 0;

			if (eventfd_read(pComm->fd_write_event, &event) < 0)
			{
				if (errno == EAGAIN)
				{
					WINPR_ASSERT(FALSE); /* not quite sure this should ever happen */
					                     /* keep on */
				}
				else
				{
					char ebuffer[256] = { 0 };
					CommLog_Print(WLOG_WARN,
					              "unexpected error on reading fd_write_event, errno=[%d] %s\n",
					              errno, winpr_strerror(errno, ebuffer, sizeof(ebuffer)));
					/* FIXME: goto return_false ? */
				}

				WINPR_ASSERT(errno == EAGAIN);
			}

			if (event == WINPR_PURGE_TXABORT)
			{
				SetLastError(ERROR_CANCELLED);
				goto return_false;
			}

			WINPR_ASSERT(event == WINPR_PURGE_TXABORT); /* no other expected event so far */
#endif
		}

		/* write_set */

		if (FD_ISSET(pComm->fd_write, &write_set))
		{
			ssize_t nbWritten = 0;
			const BYTE* ptr = lpBuffer;
			nbWritten = write(pComm->fd_write, &ptr[*lpNumberOfBytesWritten],
			                  nNumberOfBytesToWrite - (*lpNumberOfBytesWritten));

			if (nbWritten < 0)
			{
				char ebuffer[256] = { 0 };
				CommLog_Print(WLOG_WARN,
				              "CommWriteFile failed after %" PRIu32
				              " bytes written, errno=[%d] %s\n",
				              *lpNumberOfBytesWritten, errno,
				              winpr_strerror(errno, ebuffer, sizeof(ebuffer)));

				if (errno == EAGAIN)
				{
					/* keep on */
					continue;
				}
				else if (errno == EBADF)
				{
					SetLastError(ERROR_BAD_DEVICE); /* STATUS_INVALID_DEVICE_REQUEST */
					goto return_false;
				}
				else
				{
					WINPR_ASSERT(FALSE);
					SetLastError(ERROR_IO_DEVICE);
					goto return_false;
				}
			}

			*lpNumberOfBytesWritten += nbWritten;
		}
	} /* while */

	/* FIXME: this call to tcdrain() doesn't look correct and
	 * might hide a bug but was required while testing a serial
	 * printer. Its driver was expecting the modem line status
	 * SERIAL_MSR_DSR true after the sending which was never
	 * happening otherwise. A purge was also done before each
	 * Write operation. The serial port was opened with:
	 * DesiredAccess=0x0012019F. The printer worked fine with
	 * mstsc. */
	tcdrain(pComm->fd_write);

return_true:
	LeaveCriticalSection(&pComm->WriteLock);
	return TRUE;

return_false:
	LeaveCriticalSection(&pComm->WriteLock);
	return FALSE;
}
