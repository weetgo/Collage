
/* Copyright (c) 2007-2014, Stefan Eilemann <eile@equalizergraphics.com>
 *               2011-2012, Daniel Nachbaur <danielnachbaur@gmail.com>
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

#include "staticSlaveCM.h"

#include "log.h"
#include "object.h"
#include "objectCommand.h"
#include "objectDataICommand.h"
#include "objectDataIStream.h"

#include <lunchbox/scopedMutex.h>

namespace co
{
typedef CommandFunc<StaticSlaveCM> CmdFunc;

StaticSlaveCM::StaticSlaveCM(Object* object)
    : ObjectCM(object)
    , _currentIStream(new ObjectDataIStream)
{
    LBASSERT(_object);
    LBASSERT(object->getLocalNode());

    object->registerCommand(CMD_OBJECT_INSTANCE,
                            CmdFunc(this, &StaticSlaveCM::_cmdInstance), 0);
}

StaticSlaveCM::~StaticSlaveCM()
{
    delete _currentIStream;
    _currentIStream = 0;
}

void StaticSlaveCM::applyMapData(const uint128_t& version LB_UNUSED)
{
    LBASSERT(_currentIStream);
    LBASSERTINFO(version == VERSION_FIRST || version == VERSION_NONE, version);
    _currentIStream->waitReady();

    LBASSERT(_object);
    LBASSERTINFO(_currentIStream->getVersion() == VERSION_FIRST ||
                     _currentIStream->getVersion() == VERSION_NONE,
                 _currentIStream->getVersion());
    LBASSERT(_currentIStream->hasInstanceData());

    if (_currentIStream->hasData()) // not VERSION_NONE
        _object->applyInstanceData(*_currentIStream);

    LBASSERTINFO(!_currentIStream->hasData(),
                 "Object " << typeid(*_object).name()
                           << " did not unpack all data");

    delete _currentIStream;
    _currentIStream = 0;

    LBLOG(LOG_OBJECTS) << "Mapped initial data for " << _object->getID() << "."
                       << _object->getInstanceID() << " ready" << std::endl;
}

void StaticSlaveCM::addInstanceDatas(const ObjectDataIStreamDeque& cache,
                                     const uint128_t& /* start */)
{
    LB_TS_THREAD(_rcvThread);
    LBASSERT(_currentIStream);
    LBASSERT(_currentIStream->getDataSize() == 0);
    LBASSERT(cache.size() == 1);
    if (cache.empty())
        return;

    ObjectDataIStream* stream = cache.front();
    LBASSERT(stream);
    LBASSERT(stream->isReady());
    LBASSERT(stream->getVersion() == VERSION_FIRST);

    if (!stream->isReady() || stream->getVersion() != VERSION_FIRST)
        return;

    LBLOG(LOG_OBJECTS) << "Adding cached instance data" << std::endl;
    delete _currentIStream;
    _currentIStream = new ObjectDataIStream(*stream);
}

//---------------------------------------------------------------------------
// command handlers
//---------------------------------------------------------------------------
bool StaticSlaveCM::_cmdInstance(ICommand& command)
{
    LB_TS_THREAD(_rcvThread);
    LBASSERT(_currentIStream);
    _currentIStream->addDataCommand(command);

    if (_currentIStream->isReady())
        LBLOG(LOG_OBJECTS) << "id " << _object->getID() << "."
                           << _object->getInstanceID() << " ready" << std::endl;

    return true;
}
}
