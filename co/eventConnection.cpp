
/* Copyright (c) 2010-2016, Stefan Eilemann <eile@equalizergraphics.com>
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

#include "eventConnection.h"
#include "pipeConnection.h"

#include <lunchbox/scopedMutex.h>

namespace co
{
EventConnection::EventConnection()
#ifdef _WIN32
    : _event(CreateEvent(0, TRUE, FALSE, 0))

#else
    : _connection(new PipeConnection)
    , _set(false)
#endif
{
}

EventConnection::~EventConnection()
{
    _close();

#ifdef _WIN32
    if (_event)
        CloseHandle(_event);
#endif
}

bool EventConnection::connect()
{
    if (!isClosed())
        return false;

    _setState(STATE_CONNECTING);

#ifndef _WIN32
    LBCHECK(_connection->connect());
    _set = false;
#endif

    _setState(STATE_CONNECTED);
    return true;
}

void EventConnection::_close()
{
#ifndef _WIN32
    _connection->close();
#endif
    _setState(STATE_CLOSED);
}

void EventConnection::set()
{
#ifdef _WIN32
    SetEvent(_event);
#else
    lunchbox::ScopedWrite mutex(_lock);
    if (_set)
        return;

    const char c = 42;
    _connection->acceptSync()->send(&c, 1, true);
    _set = true;
#endif
}
void EventConnection::reset()
{
#ifdef _WIN32
    ResetEvent(_event);
#else
    lunchbox::ScopedWrite mutex(_lock);
    if (!_set)
        return;

    _buffer.setSize(0);
    _connection->recvNB(&_buffer, 1);

    BufferPtr buffer;
    _connection->recvSync(buffer);
    _set = false;
#endif
}

Connection::Notifier EventConnection::getNotifier() const
{
#ifdef _WIN32
    return _event;
#else
    return _connection->getNotifier();
#endif
}
}
