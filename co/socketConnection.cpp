
/* Copyright (c) 2005-2014, Stefan Eilemann <eile@equalizergraphics.com>
 *                    2010, Daniel Nachbaur <danielnachbaur@gmail.com>
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

#include "socketConnection.h"

#include "connectionDescription.h"
#include "exception.h"
#include "global.h"

#include <co/exception.h>
#include <lunchbox/log.h>
#include <lunchbox/os.h>
#include <lunchbox/sleep.h>

#include <limits>
#include <sstream>
#include <string.h>
#include <sys/types.h>

#ifdef _WIN32
#include <mswsock.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#define CO_RECV_TIMEOUT 250 /*ms*/
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/errno.h>
#include <sys/socket.h>
#endif

namespace co
{
SocketConnection::SocketConnection()
#ifdef _WIN32
    : _overlappedAcceptData(0)
    , _overlappedSocket(INVALID_SOCKET)
    , _overlappedDone(0)
#endif
{
#ifdef _WIN32
    memset(&_overlappedRead, 0, sizeof(_overlappedRead));
    memset(&_overlappedWrite, 0, sizeof(_overlappedWrite));
#endif

    ConnectionDescriptionPtr description = _getDescription();
    description->type = CONNECTIONTYPE_TCPIP;
    description->bandwidth = 102400; // 100MB/s

    LBVERB << "New SocketConnection @" << (void*)this << std::endl;
}

SocketConnection::~SocketConnection()
{
    _close();
}

namespace
{
std::string getHostName(const sockaddr_in& address)
{
    char hostname[NI_MAXHOST];
    if (getnameinfo((sockaddr*)&address.sin_addr, sizeof(sockaddr_in), hostname,
                    sizeof(hostname), 0, 0, 0) == 0)
    {
        return std::string(hostname);
    }
    if (address.sin_addr.s_addr == htonl(INADDR_ANY))
        return "*";
    return std::string();
}

bool _parseAddress(ConstConnectionDescriptionPtr description,
                   sockaddr_in& address)
{
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(description->port);
    memset(&(address.sin_zero), 0, 8); // zero the rest

    const std::string& hostname = description->getHostname();
    if (!hostname.empty())
    {
        addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_flags = 0;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* info = 0;

        if (getaddrinfo(hostname.c_str(), NULL, &hints, &info) == 0)
        {
            address.sin_addr.s_addr =
                ((sockaddr_in*)(info->ai_addr))->sin_addr.s_addr;
            freeaddrinfo(info);
        }
        else
        {
            LBWARN << "Can't resolve host " << hostname << std::endl;
            return false;
        }
    }

    LBVERB << "Address " << getHostName(address) << ":"
           << ntohs(address.sin_port) << std::endl;
    return true;
}
}

//----------------------------------------------------------------------
// connect
//----------------------------------------------------------------------
bool SocketConnection::connect()
{
    ConnectionDescriptionPtr description = _getDescription();
    LBASSERT(description->type == CONNECTIONTYPE_TCPIP);
    if (!isClosed())
        return false;

    if (description->port == 0)
        return false;

    if (description->getHostname().empty())
        description->setHostname("127.0.0.1");

    sockaddr_in address;
    if (!_parseAddress(description, address))
    {
        LBWARN << "Can't parse connection parameters" << std::endl;
        return false;
    }

    _setState(STATE_CONNECTING);

    if (!_createSocket())
        return false;

    if (address.sin_addr.s_addr == 0)
    {
        LBWARN << "Refuse to connect to 0.0.0.0" << std::endl;
        close();
        return false;
    }

#ifdef _WIN32
    const bool connected = WSAConnect(_readFD, (sockaddr*)&address,
                                      sizeof(address), 0, 0, 0, 0) == 0;
#else
    int nTries = 10;
    while (nTries--)
    {
        const bool connected =
            (::connect(_readFD, (sockaddr*)&address, sizeof(address)) == 0);
        if (connected)
            break;

        switch (errno)
        {
        case EINTR: // Happens sometimes, but looks harmless
            LBDEBUG << "connect: " << lunchbox::sysError << ", retrying"
                    << std::endl;
            lunchbox::sleep(5 /*ms*/);
            break;

        default:
            nTries = 0;
            break;
        }
    }
    const bool connected = nTries > 0;
#endif

    if (!connected)
    {
        LBDEBUG << "Could not connect to '" << description->getHostname() << ":"
                << description->port << "': " << lunchbox::sysError
                << std::endl;
        close();
        return false;
    }

    _initAIORead();
    _setState(STATE_CONNECTED);
    LBDEBUG << "Connected " << description->toString() << std::endl;
    return true;
}

void SocketConnection::_close()
{
    if (isClosed())
        return;

    if (isListening())
        _exitAIOAccept();
    else if (isConnected())
        _exitAIORead();

    LBASSERT(_readFD > 0);

#ifdef _WIN32
    const bool closed = (::closesocket(_readFD) == 0);
#else
    const bool closed = (::close(_readFD) == 0);
#endif

    if (!closed)
        LBWARN << "Could not close socket: " << lunchbox::sysError << std::endl;

    _readFD = INVALID_SOCKET;
    _writeFD = INVALID_SOCKET;
    _setState(STATE_CLOSED);
}

//----------------------------------------------------------------------
// Async IO handles
//----------------------------------------------------------------------
#ifdef _WIN32
void SocketConnection::_initAIORead()
{
    _overlappedRead.hEvent = CreateEvent(0, FALSE, FALSE, 0);
    LBASSERT(_overlappedRead.hEvent);

    _overlappedWrite.hEvent = CreateEvent(0, FALSE, FALSE, 0);
    LBASSERT(_overlappedWrite.hEvent);
    if (!_overlappedRead.hEvent)
        LBERROR << "Can't create event for AIO notification: "
                << lunchbox::sysError << std::endl;
}

void SocketConnection::_initAIOAccept()
{
    _initAIORead();
    _overlappedAcceptData = malloc(2 * (sizeof(sockaddr_in) + 16));
}

void SocketConnection::_exitAIOAccept()
{
    if (_overlappedAcceptData)
    {
        free(_overlappedAcceptData);
        _overlappedAcceptData = 0;
    }

    _exitAIORead();
}
void SocketConnection::_exitAIORead()
{
    if (_overlappedRead.hEvent)
    {
        CloseHandle(_overlappedRead.hEvent);
        _overlappedRead.hEvent = 0;
    }

    if (_overlappedWrite.hEvent)
    {
        CloseHandle(_overlappedWrite.hEvent);
        _overlappedWrite.hEvent = 0;
    }
}
#else
void SocketConnection::_initAIOAccept()
{ /* NOP */
}
void SocketConnection::_exitAIOAccept()
{ /* NOP */
}
void SocketConnection::_initAIORead()
{ /* NOP */
}
void SocketConnection::_exitAIORead()
{ /* NOP */
}
#endif

//----------------------------------------------------------------------
// accept
//----------------------------------------------------------------------
#ifdef _WIN32
void SocketConnection::acceptNB()
{
    LBASSERT(isListening());

    // Create new accept socket
    const DWORD flags = WSA_FLAG_OVERLAPPED;

    LBASSERT(_overlappedAcceptData);
    LBASSERT(_overlappedSocket == INVALID_SOCKET);
    _overlappedSocket =
        WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, 0, 0, flags);

    if (_overlappedSocket == INVALID_SOCKET)
    {
        LBERROR << "Could not create accept socket: " << lunchbox::sysError
                << ", closing listening socket" << std::endl;
        close();
        return;
    }

    const int on = 1;
    setsockopt(_overlappedSocket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
               reinterpret_cast<const char*>(&on), sizeof(on));

    // Start accept
    ResetEvent(_overlappedRead.hEvent);
    DWORD got;
    if (!AcceptEx(_readFD, _overlappedSocket, _overlappedAcceptData, 0,
                  sizeof(sockaddr_in) + 16, sizeof(sockaddr_in) + 16, &got,
                  &_overlappedRead) &&
        GetLastError() != WSA_IO_PENDING)
    {
        LBERROR << "Could not start accept operation: " << lunchbox::sysError
                << ", closing connection" << std::endl;
        close();
    }
}

ConnectionPtr SocketConnection::acceptSync()
{
    LB_TS_THREAD(_recvThread);
    if (!isListening())
        return 0;

    LBASSERT(_overlappedAcceptData);
    LBASSERT(_overlappedSocket != INVALID_SOCKET);
    if (_overlappedSocket == INVALID_SOCKET)
        return 0;

    // complete accept
    DWORD got = 0;
    DWORD flags = 0;

    if (!WSAGetOverlappedResult(_readFD, &_overlappedRead, &got, TRUE, &flags))
    {
        LBWARN << "Accept completion failed: " << lunchbox::sysError
               << ", closing socket" << std::endl;
        close();
        return 0;
    }

    sockaddr_in* local = 0;
    sockaddr_in* remote = 0;
    int localLen = 0;
    int remoteLen = 0;
    GetAcceptExSockaddrs(_overlappedAcceptData, 0, sizeof(sockaddr_in) + 16,
                         sizeof(sockaddr_in) + 16, (sockaddr**)&local,
                         &localLen, (sockaddr**)&remote, &remoteLen);
    _tuneSocket(_overlappedSocket);

    ConstConnectionDescriptionPtr description = getDescription();
    SocketConnection* newConnection = new SocketConnection;
    ConnectionPtr connection(newConnection); // to keep ref-counting correct

    newConnection->_readFD = _overlappedSocket;
    newConnection->_writeFD = _overlappedSocket;

#ifndef _WIN32
// fcntl( _overlappedSocket, F_SETFL, O_NONBLOCK );
#endif

    newConnection->_initAIORead();
    _overlappedSocket = INVALID_SOCKET;

    newConnection->_setState(STATE_CONNECTED);
    ConnectionDescriptionPtr newDescription = newConnection->_getDescription();
    newDescription->bandwidth = description->bandwidth;
    newDescription->port = ntohs(remote->sin_port);
    newDescription->setHostname(getHostName(*remote));

    LBDEBUG << "accepted connection from " << newDescription->getHostname()
            << ":" << newDescription->port << std::endl;
    return connection;
}

#else // !_WIN32

void SocketConnection::acceptNB()
{ /* NOP */
}

ConnectionPtr SocketConnection::acceptSync()
{
    if (!isListening())
        return 0;

    sockaddr_in newAddress;
    socklen_t newAddressLen = sizeof(newAddress);

    Socket fd;
    unsigned nTries = 1000;
    do
        fd = ::accept(_readFD, (sockaddr*)&newAddress, &newAddressLen);
    while (fd == INVALID_SOCKET && errno == EINTR && --nTries);

    if (fd == INVALID_SOCKET)
    {
        LBWARN << "accept failed: " << lunchbox::sysError << std::endl;
        return 0;
    }

    _tuneSocket(fd);

    ConstConnectionDescriptionPtr description = getDescription();
    SocketConnection* newConnection = new SocketConnection;

    newConnection->_readFD = fd;
    newConnection->_writeFD = fd;
    newConnection->_setState(STATE_CONNECTED);
    ConnectionDescriptionPtr newDescription = newConnection->_getDescription();
    newDescription->bandwidth = description->bandwidth;
    newDescription->port = ntohs(newAddress.sin_port);
    newDescription->setHostname(inet_ntoa(newAddress.sin_addr));

    LBDEBUG << "Accepted " << newDescription->toString() << std::endl;
    return newConnection;
}

#endif // !_WIN32

#ifdef _WIN32
//----------------------------------------------------------------------
// read
//----------------------------------------------------------------------
void SocketConnection::readNB(void* buffer, const uint64_t bytes)
{
    if (isClosed())
        return;

    WSABUF wsaBuffer = {LB_MIN(bytes, 65535), reinterpret_cast<char*>(buffer)};
    DWORD flags = 0;

    ResetEvent(_overlappedRead.hEvent);
    _overlappedDone = 0;
    const int result = WSARecv(_readFD, &wsaBuffer, 1, &_overlappedDone, &flags,
                               &_overlappedRead, 0);
    if (result == 0) // got data already
    {
        if (_overlappedDone == 0) // socket closed
        {
            LBDEBUG << "Got EOF, closing connection" << std::endl;
            close();
        }
        SetEvent(_overlappedRead.hEvent);
    }
    else if (GetLastError() != WSA_IO_PENDING)
    {
        LBWARN << "Could not start overlapped receive: " << lunchbox::sysError
               << ", closing connection" << std::endl;
        close();
    }
}

int64_t SocketConnection::readSync(void* buffer, const uint64_t bytes,
                                   const bool block)
{
    LB_TS_THREAD(_recvThread);

    if (_readFD == INVALID_SOCKET)
    {
        LBERROR << "Invalid read handle" << std::endl;
        return READ_ERROR;
    }

    if (_overlappedDone > 0)
        return _overlappedDone;

    DWORD got = 0;
    DWORD flags = 0;
    DWORD startTime = 0;

    while (true)
    {
        if (WSAGetOverlappedResult(_readFD, &_overlappedRead, &got, block,
                                   &flags))
            return got;

        const int err = WSAGetLastError();
        if (err == ERROR_SUCCESS || got > 0)
        {
            LBWARN << "Got " << lunchbox::sysError << " with " << got
                   << " bytes on " << getDescription() << std::endl;
            return got;
        }

        if (startTime == 0)
            startTime = GetTickCount();

        switch (err)
        {
        case WSA_IO_INCOMPLETE:
            return READ_TIMEOUT;

        case WSASYSCALLFAILURE: // happens sometimes!?
        case WSA_IO_PENDING:
            if (GetTickCount() - startTime > CO_RECV_TIMEOUT) // timeout
            {
                LBWARN << "Error timeout " << std::endl;
                return READ_ERROR;
            }

            LBWARN << "WSAGetOverlappedResult error loop" << std::endl;
            // one millisecond to recover
            lunchbox::sleep(1);
            break;

        default:
            LBWARN << "Got " << lunchbox::sysError << ", closing connection"
                   << std::endl;
            close();
            return READ_ERROR;
        }
    }
}

int64_t SocketConnection::write(const void* buffer, const uint64_t bytes)
{
    if (!isConnected() || _writeFD == INVALID_SOCKET)
        return -1;

    DWORD wrote;
    WSABUF wsaBuffer = {LB_MIN(bytes, 65535),
                        const_cast<char*>(static_cast<const char*>(buffer))};

    ResetEvent(_overlappedWrite.hEvent);
    if (WSASend(_writeFD, &wsaBuffer, 1, &wrote, 0, &_overlappedWrite, 0) == 0)
        // ok
        return wrote;

    if (WSAGetLastError() != WSA_IO_PENDING)
        return -1;

    const DWORD err = WaitForSingleObject(_overlappedWrite.hEvent, INFINITE);
    switch (err)
    {
    case WAIT_FAILED:
    case WAIT_ABANDONED:
    {
        LBWARN << "Write error" << lunchbox::sysError << std::endl;
        return -1;
    }

    default:
        LBWARN << "Unhandled write error " << err << ": " << lunchbox::sysError
               << std::endl;
    // no break;
    case WAIT_OBJECT_0:
        break;
    }

    DWORD got = 0;
    DWORD flags = 0;
    if (WSAGetOverlappedResult(_writeFD, &_overlappedWrite, &got, false,
                               &flags))
    {
        return got;
    }

    switch (WSAGetLastError())
    {
    case WSA_IO_INCOMPLETE:
        throw Exception(Exception::TIMEOUT_WRITE);

    default:
    {
        LBWARN << "Write error : " << lunchbox::sysError << std::endl;
        return -1;
    }
    }

    LBUNREACHABLE;
    return -1;
}
#endif // _WIN32

bool SocketConnection::_createSocket()
{
    ConstConnectionDescriptionPtr description = getDescription();
#ifdef _WIN32
    const DWORD flags = WSA_FLAG_OVERLAPPED;
    const SOCKET fd =
        WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, 0, 0, flags);
#else
    const Socket fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#endif

    if (fd == INVALID_SOCKET)
    {
        LBERROR << "Could not create socket: " << lunchbox::sysError
                << std::endl;
        return false;
    }

    _tuneSocket(fd);

    _readFD = fd;
    _writeFD = fd; // TCP/IP sockets are bidirectional

    return true;
}

void SocketConnection::_tuneSocket(const Socket fd)
{
    const int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on),
               sizeof(on));
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on),
               sizeof(on));

#ifdef _WIN32
    const int size = 128768;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&size),
               sizeof(size));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&size),
               sizeof(size));
#endif
}

//----------------------------------------------------------------------
// listen
//----------------------------------------------------------------------
bool SocketConnection::listen()
{
    ConnectionDescriptionPtr description = _getDescription();
    LBASSERT(description->type == CONNECTIONTYPE_TCPIP);

    if (!isClosed())
        return false;

    _setState(STATE_CONNECTING);

    sockaddr_in address;
    const size_t size = sizeof(sockaddr_in);

    if (!_parseAddress(description, address))
    {
        LBWARN << "Can't parse connection parameters" << std::endl;
        return false;
    }

    if (!_createSocket())
        return false;

    const bool bound = (::bind(_readFD, (sockaddr*)&address, size) == 0);

    if (!bound)
    {
        LBWARN << "Could not bind socket " << _readFD << ": "
               << lunchbox::sysError << " to " << getHostName(address) << ":"
               << ntohs(address.sin_port) << " AF " << (int)address.sin_family
               << std::endl;

        close();
        return false;
    }

    if (::listen(_readFD, SOMAXCONN) != 0)
    {
        LBWARN << "Could not listen on socket: " << lunchbox::sysError
               << std::endl;
        close();
        return false;
    }

    // get socket parameters
    socklen_t used = size;
    getsockname(_readFD, (struct sockaddr*)&address, &used);
    description->port = ntohs(address.sin_port);

    std::string hostname = description->getHostname();
    if (hostname.empty())
    {
        if (address.sin_addr.s_addr == INADDR_ANY)
        {
            char cHostname[256] = {0};
            gethostname(cHostname, 256);
            hostname = cHostname;

            description->setHostname(hostname);
        }
        else
            description->setHostname(getHostName(address));
    }

    _initAIOAccept();
    _setState(STATE_LISTENING);

    LBDEBUG << "Listening on " << description->getHostname() << "["
            << getHostName(address) << "]:" << description->port << " ("
            << description->toString() << ")" << std::endl;

    return true;
}

uint16_t SocketConnection::_getPort() const
{
    sockaddr_in address;
    socklen_t used = sizeof(address);
    getsockname(_readFD, (struct sockaddr*)&address, &used);
    return ntohs(address.sin_port);
}
}
