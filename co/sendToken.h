
/* Copyright (c) 2013-2014, Stefan.Eilemann@epfl.ch
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

#ifndef CO_SENDTOKEN_H
#define CO_SENDTOKEN_H

#include <co/api.h>
#include <co/types.h>

#include <boost/noncopyable.hpp>
#include <lunchbox/referenced.h> // base class

namespace co
{
/** @internal */
class SendToken : public lunchbox::Referenced, public boost::noncopyable
{
public:
    ~SendToken() { release(); }
    CO_API void release();

private:
    explicit SendToken(NodePtr node)
        : node_(node)
    {
    }
    NodePtr node_;
    friend class LocalNode;
};
}

#endif // CO_SENDTOKEN_H
