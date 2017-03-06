
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

#ifndef CO_QUEUECOMMAND_H
#define CO_QUEUECOMMAND_H

#include <co/commands.h>

namespace co
{
enum QueueCommand
{
    CMD_QUEUE_GET_ITEM = CMD_OBJECT_CUSTOM, // 10
    CMD_QUEUE_EMPTY,
    CMD_QUEUE_ITEM,
    CMD_QUEUE_CUSTOM = 15 //!< Commands for subclasses of queues start here
};
}

#endif // CO_QUEUECOMMAND_H
