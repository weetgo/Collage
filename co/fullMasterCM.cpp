
/* Copyright (c) 2007-2016, Stefan Eilemann <eile@equalizergraphics.com>
 *                          Daniel Nachbaur <danielnachbaur@gmail.com>
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

#include "fullMasterCM.h"

#include "log.h"
#include "node.h"
#include "nodeCommand.h"
#include "oCommand.h"
#include "object.h"
#include "objectDataIStream.h"

//#define CO_INSTRUMENT

namespace co
{
namespace
{
#ifdef CO_INSTRUMENT
lunchbox::a_int32_t _bytesBuffered;
#endif
}

FullMasterCM::FullMasterCM(Object* object)
    : VersionedMasterCM(object)
    , _commitCount(0)
    , _nVersions(0)
{
}

FullMasterCM::~FullMasterCM()
{
    for (InstanceDataDeque::const_iterator i = _instanceDatas.begin();
         i != _instanceDatas.end(); ++i)
    {
        delete *i;
    }
    _instanceDatas.clear();

    for (InstanceDatas::const_iterator i = _instanceDataCache.begin();
         i != _instanceDataCache.end(); ++i)
    {
        delete *i;
    }
    _instanceDataCache.clear();
}

void FullMasterCM::sendInstanceData(const Nodes& nodes)
{
    LB_TS_THREAD(_cmdThread);
    Mutex mutex(_slaves);
    if (!_slaves->empty())
        return;

    InstanceData* data = _instanceDatas.back();
    data->os.sendInstanceData(nodes);
}

void FullMasterCM::init()
{
    LBASSERT(_commitCount == 0);
    VersionedMasterCM::init();

    InstanceData* data = _newInstanceData();

    data->os.enableCommit(VERSION_FIRST, *_slaves);
    _object->getInstanceData(data->os);
    data->os.disable();

    _instanceDatas.push_back(data);
    ++_version;
    ++_commitCount;
}

void FullMasterCM::setAutoObsolete(const uint32_t count)
{
    Mutex mutex(_slaves);
    _nVersions = count;
    _obsolete();
}

void FullMasterCM::_updateCommitCount(const uint32_t incarnation)
{
    LBASSERT(!_instanceDatas.empty());
    if (incarnation == CO_COMMIT_NEXT)
    {
        ++_commitCount;
        return;
    }

    if (incarnation >= _commitCount)
    {
        _commitCount = incarnation;
        return;
    }

    LBASSERTINFO(incarnation >= _commitCount,
                 "Detected decreasing commit incarnation counter");
    _commitCount = incarnation;

    // obsolete 'future' old packages
    while (_instanceDatas.size() > 1)
    {
        InstanceData* data = _instanceDatas.back();
        if (data->commitCount <= _commitCount)
            break;

#ifdef CO_INSTRUMENT
        _bytesBuffered -= data->os.getSaveBuffer().getSize();
        LBINFO << _bytesBuffered << " bytes used" << std::endl;
#endif
        _releaseInstanceData(data);
        _instanceDatas.pop_back();
    }

    InstanceData* data = _instanceDatas.back();
    if (data->commitCount > _commitCount)
    {
        // tweak commitCount of minimum retained version for correct obsoletion
        data->commitCount = 0;
        _version = data->os.getVersion();
    }
}

void FullMasterCM::_obsolete()
{
    LBASSERT(!_instanceDatas.empty());
    while (_instanceDatas.size() > 1 && _commitCount > _nVersions)
    {
        InstanceData* data = _instanceDatas.front();
        if (data->commitCount >= (_commitCount - _nVersions))
            break;

#ifdef CO_INSTRUMENT
        _bytesBuffered -= data->os.getSaveBuffer().getSize();
        LBINFO << _bytesBuffered << " bytes used" << std::endl;
#endif
#if 0
        LBINFO
            << "Remove v" << data->os.getVersion() << " c" << data->commitCount
            << "@" << _commitCount << "/" << _nVersions << " from "
            << lunchbox::className( _object ) << " " << ObjectVersion( _object )
            << std::endl;
#endif
        _releaseInstanceData(data);
        _instanceDatas.pop_front();
    }
    _checkConsistency();
}

bool FullMasterCM::_initSlave(const MasterCMCommand& command,
                              const uint128_t& /*replyVersion*/,
                              bool replyUseCache)
{
    _checkConsistency();

    const uint128_t& version = command.getRequestedVersion();
    const uint128_t oldest = _instanceDatas.front()->os.getVersion();
    uint128_t start =
        (version == VERSION_OLDEST || version < oldest) ? oldest : version;
    uint128_t end = _version;

#ifndef NDEBUG
    if (version != VERSION_OLDEST && version < start)
        LBINFO << "Mapping version " << start << " instead of requested "
               << version << " for " << lunchbox::className(_object) << " "
               << ObjectVersion(_object->getID(), _version) << " of "
               << _instanceDatas.size() << "/" << _nVersions << std::endl;
#endif

    const uint128_t& minCachedVersion = command.getMinCachedVersion();
    const uint128_t& maxCachedVersion = command.getMaxCachedVersion();
    const uint128_t replyVersion = start;
    if (replyUseCache)
    {
        if (minCachedVersion <= start && maxCachedVersion >= start)
        {
#ifdef CO_INSTRUMENT_MULTICAST
            _hit += maxCachedVersion + 1 - start;
#endif
            start = maxCachedVersion + 1;
        }
        else if (maxCachedVersion == end)
        {
            end = LB_MAX(start, minCachedVersion - 1);
#ifdef CO_INSTRUMENT_MULTICAST
            _hit += _version - end;
#endif
        }
        // TODO else cached block in the middle, send head and tail elements
    }

#if 0
    LBLOG( LOG_OBJECTS )
        << *_object << ", instantiate on " << node->getNodeID() << " with v"
        << ((requested == VERSION_OLDEST) ? oldest : requested) << " ("
        << requested << ") sending " << start << ".." << end << " have "
        << _version - _nVersions << ".." << _version << " "
        << _instanceDatas.size() << std::endl;
#endif
    LBASSERT(start >= oldest);

    bool dataSent = false;

    // send all instance datas from start..end
    InstanceDataDeque::iterator i = _instanceDatas.begin();
    while (i != _instanceDatas.end() && (*i)->os.getVersion() < start)
        ++i;

    for (; i != _instanceDatas.end() && (*i)->os.getVersion() <= end; ++i)
    {
        if (!dataSent)
        {
            _sendMapSuccess(command, true);
            dataSent = true;
        }

        InstanceData* data = *i;
        LBASSERT(data);
        data->os.sendMapData(command.getRemoteNode(), command.getInstanceID());

#ifdef CO_INSTRUMENT_MULTICAST
        ++_miss;
#endif
    }

    if (!dataSent)
    {
        _sendMapSuccess(command, false);
        _sendMapReply(command, replyVersion, true, replyUseCache, false);
    }
    else
        _sendMapReply(command, replyVersion, true, replyUseCache, true);

#ifdef CO_INSTRUMENT_MULTICAST
    if (_miss % 100 == 0)
        LBINFO << "Cached " << _hit << "/" << _hit + _miss
               << " instance data transmissions" << std::endl;
#endif
    return true;
}

void FullMasterCM::_checkConsistency() const
{
#ifndef NDEBUG
    LBASSERT(!_instanceDatas.empty());
    LBASSERT(_object->isAttached());

    if (_version == VERSION_NONE)
        return;

    uint128_t version = _version;
    for (InstanceDataDeque::const_reverse_iterator i = _instanceDatas.rbegin();
         i != _instanceDatas.rend(); ++i)
    {
        const InstanceData* data = *i;
        LBASSERT(data->os.getVersion() != VERSION_NONE);
        LBASSERTINFO(data->os.getVersion() == version,
                     data->os.getVersion() << " != " << version);
        if (data != _instanceDatas.front())
        {
            LBASSERTINFO(data->commitCount + _nVersions >= _commitCount,
                         data->commitCount << ", " << _commitCount << " ["
                                           << _nVersions << "]");
        }
        --version;
    }
#endif
}

//---------------------------------------------------------------------------
// cache handling
//---------------------------------------------------------------------------
FullMasterCM::InstanceData* FullMasterCM::_newInstanceData()
{
    InstanceData* instanceData;

    if (_instanceDataCache.empty())
        instanceData = new InstanceData(this);
    else
    {
        instanceData = _instanceDataCache.back();
        _instanceDataCache.pop_back();
    }

    instanceData->commitCount = _commitCount;
    instanceData->os.reset();
    instanceData->os.enableSave();
    return instanceData;
}

void FullMasterCM::_addInstanceData(InstanceData* data)
{
    LBASSERT(data->os.getVersion() != VERSION_NONE);
    LBASSERT(data->os.getVersion() != VERSION_INVALID);

    _instanceDatas.push_back(data);
#ifdef CO_INSTRUMENT
    _bytesBuffered += data->os.getSaveBuffer().getSize();
    LBINFO << _bytesBuffered << " bytes used" << std::endl;
#endif
}

void FullMasterCM::_releaseInstanceData(InstanceData* data)
{
#ifdef CO_AGGRESSIVE_CACHING
    _instanceDataCache.push_back(data);
#else
    delete data;
#endif
}

uint128_t FullMasterCM::commit(const uint32_t incarnation)
{
    LBASSERT(_version != VERSION_NONE);

    if (!_object->isDirty())
    {
        Mutex mutex(_slaves);
        _updateCommitCount(incarnation);
        _obsolete();
        return _version;
    }

    _maxVersion.waitGE(_version.low() + 1);
    Mutex mutex(_slaves);
#if 0
    LBLOG( LOG_OBJECTS ) << "commit v" << _version << " " << command
                         << std::endl;
#endif
    _updateCommitCount(incarnation);
    _commit();
    _obsolete();
    return _version;
}

void FullMasterCM::_commit()
{
    InstanceData* instanceData = _newInstanceData();
    instanceData->os.enableCommit(_version + 1, *_slaves);
    _object->getInstanceData(instanceData->os);
    instanceData->os.disable();

    if (instanceData->os.hasSentData())
    {
        ++_version;
        LBASSERT(_version != VERSION_NONE);
#if 0
        LBINFO << "Committed v" << _version << "@" << _commitCount << ", id "
               << _object->getID() << std::endl;
#endif
        _addInstanceData(instanceData);
    }
    else
        _instanceDataCache.push_back(instanceData);
}

void FullMasterCM::push(const uint128_t& groupID, const uint128_t& typeID,
                        const Nodes& nodes)
{
    Mutex mutex(_slaves);
    InstanceData* instanceData = _instanceDatas.back();
    instanceData->os.push(nodes, _object->getID(), groupID, typeID);
}

bool FullMasterCM::sendSync(const MasterCMCommand& command)
{
    // const uint128_t& version = command.getRequestedVersion();
    const uint128_t& maxCachedVersion = command.getMaxCachedVersion();
    const bool useCache =
        command.useCache() &&
        command.getMasterInstanceID() == _object->getInstanceID() &&
        maxCachedVersion == _version;

    if (!useCache)
    {
        Mutex mutex(_slaves);
        InstanceData* instanceData = _instanceDatas.back();
        instanceData->os.sync(command);
    }

    NodePtr node = command.getNode();
    node->send(CMD_NODE_SYNC_OBJECT_REPLY, useCache /*preferMulticast*/)
        << node->getNodeID() << command.getObjectID() << command.getRequestID()
        << true << command.useCache() << useCache;
    return true;
}
}
