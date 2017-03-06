
/* Copyright (c) 2005-2017, Stefan Eilemann <eile@equalizergraphics.com>
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

#ifndef CO_NODECOMMAND_H
#define CO_NODECOMMAND_H

#include <co/commands.h>

namespace co
{
enum NodeCommand
{
    CMD_NODE_CONNECT,
    CMD_NODE_CONNECT_REPLY,
    CMD_NODE_ID,
    CMD_NODE_STOP_RCV,
    CMD_NODE_STOP_CMD,
    CMD_NODE_SET_AFFINITY_RCV,
    CMD_NODE_SET_AFFINITY_CMD,
    CMD_NODE_MESSAGE,
    CMD_NODE_CONNECT_ACK,
    CMD_NODE_DISCONNECT,
    CMD_NODE_GET_NODE_DATA,
    CMD_NODE_GET_NODE_DATA_REPLY,
    CMD_NODE_ACQUIRE_SEND_TOKEN,
    CMD_NODE_ACQUIRE_SEND_TOKEN_REPLY,
    CMD_NODE_RELEASE_SEND_TOKEN,
    CMD_NODE_ADD_LISTENER,
    CMD_NODE_REMOVE_LISTENER,
    CMD_NODE_ACK_REQUEST,
    CMD_NODE_FIND_MASTER_NODE_ID,
    CMD_NODE_FIND_MASTER_NODE_ID_REPLY,
    CMD_NODE_ATTACH_OBJECT,
    CMD_NODE_DETACH_OBJECT,
    CMD_NODE_REGISTER_OBJECT,
    CMD_NODE_DEREGISTER_OBJECT,
    CMD_NODE_MAP_OBJECT,
    CMD_NODE_MAP_OBJECT_SUCCESS,
    CMD_NODE_MAP_OBJECT_REPLY,
    CMD_NODE_UNMAP_OBJECT,
    CMD_NODE_UNSUBSCRIBE_OBJECT,
    CMD_NODE_OBJECT_INSTANCE,
    CMD_NODE_OBJECT_INSTANCE_MAP,
    CMD_NODE_OBJECT_INSTANCE_COMMIT,
    CMD_NODE_OBJECT_INSTANCE_PUSH,
    CMD_NODE_OBJECT_INSTANCE_SYNC,
    CMD_NODE_DISABLE_SEND_ON_REGISTER,
    CMD_NODE_REMOVE_NODE,
    CMD_NODE_OBJECT_PUSH,
    CMD_NODE_COMMAND,
    CMD_NODE_PING,
    CMD_NODE_PING_REPLY,
    CMD_NODE_ADD_CONNECTION,
    CMD_NODE_SYNC_OBJECT,
    CMD_NODE_SYNC_OBJECT_REPLY
    // check that not more than CMD_NODE_CUSTOM have been defined!
};
}

#endif // CO_NODECOMMAND_H
