
/* Copyright (c) 2007-2012, Stefan Eilemann <eile@equalizergraphics.com>
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

#include "bufferConnection.h"

#include <lunchbox/buffer.h>
#include <string.h>

namespace co
{
namespace detail
{
class BufferConnection
{
public:
    lunchbox::Bufferb buffer;
};
}

BufferConnection::BufferConnection()
    : _impl(new detail::BufferConnection)
{
    _setState(STATE_CONNECTED);
    LBVERB << "New BufferConnection @" << (void*)this << std::endl;
}

BufferConnection::~BufferConnection()
{
    _setState(STATE_CLOSED);
    if (!_impl->buffer.isEmpty())
        LBWARN << "Deleting BufferConnection with buffered data" << std::endl;
    delete _impl;
}

const lunchbox::Bufferb& BufferConnection::getBuffer() const
{
    return _impl->buffer;
}

lunchbox::Bufferb& BufferConnection::getBuffer()
{
    return _impl->buffer;
}

uint64_t BufferConnection::getSize() const
{
    return _impl->buffer.getSize();
}

int64_t BufferConnection::write(const void* buffer, const uint64_t bytes)
{
    _impl->buffer.append(reinterpret_cast<const uint8_t*>(buffer), bytes);
    return bytes;
}

void BufferConnection::sendBuffer(ConnectionPtr connection)
{
    if (_impl->buffer.isEmpty())
        return;

    if (!connection)
    {
        LBWARN << "NULL connection during buffer write" << std::endl;
        return;
    }

    LBCHECK(connection->send(_impl->buffer.getData(), _impl->buffer.getSize()));
    _impl->buffer.setSize(0);
}
}
