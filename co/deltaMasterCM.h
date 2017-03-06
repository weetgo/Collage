
/* Copyright (c) 2007-2013, Stefan Eilemann <eile@equalizergraphics.com>
 *                    2010, Cedric Stalder <cedric.stalder@gmail.com>
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

#ifndef CO_DELTAMASTERCM_H
#define CO_DELTAMASTERCM_H

#include "fullMasterCM.h" // base class
#include "objectDeltaDataOStream.h"

namespace co
{
/**
 * An object change manager handling full versions and deltas for the master
 * instance.
 * @internal
 */
class DeltaMasterCM : public FullMasterCM
{
public:
    explicit DeltaMasterCM(Object* object);
    virtual ~DeltaMasterCM();

protected:
    void _commit() override;

private:
    /* The command handlers. */
    bool _cmdCommit(ICommand& pkg);

    typedef ObjectDeltaDataOStream DeltaData;
    DeltaData _deltaData;
};
}

#endif // CO_DELTAMASTERCM_H
