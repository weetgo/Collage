
/* Copyright (c) 2005-2012, Stefan Eilemann <eile@equalizergraphics.com>
 *
 * This file is part of Collage <https://github.com/Eyescale/Collage>
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License version 2.1 as published
 * by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "fdConnection.h"

#include "connectionDescription.h"
#include "exception.h"
#include "global.h"
#include "log.h"

#include <lunchbox/os.h>

#include <errno.h>
#include <poll.h>

namespace co
{
FDConnection::FDConnection()
    : _readFD(0)
    , _writeFD(0)
{
}

int FDConnection::_getTimeOut()
{
    const uint32_t timeout = Global::getTimeout();
    return timeout == LB_TIMEOUT_INDEFINITE ? -1 : int(timeout);
}

//----------------------------------------------------------------------
// read
//----------------------------------------------------------------------
int64_t FDConnection::readSync(void* buffer, const uint64_t bytes, const bool)
{
    if (_readFD < 1)
        return -1;

    ssize_t bytesRead = ::read(_readFD, buffer, bytes);
    if (bytesRead > 0)
        return bytesRead;

    if (bytesRead == 0 || errno == EWOULDBLOCK || errno == EAGAIN)
    {
        struct pollfd fds[1];
        fds[0].fd = _readFD;
        fds[0].events = POLLIN;

        const int res = poll(fds, 1, _getTimeOut());
        if (res < 0)
        {
            LBWARN << "Error during read : " << strerror(errno) << std::endl;
            return -1;
        }

        if (res == 0)
            throw Exception(Exception::TIMEOUT_READ);

        bytesRead = ::read(_readFD, buffer, bytes);
    }

    if (bytesRead > 0)
        return bytesRead;

    if (bytesRead == 0) // EOF
    {
        LBDEBUG << "Got EOF, closing " << getDescription()->toString()
                << std::endl;
        close();
        return -1;
    }

    LBASSERT(bytesRead == -1); // error

    if (errno == EINTR) // if interrupted, try again
        return 0;

    LBWARN << "Error during read: " << strerror(errno) << ", " << bytes
           << "b on fd " << _readFD << std::endl;
    return -1;
}

//----------------------------------------------------------------------
// write
//----------------------------------------------------------------------
int64_t FDConnection::write(const void* buffer, const uint64_t bytes)
{
    if (!isConnected() || _writeFD < 1)
        return -1;

    ssize_t bytesWritten = ::write(_writeFD, buffer, bytes);
    if (bytesWritten > 0)
        return bytesWritten;

    if (bytesWritten == 0 || errno == EWOULDBLOCK || errno == EAGAIN)
    {
        struct pollfd fds[1];
        fds[0].fd = _writeFD;
        fds[0].events = POLLOUT;
        const int res = poll(fds, 1, _getTimeOut());
        if (res < 0)
        {
            LBWARN << "Write error: " << lunchbox::sysError << std::endl;
            return -1;
        }

        if (res == 0)
            throw Exception(Exception::TIMEOUT_WRITE);

        bytesWritten = ::write(_writeFD, buffer, bytes);
    }

    if (bytesWritten > 0)
        return bytesWritten;

    if (bytesWritten == -1) // error
    {
        if (errno == EINTR) // if interrupted, try again
            return 0;

        LBWARN << "Error during write: " << lunchbox::sysError << std::endl;
        return -1;
    }

    return bytesWritten;
}
}
