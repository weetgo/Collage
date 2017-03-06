
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

#ifndef CO_OBJECTCM_H
#define CO_OBJECTCM_H

#include <co/dispatcher.h> // base class
#include <co/masterCMCommand.h>
#include <co/objectVersion.h> // VERSION_FOO values
#include <co/types.h>

//#define CO_INSTRUMENT_MULTICAST
#ifdef CO_INSTRUMENT_MULTICAST
#include <lunchbox/atomic.h>
#endif
#include <lunchbox/spinLock.h> // member

namespace co
{
class ObjectCM;
typedef lunchbox::RefPtr<ObjectCM> ObjectCMPtr;

/**
 * @internal
 * The object change manager base class.
 *
 * Each object has a change manager to create and store version information.
 * The type of change manager depends on the object implementation, and if it is
 * the master object or a slave object.
 */
class ObjectCM : public Dispatcher, public lunchbox::Referenced
{
public:
    /** Construct a new change manager. */
    explicit ObjectCM(Object* object);

    /** Destruct this change manager. */
    virtual ~ObjectCM();

    /** Initialize the change manager. */
    virtual void init() = 0;

    /** Deinitialize the change manager. */
    virtual void exit();

    /** @name Versioning */
    //@{
    /** @sa Object::push() */
    virtual void push(const uint128_t& groupID, const uint128_t& typeID,
                      const Nodes& nodes);

    /**
     * Synchronize an instance to the managed object.
     *
     * @param command the command initiating the sync.
     * @return true if handled, false otherwise.
     */
    virtual bool sendSync(const MasterCMCommand& command);

    /**
     * Commit a new version.
     *
     * @param incarnation the commit incarnation for auto obsoletion.
     * @return the new head version.
     */
    virtual uint128_t commit(const uint32_t incarnation LB_UNUSED)
    {
        LBUNIMPLEMENTED;
        return VERSION_NONE;
    }

    /**
     * Automatically obsolete old versions.
     *
     * @param count the number of versions to retain, excluding the head
     *              version.
     */
    virtual void setAutoObsolete(const uint32_t count LB_UNUSED)
    {
        LBUNIMPLEMENTED;
    }

    /** @return get the number of versions this object retains. */
    virtual uint32_t getAutoObsolete() const
    {
        LBUNIMPLEMENTED;
        return 0;
    }

    /**
     * Sync to a given version.
     *
     * @param version the version to synchronize, must be bigger than the
     *                current version.
     * @return the version of the object after the operation.
     */
    virtual uint128_t sync(const uint128_t& version LB_UNUSED)
    {
        LBUNIMPLEMENTED;
        return VERSION_FIRST;
    }

    /** @return the latest available (head) version. */
    virtual uint128_t getHeadVersion() const = 0;

    /** @return the current version. */
    virtual uint128_t getVersion() const = 0;
    //@}

    /** @return if this object keeps instance data buffers. */
    virtual bool isBuffered() const { return false; }
    /** @return if this instance is the master version. */
    virtual bool isMaster() const = 0;

    /** @return the instance identifier of the master object. */
    virtual uint32_t getMasterInstanceID() const = 0;

    /** Set the master node. */
    virtual void setMasterNode(NodePtr) { /* nop */}

    /** @return the master node, may be 0. */
    virtual NodePtr getMasterNode() { return 0; }
    /**
     * Add a subscribed slave to the managed object.
     *
     * @param command the subscribe command initiating the add.
     * @return true if handled, false otherwise.
     */
    virtual bool addSlave(const MasterCMCommand& command) = 0;

    /**
     * Remove a subscribed slave.
     *
     * @param node the slave node.
     * @param instanceID the slave's instance identifier.
     */
    virtual void removeSlave(NodePtr node LB_UNUSED,
                             const uint32_t instanceID LB_UNUSED)
    {
        LBUNIMPLEMENTED;
    }

    /** Remove all subscribed slaves from the given node. */
    virtual void removeSlaves(NodePtr node) = 0;

    /** @return the vector of current slave nodes. */
    virtual const Nodes getSlaveNodes() const { return Nodes(); }
    /** Apply the initial data after mapping. */
    virtual void applyMapData(const uint128_t& version LB_UNUSED)
    {
        LBUNIMPLEMENTED;
    }

    /** Add existing instance data to the object (from local node cache) */
    virtual void addInstanceDatas(const ObjectDataIStreamDeque&,
                                  const uint128_t&)
    {
        LBDONTCALL;
    }

    /** Speculatively send instance data to all nodes. */
    virtual void sendInstanceData(const Nodes&) {}
    /** @internal @return the object. */
    const Object* getObject() const { return _object; }
    /** @internal Swap the object. */
    void setObject(Object* object)
    {
        LBASSERT(object);
        _object = object;
    }

    /** The default CM for unattached objects. */
    static ObjectCMPtr ZERO;

protected:
    Object* _object;          //!< The managed object.
    lunchbox::SpinLock _lock; //!< Protects unbuffered operations on _object

#ifdef CO_INSTRUMENT_MULTICAST
    static lunchbox::a_int32_t _hit;
    static lunchbox::a_int32_t _miss;
#endif

    bool _addSlave(const MasterCMCommand& command, const uint128_t& version);
    virtual bool _initSlave(const MasterCMCommand& command,
                            const uint128_t& replyVersion, bool replyUseCache);
    void _sendMapSuccess(const MasterCMCommand& command, const bool multicast);
    void _sendMapReply(const MasterCMCommand& command, const uint128_t& version,
                       const bool result, const bool useCache,
                       const bool multicast);
    void _sendEmptyVersion(const MasterCMCommand& command,
                           const uint128_t& version, const bool multicast);
};
}

#endif // CO_OBJECTCM_H
