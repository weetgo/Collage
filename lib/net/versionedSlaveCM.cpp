
/* Copyright (c) 2007-2010, Stefan Eilemann <eile@equalizergraphics.com> 
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

#include <pthread.h>
#include "versionedSlaveCM.h"

#include "command.h"
#include "commands.h"
#include "log.h"
#include "object.h"
#include "objectDeltaDataIStream.h"
#include "objectInstanceDataIStream.h"
#include "session.h"

#include <eq/base/scopedMutex.h>
#include <limits>

namespace eq
{
namespace net
{
typedef CommandFunc<VersionedSlaveCM> CmdFunc;

VersionedSlaveCM::VersionedSlaveCM( Object* object, uint32_t masterInstanceID )
        : _object( object )
        , _version( VERSION_NONE )
        , _mutex( 0 )
        , _currentIStream( 0 )
        , _masterInstanceID( masterInstanceID )
        , _ostream( object )
{
    registerCommand( CMD_OBJECT_INSTANCE,
                     CmdFunc( this, &VersionedSlaveCM::_cmdInstance ), 0 );
    registerCommand( CMD_OBJECT_DELTA,
                     CmdFunc( this, &VersionedSlaveCM::_cmdDelta ), 0 );
    registerCommand( CMD_OBJECT_COMMIT, 
                     CmdFunc( this, &VersionedSlaveCM::_cmdCommit ), 0 );
    registerCommand( CMD_OBJECT_VERSION,
                     CmdFunc( this, &VersionedSlaveCM::_cmdVersion ), 0 );
}

VersionedSlaveCM::~VersionedSlaveCM()
{
    delete _mutex;
    _mutex = 0;

    while( !_queuedVersions.isEmpty( ))
        delete _queuedVersions.pop();

    delete _currentIStream;
    _currentIStream = 0;

    _version = VERSION_NONE;
    _master = 0;
}

void VersionedSlaveCM::makeThreadSafe()
{
    if( _mutex ) 
        return;

    _mutex = new base::Lock;
}

uint32_t VersionedSlaveCM::commitNB()
{
    NodePtr localNode = _object->getLocalNode();
    ObjectCommitPacket packet;
    packet.instanceID = _object->_instanceID;
    packet.requestID  = localNode->registerRequest();

    _object->send( _object->getLocalNode(), packet );
    return packet.requestID;
}

uint32_t VersionedSlaveCM::commitSync( const uint32_t commitID )
{
    NodePtr localNode = _object->getLocalNode();
    uint32_t version = VERSION_NONE;
    localNode->waitRequest( commitID, version );
    return version;
}

uint32_t VersionedSlaveCM::sync( const uint32_t version )
{
    EQLOG( LOG_OBJECTS ) << "sync to v" << version << ", id " 
                         << _object->getID() << "." << _object->getInstanceID()
                         << std::endl;
    if( _version == version )
        return _version;

    if( !_mutex )
        CHECK_THREAD( _thread );

    base::ScopedMutex<> mutex( _mutex );

    if( version == VERSION_HEAD )
    {
        _syncToHead();
        return _version;
    }

    EQASSERTINFO( _version <= version,
                  "can't sync to older version of object " << 
                  typeid( *_object ).name() << " " << _object->getID() <<
                  " (" << _version << ", " << version <<")" );

    while( _version < version )
    {
        ObjectDataIStream* is = _queuedVersions.pop();
        _unpackOneVersion( is );
        EQASSERTINFO( _version == is->getVersion(), "Have version " 
                      << _version << " instead of " << is->getVersion( ));
        delete is;
    }

    NodePtr node = _object->getLocalNode();
    if( node.isValid( ))
        node->flushCommands();

    return _version;
}

void VersionedSlaveCM::_syncToHead()
{
    if( _queuedVersions.isEmpty( ))
        return;

    ObjectDataIStream* is = 0;
    while( _queuedVersions.tryPop( is ))
    {
        EQASSERT( is );
        _unpackOneVersion( is );
        EQASSERTINFO( _version == is->getVersion(), "Have version " 
                      << _version << " instead of " << is->getVersion( ));
        delete is;
    }

    NodePtr localNode = _object->getLocalNode();
    if( localNode.isValid( ))
        localNode->flushCommands();
}


uint32_t VersionedSlaveCM::getHeadVersion() const
{
    ObjectDataIStream* is = 0;
    if( _queuedVersions.getBack( is ))
    {
        EQASSERT( is );
        return is->getVersion();
    }
    return _version;    
}

void VersionedSlaveCM::_unpackOneVersion( ObjectDataIStream* is )
{
    EQASSERT( is );
    EQASSERTINFO( _version == is->getVersion() - 1, "Expected version " 
                  << _version + 1 << ", got " << is->getVersion() );

    if( is->getType() == ObjectDataIStream::TYPE_INSTANCE )
        _object->applyInstanceData( *is );
    else
    {
        EQASSERT( is->getType() == ObjectDataIStream::TYPE_DELTA );
        _object->unpack( *is );
    }

    _version = is->getVersion();
    EQASSERT( _version != VERSION_INVALID );
    EQASSERT( _version != VERSION_NONE );
    EQLOG( LOG_OBJECTS ) << "applied v" << _version << ", id "
                         << _object->getID() << "." << _object->getInstanceID()
                         << std::endl;

    EQASSERTINFO( is->getRemainingBufferSize()==0 && is->nRemainingBuffers()==0,
                  "Object " << typeid( *_object ).name() <<
                  " did not unpack all data" );
}


void VersionedSlaveCM::applyMapData()
{
    ObjectDataIStream* is = _queuedVersions.pop();
    EQASSERTINFO( is->getType() == ObjectDataIStream::TYPE_INSTANCE,
                  typeid( *_object ).name() << " id " << _object->getID() <<
                  "." << _object->getInstanceID( ));

    _object->applyInstanceData( *is );
    _version = is->getVersion();
    EQASSERT( _version != VERSION_INVALID );

    EQASSERTINFO( is->getRemainingBufferSize()==0 && is->nRemainingBuffers()==0,
                  "Object " << typeid( *_object ).name() << 
                  " did not unpack all data" );

    delete is;
    EQLOG( LOG_OBJECTS ) << "Mapped initial data for " << _object->getID()
                         << "." << _object->getInstanceID() << " v" << _version
                         << " ready" << std::endl;
}

void VersionedSlaveCM::addInstanceDatas( const InstanceDataDeque& cache, 
                                         const uint32_t startVersion )
{
    CHECK_THREAD( _cmdThread );
    EQLOG( LOG_OBJECTS ) << base::disableFlush << "Adding data front ";

    uint32_t oldest = VERSION_NONE;
    uint32_t newest = 0;
    if( !_queuedVersions.isEmpty( ))
    {
        ObjectDataIStream* is = 0;

        EQCHECK( _queuedVersions.getFront( is ));
        oldest = is->getVersion();

        EQCHECK( _queuedVersions.getBack( is ));
        newest = is->getVersion();
    }

    InstanceDataDeque  head;
    InstanceDataVector tail;

    for( InstanceDataDeque::const_iterator i = cache.begin();
         i != cache.end(); ++i )
    {
        ObjectInstanceDataIStream* stream = *i;
        const uint32_t version = stream->getVersion();
        if( version < startVersion )
            continue;
        
        EQASSERT( stream->isReady( ));
        if( !stream->isReady( ))
            break;

        if( version < oldest )
            head.push_front( stream );
        else if( version > newest )
            tail.push_back( stream );
    }

    for( InstanceDataDeque::const_iterator i = head.begin();
         i != head.end(); ++i )
    {
        const ObjectInstanceDataIStream* stream = *i;
        _queuedVersions.pushFront( new ObjectInstanceDataIStream( *stream ));
        EQLOG( LOG_OBJECTS ) << stream->getVersion() << ' ';
    }

    EQLOG( LOG_OBJECTS ) << " back ";
    for( InstanceDataVector::const_iterator i = tail.begin();
         i != tail.end(); ++i )
    {
        const ObjectInstanceDataIStream* stream = *i;
        _queuedVersions.push( new ObjectInstanceDataIStream( *stream ));
        EQLOG( LOG_OBJECTS ) << stream->getVersion() << ' ';
    }

#ifndef NDEBUG // consistency check
    uint32_t version = std::numeric_limits< uint32_t >::max();
    for( InstanceDataVector::const_iterator i = tail.begin();
         i != tail.end(); ++i )
    {
        const ObjectInstanceDataIStream* stream = *i;
        if( version != std::numeric_limits< uint32_t >::max( ))
            EQASSERT( version + 1 == stream->getVersion( ));
        version = stream->getVersion();
    }
#endif

    EQLOG( LOG_OBJECTS ) << std::endl << base::enableFlush;
}

//---------------------------------------------------------------------------
// command handlers
//---------------------------------------------------------------------------

bool VersionedSlaveCM::_ignoreCommand( const Command& command ) const
{
    if( _version != VERSION_NONE || !_queuedVersions.isEmpty( ))
        return false;

    const ObjectPacket* packet = command.getPacket< const ObjectPacket >();

    if( packet->instanceID == _object->getInstanceID( ))
        return false;

    // Detected the following case:
    // - p1, t1 calls commit
    // - p1, t2 calls mapObject
    // - p1, cmd commits new version
    // - p1, cmd subscribes object
    // - p1, rcv attaches object
    // - p1, cmd receives commit data
    // -> newly attached object recv new commit data before map data, ignore it
    return true;
}

CommandResult VersionedSlaveCM::_cmdInstance( Command& command )
{
    CHECK_THREAD( _cmdThread );
    EQASSERT( command.getNode().isValid( ));

    if( _ignoreCommand( command ))
        return COMMAND_HANDLED;

    if( !_currentIStream )
        _currentIStream = new ObjectInstanceDataIStream;

    _currentIStream->addDataPacket( command );

    if( _currentIStream->isReady( ))
    {
        const uint32_t version = _currentIStream->getVersion();
        EQLOG( LOG_OBJECTS ) << "v" << version << ", id " << _object->getID()
                             << "." << _object->getInstanceID() << " ready"
                             << std::endl;

        _queuedVersions.push( _currentIStream );
        _object->notifyNewHeadVersion( version );
        _currentIStream = 0;
    }
    return COMMAND_HANDLED;
}

CommandResult VersionedSlaveCM::_cmdDelta( Command& command )
{
    CHECK_THREAD( _cmdThread );

    if( _ignoreCommand( command ))
        return COMMAND_HANDLED;

    if( !_currentIStream )
        _currentIStream = new ObjectDeltaDataIStream;

    _currentIStream->addDataPacket( command );

    if( _currentIStream->isReady( ))
    {
        const uint32_t version = _currentIStream->getVersion();
        EQLOG( LOG_OBJECTS ) << "v" << version << ", id " << _object->getID()
                             << "." << _object->getInstanceID() << " ready"
                             << std::endl;

        _queuedVersions.push( _currentIStream );
        _object->notifyNewHeadVersion( version );
        _currentIStream = 0;
    }
    return COMMAND_HANDLED;
}

CommandResult VersionedSlaveCM::_cmdCommit( Command& command )
{
    CHECK_THREAD( _cmdThread );
    const ObjectCommitPacket* packet = command.getPacket<ObjectCommitPacket>();
    EQLOG( LOG_OBJECTS ) << "commit v" << _version << " " << command 
                         << std::endl;

    NodePtr localNode = _object->getLocalNode();
    if( !_master || !_master->isConnected( ))
    {
        EQASSERTINFO( false, "Master node not connected" );
        localNode->serveRequest( packet->requestID,
                                 static_cast< uint32_t >( VERSION_NONE ));
        return COMMAND_HANDLED;
    }

    _ostream.enable( _master, false );
    _object->pack( _ostream );
    _ostream.disable();

    localNode->serveRequest( packet->requestID, _object->getVersion( ));
    return COMMAND_HANDLED;
}

CommandResult VersionedSlaveCM::_cmdVersion( Command& command )
{
    const ObjectVersionPacket* packet = 
        command.getPacket< ObjectVersionPacket >();
    _version = packet->version;
    EQASSERT( _version != VERSION_INVALID );
    return COMMAND_HANDLED;
}

}
}
