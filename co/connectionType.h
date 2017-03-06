
/* Copyright (c) 2007-2016, Stefan Eilemann <eile@equalizergraphics.com>
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

#ifndef CO_CONNECTIONTYPE_H
#define CO_CONNECTIONTYPE_H

#include <iostream>
#include <lunchbox/debug.h>

namespace co
{
/** The supported network protocols. */
enum ConnectionType
{
    CONNECTIONTYPE_NONE = 0,
    CONNECTIONTYPE_TCPIP,     //!< TCP/IP sockets
    CONNECTIONTYPE_PIPE,      //!< pipe() based uni-directional connection
    CONNECTIONTYPE_NAMEDPIPE, //!< Named pipe based bidirectional connection
    CONNECTIONTYPE_IB,        //!< @deprecated Win XP Infiniband RDMA
    CONNECTIONTYPE_RDMA,      //!< Infiniband RDMA CM
    CONNECTIONTYPE_UDT,       //!< UDT connection
    CONNECTIONTYPE_MULTICAST = 0x100, //!< @internal MC types after this:
    CONNECTIONTYPE_RSP                //!< UDP-based reliable stream protocol
};

/** @internal */
inline std::ostream& operator<<(std::ostream& os, const ConnectionType& type)
{
    switch (type)
    {
    case CONNECTIONTYPE_TCPIP:
        return os << "TCPIP";
    case CONNECTIONTYPE_PIPE:
        return os << "ANON_PIPE";
    case CONNECTIONTYPE_NAMEDPIPE:
        return os << "PIPE";
    case CONNECTIONTYPE_RSP:
        return os << "RSP";
    case CONNECTIONTYPE_NONE:
        return os << "NONE";
    case CONNECTIONTYPE_RDMA:
        return os << "RDMA";
    case CONNECTIONTYPE_UDT:
        return os << "UDT";

    default:
        LBASSERTINFO(false, "Not implemented");
        return os << "ERROR";
    }
    return os;
}
}

#endif // CO_CONNECTIONTYPE_H
