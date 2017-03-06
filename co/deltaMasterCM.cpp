
/* Copyright (c) 2007-2012, Stefan Eilemann <eile@equalizergraphics.com>
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

#include "deltaMasterCM.h"

#include "log.h"
#include "node.h"
#include "object.h"
#include "objectDataIStream.h"

namespace co
{
DeltaMasterCM::DeltaMasterCM(Object* object)
    : FullMasterCM(object)
#pragma warning(push)
#pragma warning(disable : 4355)
    , _deltaData(this)
#pragma warning(pop)
{
}

DeltaMasterCM::~DeltaMasterCM()
{
}

void DeltaMasterCM::_commit()
{
    if (!_slaves->empty())
    {
        _deltaData.reset();
        _deltaData.enableCommit(_version + 1, *_slaves);
        _object->pack(_deltaData);
        _deltaData.disable();
    }

    if (_slaves->empty() || _deltaData.hasSentData())
    {
        // save instance data
        InstanceData* instanceData = _newInstanceData();

        instanceData->os.enableCommit(_version + 1, Nodes());
        _object->getInstanceData(instanceData->os);
        instanceData->os.disable();

        if (_deltaData.hasSentData() || instanceData->os.hasSentData())
        {
            ++_version;
            LBASSERT(_version != VERSION_NONE);

            _addInstanceData(instanceData);
        }
        else
            _releaseInstanceData(instanceData);

#if 0
        LBLOG( LOG_OBJECTS ) << "Committed v" << _version << " " << *_object
                             << std::endl;
#endif
    }
}
}
