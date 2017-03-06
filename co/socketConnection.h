
/* Copyright (c) 2005-2013, Stefan Eilemann <eile@equalizergraphics.com>
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

#ifndef CO_SOCKETCONNECTION_H
#define CO_SOCKETCONNECTION_H

#include <co/connectionType.h> // enum
#include <lunchbox/api.h>
#include <lunchbox/buffer.h> // member
#include <lunchbox/os.h>
#include <lunchbox/thread.h> // for LB_TS_VAR

#ifdef WIN32
#include <co/connection.h>
#else
#include "fdConnection.h"
#include <netinet/in.h>
#endif

namespace co
{
/** A socket connection (TCPIP). */
class SocketConnection
#ifdef WIN32
    : public Connection
#else
    : public FDConnection
#endif
{
public:
    /** Create a new TCP-based connection */
    SocketConnection();
    bool connect() override;
    bool listen() override;
    void acceptNB() override;
    ConnectionPtr acceptSync() override;
    void close() override { _close(); }
#ifdef WIN32
    /** @sa Connection::getNotifier */
    Notifier getNotifier() const override { return _overlappedRead.hEvent; }
#endif

protected:
    virtual ~SocketConnection();

#ifdef WIN32
    void readNB(void* buffer, const uint64_t bytes) override;
    int64_t readSync(void* buffer, const uint64_t bytes,
                     const bool block) override;
    int64_t write(const void* buffer, const uint64_t bytes) override;

    typedef UINT_PTR Socket;
#else
    //! @cond IGNORE
    typedef int Socket;
    enum
    {
        INVALID_SOCKET = -1
    };
//! @endcond
#endif

private:
    void _initAIOAccept();
    void _exitAIOAccept();
    void _initAIORead();
    void _exitAIORead();

    bool _createSocket();
    void _tuneSocket(const Socket fd);
    uint16_t _getPort() const;

#ifdef WIN32
    union {
        Socket _readFD;
        Socket _writeFD;
    };

    // overlapped data structures
    OVERLAPPED _overlappedRead;
    OVERLAPPED _overlappedWrite;
    void* _overlappedAcceptData;
    Socket _overlappedSocket;
    DWORD _overlappedDone;

    LB_TS_VAR(_recvThread);
#endif

    void _close();
};
}

#endif // CO_SOCKETCONNECTION_H
