
/* Copyright (c) 2009-2017, Cedric Stalder <cedric.stalder@gmail.com>
 *                          Stefan Eilemann <eile@equalizergraphics.com>
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

#include "rspConnection.h"

#include "connection.h"
#include "connectionDescription.h"
#include "global.h"
#include "log.h"

#include <lunchbox/rng.h>
#include <lunchbox/scopedMutex.h>
#include <lunchbox/sleep.h>

#include <boost/bind.hpp>

//#define CO_INSTRUMENT_RSP
#define CO_RSP_MERGE_WRITES
#define CO_RSP_MAX_TIMEOUTS 1000
#ifdef _WIN32
#define CO_RSP_DEFAULT_PORT (4242)
#else
#define CO_RSP_DEFAULT_PORT ((getuid() % 64511) + 1024)
#endif

// Note: Do not use version > 255, endianness detection magic relies on this.
const uint16_t CO_RSP_PROTOCOL_VERSION = 0;

namespace ip = boost::asio::ip;

namespace co
{
namespace
{
#ifdef CO_INSTRUMENT_RSP
lunchbox::a_int32_t nReadData;
lunchbox::a_int32_t nBytesRead;
lunchbox::a_int32_t nBytesWritten;
lunchbox::a_int32_t nDatagrams;
lunchbox::a_int32_t nRepeated;
lunchbox::a_int32_t nMergedDatagrams;
lunchbox::a_int32_t nAckRequests;
lunchbox::a_int32_t nAcksSend;
lunchbox::a_int32_t nAcksSendTotal;
lunchbox::a_int32_t nAcksRead;
lunchbox::a_int32_t nAcksAccepted;
lunchbox::a_int32_t nNAcksSend;
lunchbox::a_int32_t nNAcksRead;
lunchbox::a_int32_t nNAcksResend;

float writeWaitTime = 0.f;
lunchbox::Clock instrumentClock;
#endif

static uint16_t _numBuffers = 0;
}

RSPConnection::RSPConnection()
    : _id(0)
    , _idAccepted(false)
    , _mtu(Global::getIAttribute(Global::IATTR_UDP_MTU))
    , _ackFreq(Global::getIAttribute(Global::IATTR_RSP_ACK_FREQUENCY))
    , _payloadSize(_mtu - sizeof(DatagramData))
    , _timeouts(0)
    , _event(new EventConnection)
    , _read(0)
    , _write(0)
    , _timeout(_ioService)
    , _wakeup(_ioService)
    , _maxBucketSize((_mtu * _ackFreq) >> 1)
    , _bucketSize(0)
    , _sendRate(0)
    , _thread(0)
    , _acked(std::numeric_limits<uint16_t>::max())
    , _threadBuffers(Global::getIAttribute(Global::IATTR_RSP_NUM_BUFFERS))
    , _recvBuffer(_mtu)
    , _readBuffer(0)
    , _readBufferPos(0)
    , _sequence(0)
    // ensure we have a handleConnectedTimeout before the write pop
    , _writeTimeOut(Global::IATTR_RSP_ACK_TIMEOUT * CO_RSP_MAX_TIMEOUTS * 2)
{
    _buildNewID();
    ConnectionDescriptionPtr description = _getDescription();
    description->type = CONNECTIONTYPE_RSP;
    description->bandwidth = 102400;

    LBCHECK(_event->connect());

    _buffers.reserve(Global::getIAttribute(Global::IATTR_RSP_NUM_BUFFERS));
    while (static_cast<int32_t>(_buffers.size()) <
           Global::getIAttribute(Global::IATTR_RSP_NUM_BUFFERS))
    {
        _buffers.push_back(new Buffer(_mtu));
    }

    LBASSERT(sizeof(DatagramNack) <= size_t(_mtu));
    LBLOG(LOG_RSP) << "New RSP connection, " << _buffers.size()
                   << " buffers of " << _mtu << " bytes" << std::endl;
}

RSPConnection::~RSPConnection()
{
    close();
    while (!_buffers.empty())
    {
        delete _buffers.back();
        _buffers.pop_back();
    }
#ifdef CO_INSTRUMENT_RSP
    LBWARN << *this << std::endl;
    instrumentClock.reset();
#endif
}

void RSPConnection::close()
{
    if (_parent.isValid() && _parent->_id == _id)
        _parent->close();

    while (!_parent && _isWriting())
        lunchbox::sleep(10 /*ms*/);

    if (isClosed())
        return;
    {
        lunchbox::ScopedWrite mutex(_mutexEvent);
        if (_thread)
        {
            LBASSERT(!_thread->isCurrent());
            _sendSimpleDatagram(ID_EXIT, _id);
            _ioService.stop();
            _thread->join();
            delete _thread;
        }

        _setState(STATE_CLOSING);
        if (_thread)
        {
            _thread = 0;

            // notify children to close
            for (RSPConnectionsCIter i = _children.begin();
                 i != _children.end(); ++i)
            {
                RSPConnectionPtr child = *i;
                lunchbox::ScopedWrite mutexChild(child->_mutexEvent);
                child->_appBuffers.push(0);
                child->_event->set();
            }

            _children.clear();
            _newChildren.clear();
        }

        _parent = 0;

        if (_read)
            _read->close();
        delete _read;
        _read = 0;

        if (_write)
            _write->close();
        delete _write;
        _write = 0;

        _threadBuffers.clear();
        _appBuffers.push(0); // unlock any other read/write threads

        _setState(STATE_CLOSED);
    }
    _event->close();
}

//----------------------------------------------------------------------
// Async IO handles
//----------------------------------------------------------------------
uint16_t RSPConnection::_buildNewID()
{
    lunchbox::RNG rng;
    _id = rng.get<uint16_t>();
    return _id;
}

bool RSPConnection::listen()
{
    ConnectionDescriptionPtr description = _getDescription();
    LBASSERT(description->type == CONNECTIONTYPE_RSP);

    if (!isClosed())
        return false;

    _setState(STATE_CONNECTING);
    _numBuffers = Global::getIAttribute(Global::IATTR_RSP_NUM_BUFFERS);

    // init udp connection
    if (description->port == ConnectionDescription::RANDOM_MULTICAST_PORT)
        description->port = 0; // Let OS choose
    else if (description->port == 0)
        description->port = CO_RSP_DEFAULT_PORT;
    if (description->getHostname().empty())
        description->setHostname("239.255.42.43");
    if (description->getInterface().empty())
        description->setInterface("0.0.0.0");

    try
    {
        const ip::address readAddress(ip::address::from_string("0.0.0.0"));
        const ip::udp::endpoint readEndpoint(readAddress, description->port);
        const std::string& port = std::to_string(unsigned(description->port));
        ip::udp::resolver resolver(_ioService);
        const ip::udp::resolver::query queryHN(ip::udp::v4(),
                                               description->getHostname(),
                                               port);
        const ip::udp::resolver::iterator hostname = resolver.resolve(queryHN);

        if (hostname == ip::udp::resolver::iterator())
            return false;

        const ip::udp::endpoint writeEndpoint = *hostname;
        const ip::address mcAddr(writeEndpoint.address());

        _read = new ip::udp::socket(_ioService);
        _write = new ip::udp::socket(_ioService);
        _read->open(readEndpoint.protocol());
        _write->open(writeEndpoint.protocol());

        _read->set_option(ip::udp::socket::reuse_address(true));
        _write->set_option(ip::udp::socket::reuse_address(true));
        _read->set_option(ip::udp::socket::receive_buffer_size(
            Global::getIAttribute(Global::IATTR_UDP_BUFFER_SIZE)));
        _write->set_option(ip::udp::socket::send_buffer_size(
            Global::getIAttribute(Global::IATTR_UDP_BUFFER_SIZE)));

        _read->bind(readEndpoint);
        description->port = _read->local_endpoint().port();

        const ip::udp::resolver::query queryIF(ip::udp::v4(),
                                               description->getInterface(),
                                               "0");
        const ip::udp::resolver::iterator interfaceIP =
            resolver.resolve(queryIF);

        if (interfaceIP == ip::udp::resolver::iterator())
            return false;

        const ip::address ifAddr(ip::udp::endpoint(*interfaceIP).address());
        LBDEBUG << "Joining " << mcAddr << " on " << ifAddr << std::endl;

        _read->set_option(
            ip::multicast::join_group(mcAddr.to_v4(), ifAddr.to_v4()));
        _write->set_option(ip::multicast::outbound_interface(ifAddr.to_v4()));
#ifdef SO_BINDTODEVICE // https://github.com/Eyescale/Collage/issues/16
        const std::string& ifIP = ifAddr.to_string();
        ::setsockopt(_write->native(), SOL_SOCKET, SO_BINDTODEVICE,
                     ifIP.c_str(), ifIP.size() + 1);
        ::setsockopt(_read->native(), SOL_SOCKET, SO_BINDTODEVICE, ifIP.c_str(),
                     ifIP.size() + 1);
#endif

        _write->connect(writeEndpoint);

        _read->set_option(ip::multicast::enable_loopback(false));
        _write->set_option(ip::multicast::enable_loopback(false));
    }
    catch (const boost::system::system_error& e)
    {
        LBWARN << "can't setup underlying UDP connection: " << e.what()
               << std::endl;
        delete _read;
        delete _write;
        _read = 0;
        _write = 0;
        return false;
    }

    // init communication protocol thread
    _thread = new Thread(this);
    _bucketSize = 0;
    _sendRate = description->bandwidth;

    // waits until RSP protocol establishes connection to the multicast network
    if (!_thread->start())
    {
        close();
        return false;
    }

    // Make all buffers available for writing
    LBASSERT(_appBuffers.isEmpty());
    _appBuffers.push(_buffers);

    LBDEBUG << "Listening on " << description->getHostname() << ":"
            << description->port << " (" << description->toString() << " @"
            << (void*)this << ")" << std::endl;
    return true;
}

ConnectionPtr RSPConnection::acceptSync()
{
    if (!isListening())
        return 0;

    lunchbox::ScopedWrite mutex(_mutexConnection);
    LBASSERT(!_newChildren.empty());
    if (_newChildren.empty())
        return 0;

    RSPConnectionPtr newConnection = _newChildren.back();
    _newChildren.pop_back();

    LBDEBUG << _id << " accepted RSP connection " << newConnection->_id
            << std::endl;

    lunchbox::ScopedWrite mutex2(_mutexEvent);
    if (_newChildren.empty())
        _event->reset();
    else
        _event->set();

    return newConnection;
}

int64_t RSPConnection::readSync(void* buffer, const uint64_t bytes, const bool)
{
    LBASSERT(bytes > 0);
    if (!isConnected())
        return -1;

    uint64_t bytesLeft = bytes;
    uint8_t* ptr = reinterpret_cast<uint8_t*>(buffer);

    // redundant (done by the caller already), but saves some lock ops
    while (bytesLeft)
    {
        if (!_readBuffer)
        {
            LBASSERT(_readBufferPos == 0);
            _readBuffer = _appBuffers.pop();
            if (!_readBuffer)
            {
                close();
                return (bytes == bytesLeft)
                           ? -1
                           : static_cast<int64_t>(bytes - bytesLeft);
            }
        }

        const DatagramData* header =
            reinterpret_cast<const DatagramData*>(_readBuffer->getData());
        const uint8_t* payload = reinterpret_cast<const uint8_t*>(header + 1);
        const size_t dataLeft = header->size - _readBufferPos;
        const size_t size = LB_MIN(static_cast<size_t>(bytesLeft), dataLeft);

        memcpy(ptr, payload + _readBufferPos, size);
        _readBufferPos += size;
        ptr += size;
        bytesLeft -= size;

        // if all data in the buffer has been taken
        if (_readBufferPos >= header->size)
        {
            LBASSERT(_readBufferPos == header->size);
            // LBLOG( LOG_RSP ) << "reset read buffer  " << header->sequence
            //                 << std::endl;

            LBCHECK(_threadBuffers.push(_readBuffer));
            _readBuffer = 0;
            _readBufferPos = 0;
        }
        else
        {
            LBASSERT(_readBufferPos < header->size);
        }
    }

    if (_readBuffer || !_appBuffers.isEmpty())
        _event->set();
    else
    {
        lunchbox::ScopedWrite mutex(_mutexEvent);
        if (_appBuffers.isEmpty())
            _event->reset();
    }

#ifdef CO_INSTRUMENT_RSP
    nBytesRead += bytes;
#endif
    return bytes;
}

void RSPConnection::Thread::run()
{
    _connection->_runThread();
    _connection = 0;
    LBDEBUG << "Left RSP protocol thread" << std::endl;
}

void RSPConnection::_handleTimeout(const boost::system::error_code& error)
{
    if (error == boost::asio::error::operation_aborted)
        return;

    if (isListening())
        _handleConnectedTimeout();
    else if (_idAccepted)
        _handleInitTimeout();
    else
        _handleAcceptIDTimeout();
}

void RSPConnection::_handleAcceptIDTimeout()
{
    ++_timeouts;
    if (_timeouts < 20)
    {
        LBLOG(LOG_RSP) << "Announce " << _id << " " << _timeouts << std::endl;
        _sendSimpleDatagram(ID_HELLO, _id);
    }
    else
    {
        LBLOG(LOG_RSP) << "Confirm " << _id << std::endl;
        _sendSimpleDatagram(ID_CONFIRM, _id);
        _addConnection(_id, _sequence);
        _idAccepted = true;
        _timeouts = 0;
        // send a first datagram to announce me and discover all other
        // connections
        _sendCountNode();
    }
    _setTimeout(10);
}

void RSPConnection::_handleInitTimeout()
{
    LBASSERT(!isListening())
    ++_timeouts;
    if (_timeouts < 20)
        _sendCountNode();
    else
    {
        _setState(STATE_LISTENING);
        LBDEBUG << "RSP connection " << _id << " listening" << std::endl;
        _timeouts = 0;
        _ioService.stop(); // thread initialized, run restarts
    }
    _setTimeout(10);
}

void RSPConnection::_clearWriteQueues()
{
    while (!_threadBuffers.isEmpty())
    {
        Buffer* buffer = 0;
        _threadBuffers.pop(buffer);
        _writeBuffers.push_back(buffer);
    }

    _finishWriteQueue(_sequence - 1);
    LBASSERT(_threadBuffers.isEmpty() && _writeBuffers.empty());
}

void RSPConnection::_handleConnectedTimeout()
{
    if (!isListening())
    {
        _clearWriteQueues();
        _ioService.stop();
        return;
    }

    _processOutgoing();

    if (_timeouts >= CO_RSP_MAX_TIMEOUTS)
    {
        LBERROR << "Too many timeouts during send: " << _timeouts << std::endl;
        bool all = true;
        for (RSPConnectionsCIter i = _children.begin(); i != _children.end();
             ++i)
        {
            RSPConnectionPtr child = *i;
            if (child->_acked >= _sequence - _numBuffers && child->_id != _id)
            {
                all = false;
                break;
            }
        }

        // if all connections failed we probably got disconnected -> close and
        // exit else close all failed child connections
        if (all)
        {
            _sendSimpleDatagram(ID_EXIT, _id);
            _appBuffers.pushFront(0); // unlock write function

            for (RSPConnectionsCIter i = _children.begin();
                 i != _children.end(); ++i)
            {
                RSPConnectionPtr child = *i;
                child->_setState(STATE_CLOSING);
                child->_appBuffers.push(0); // unlock read func
            }

            _clearWriteQueues();
            _ioService.stop();
            return;
        }

        RSPConnectionsCIter i = _children.begin();
        while (i != _children.end())
        {
            RSPConnectionPtr child = *i;
            if (child->_acked < _sequence - 1 && _id != child->_id)
            {
                _sendSimpleDatagram(ID_EXIT, child->_id);
                _removeConnection(child->_id);
            }
            else
            {
                uint16_t wb = static_cast<uint16_t>(_writeBuffers.size());
                child->_acked = _sequence - wb;
                ++i;
            }
        }

        _timeouts = 0;
    }
}

RSPConnection::DatagramNode* RSPConnection::_getDatagramNode(const size_t bytes)
{
    if (bytes < sizeof(DatagramNode))
    {
        LBERROR << "DatagramNode size mismatch, got " << bytes << " instead of "
                << sizeof(DatagramNode) << " bytes" << std::endl;
        // close();
        return 0;
    }
    DatagramNode& node =
        *reinterpret_cast<DatagramNode*>(_recvBuffer.getData());
    if (node.protocolVersion != CO_RSP_PROTOCOL_VERSION)
    {
        LBERROR << "Protocol version mismatch, got " << node.protocolVersion
                << " instead of " << CO_RSP_PROTOCOL_VERSION << std::endl;
        // close();
        return 0;
    }
    return &node;
}

bool RSPConnection::_initThread()
{
    LBLOG(LOG_RSP) << "Started RSP protocol thread" << std::endl;
    _timeouts = 0;

    // send a first datagram to announce me and discover other connections
    LBLOG(LOG_RSP) << "Announce " << _id << std::endl;
    _sendSimpleDatagram(ID_HELLO, _id);
    _setTimeout(10);
    _asyncReceiveFrom();
    _ioService.run();
    return isListening();
}

void RSPConnection::_runThread()
{
    //__debugbreak();
    _ioService.reset();
    _ioService.run();
}

void RSPConnection::_setTimeout(const int32_t timeOut)
{
    LBASSERT(timeOut >= 0);
    _timeout.expires_from_now(boost::posix_time::milliseconds(timeOut));
    _timeout.async_wait(boost::bind(&RSPConnection::_handleTimeout, this,
                                    boost::asio::placeholders::error));
}

void RSPConnection::_postWakeup()
{
    _wakeup.expires_from_now(boost::posix_time::milliseconds(0));
    _wakeup.async_wait(boost::bind(&RSPConnection::_handleTimeout, this,
                                   boost::asio::placeholders::error));
}

void RSPConnection::_processOutgoing()
{
#ifdef CO_INSTRUMENT_RSP
    if (instrumentClock.getTime64() > 1000)
    {
        LBWARN << *this << std::endl;
        instrumentClock.reset();
    }
#endif

    if (!_repeatQueue.empty())
        _repeatData();
    else
        _writeData();

    if (!_threadBuffers.isEmpty() || !_repeatQueue.empty())
    {
        _setTimeout(0); // call again to send remaining
        return;
    }
    // no more data to write, check/send ack request, reset timeout

    if (_writeBuffers.empty()) // got all acks
    {
        _timeouts = 0;
        _timeout.cancel();
        return;
    }

    const int64_t timeout =
        Global::getIAttribute(Global::IATTR_RSP_ACK_TIMEOUT);
    const int64_t left = timeout - _clock.getTime64();

    if (left > 0)
    {
        _setTimeout(left);
        return;
    }

    // (repeat) ack request
    _clock.reset();
    ++_timeouts;
    if (_timeouts < CO_RSP_MAX_TIMEOUTS)
        _sendAckRequest();
    _setTimeout(timeout);
}

void RSPConnection::_writeData()
{
    Buffer* buffer = 0;
    if (!_threadBuffers.pop(buffer)) // nothing to write
        return;

    _timeouts = 0;
    LBASSERT(buffer);

    // write buffer
    DatagramData* header = reinterpret_cast<DatagramData*>(buffer->getData());
    header->sequence = _sequence++;

#ifdef CO_RSP_MERGE_WRITES
    if (header->size < _payloadSize && !_threadBuffers.isEmpty())
    {
        std::vector<Buffer*> appBuffers;
        while (header->size < _payloadSize && !_threadBuffers.isEmpty())
        {
            Buffer* buffer2 = 0;
            LBCHECK(_threadBuffers.getFront(buffer2));
            LBASSERT(buffer2);
            DatagramData* header2 =
                reinterpret_cast<DatagramData*>(buffer2->getData());

            if (uint32_t(header->size + header2->size) > _payloadSize)
                break;

            memcpy(reinterpret_cast<uint8_t*>(header + 1) + header->size,
                   header2 + 1, header2->size);
            header->size += header2->size;
            LBCHECK(_threadBuffers.pop(buffer2));
            appBuffers.push_back(buffer2);
#ifdef CO_INSTRUMENT_RSP
            ++nMergedDatagrams;
#endif
        }

        if (!appBuffers.empty())
            _appBuffers.push(appBuffers);
    }
#endif

    // send data
    //  Note 1: We could optimize the send away if we're all alone, but this is
    //          not a use case for RSP, so we don't care.
    //  Note 2: Data to myself will be 'written' in _finishWriteQueue once we
    //          got all acks for the packet
    const uint32_t size = header->size + sizeof(DatagramData);

    _waitWritable(size); // OPT: process incoming in between
    _write->send(boost::asio::buffer(header, size));

#ifdef CO_INSTRUMENT_RSP
    ++nDatagrams;
    nBytesWritten += header->size;
#endif

    // save datagram for repeats (and self)
    _writeBuffers.push_back(buffer);

    if (_children.size() == 1) // We're all alone
    {
        LBASSERT(_children.front()->_id == _id);
        _finishWriteQueue(_sequence - 1);
    }
}

void RSPConnection::_waitWritable(const uint64_t bytes)
{
#ifdef CO_INSTRUMENT_RSP
    lunchbox::Clock clock;
#endif

    _bucketSize += static_cast<uint64_t>(_clock.resetTimef() * _sendRate);
    // opt omit: * 1024 / 1000;
    _bucketSize = LB_MIN(_bucketSize, _maxBucketSize);

    const uint64_t size = LB_MIN(bytes, static_cast<uint64_t>(_mtu));
    while (_bucketSize < size)
    {
        lunchbox::Thread::yield();
        float time = _clock.resetTimef();

        while (time == 0.f)
        {
            lunchbox::Thread::yield();
            time = _clock.resetTimef();
        }

        _bucketSize += static_cast<int64_t>(time * _sendRate);
        _bucketSize = LB_MIN(_bucketSize, _maxBucketSize);
    }
    _bucketSize -= size;

#ifdef CO_INSTRUMENT_RSP
    writeWaitTime += clock.getTimef();
#endif

    ConstConnectionDescriptionPtr description = getDescription();
    if (_sendRate < description->bandwidth)
    {
        _sendRate += int64_t(
            float(Global::getIAttribute(Global::IATTR_RSP_ERROR_UPSCALE)) *
            float(description->bandwidth) * .001f);
        LBLOG(LOG_RSP) << "speeding up to " << _sendRate << " KB/s"
                       << std::endl;
    }
}

void RSPConnection::_repeatData()
{
    _timeouts = 0;

    while (!_repeatQueue.empty())
    {
        Nack& request = _repeatQueue.front();
        const uint16_t distance = _sequence - request.start;

        if (distance == 0)
        {
            LBWARN << "ignoring invalid nack (" << request.start << ".."
                   << request.end << ")" << std::endl;
            _repeatQueue.pop_front();
            continue;
        }

        if (distance <= _writeBuffers.size()) // not already acked
        {
            //          LBLOG( LOG_RSP ) << "Repeat " << request.start << ", "
            //          << _sendRate
            //                           << "KB/s"<< std::endl;

            const size_t i = _writeBuffers.size() - distance;
            Buffer* buffer = _writeBuffers[i];
            LBASSERT(buffer);

            DatagramData* header =
                reinterpret_cast<DatagramData*>(buffer->getData());
            const uint32_t size = header->size + sizeof(DatagramData);
            LBASSERT(header->sequence == request.start);

            // send data
            _waitWritable(size); // OPT: process incoming in between
            _write->send(boost::asio::buffer(header, size));
#ifdef CO_INSTRUMENT_RSP
            ++nRepeated;
#endif
        }

        if (request.start == request.end)
            _repeatQueue.pop_front(); // done with request
        else
            ++request.start;

        if (distance <= _writeBuffers.size()) // send something
            return;
    }
}

void RSPConnection::_finishWriteQueue(const uint16_t sequence)
{
    LBASSERT(!_writeBuffers.empty());

    RSPConnectionPtr connection = _findConnection(_id);
    LBASSERT(connection.isValid());
    LBASSERT(connection->_recvBuffers.empty());

    // Bundle pushing the buffers to the app to avoid excessive lock ops
    Buffers readBuffers;
    Buffers freeBuffers;

    const uint16_t size = _sequence - sequence - 1;
    LBASSERTINFO(size <= uint16_t(_writeBuffers.size()),
                 size << " > " << _writeBuffers.size());
    LBLOG(LOG_RSP) << "Got all remote acks for " << sequence << " current "
                   << _sequence << " advance " << _writeBuffers.size() - size
                   << " buffers" << std::endl;

    while (_writeBuffers.size() > size_t(size))
    {
        Buffer* buffer = _writeBuffers.front();
        _writeBuffers.pop_front();

#ifndef NDEBUG
        DatagramData* datagram =
            reinterpret_cast<DatagramData*>(buffer->getData());
        LBASSERT(datagram->writerID == _id);
        LBASSERTINFO(datagram->sequence ==
                         uint16_t(connection->_sequence + readBuffers.size()),
                     datagram->sequence << ", " << connection->_sequence << ", "
                                        << readBuffers.size());
// LBLOG( LOG_RSP ) << "self receive " << datagram->sequence << std::endl;
#endif

        Buffer* newBuffer = connection->_newDataBuffer(*buffer);
        if (!newBuffer && !readBuffers.empty()) // push prepared app buffers
        {
            lunchbox::ScopedWrite mutex(connection->_mutexEvent);
            LBLOG(LOG_RSP) << "post " << readBuffers.size()
                           << " buffers starting with sequence "
                           << connection->_sequence << std::endl;

            connection->_appBuffers.push(readBuffers);
            connection->_sequence += uint16_t(readBuffers.size());
            readBuffers.clear();
            connection->_event->set();
        }

        while (!newBuffer) // no more data buffers, wait for app to drain
        {
            newBuffer = connection->_newDataBuffer(*buffer);
            lunchbox::Thread::yield();
        }

        freeBuffers.push_back(buffer);
        readBuffers.push_back(newBuffer);
    }

    _appBuffers.push(freeBuffers);
    if (!readBuffers.empty())
    {
        lunchbox::ScopedWrite mutex(connection->_mutexEvent);
#if 0
        LBLOG( LOG_RSP )
            << "post " << readBuffers.size() << " buffers starting at "
            << connection->_sequence << std::endl;
#endif

        connection->_appBuffers.push(readBuffers);
        connection->_sequence += uint16_t(readBuffers.size());
        connection->_event->set();
    }

    connection->_acked = uint16_t(connection->_sequence - 1);
    LBASSERT(connection->_acked == sequence);

    _timeouts = 0;
}

void RSPConnection::_handlePacket(const boost::system::error_code& /* error */,
                                  const size_t bytes)
{
    if (isListening())
    {
        _handleConnectedData(bytes);

        if (isListening())
            _processOutgoing();
        else
        {
            _ioService.stop();
            return;
        }
    }
    else if (bytes >= sizeof(DatagramNode))
    {
        if (_idAccepted)
            _handleInitData(bytes, false);
        else
            _handleAcceptIDData(bytes);
    }

    // LBLOG( LOG_RSP ) << "_handlePacket timeout " << timeout << std::endl;
    _asyncReceiveFrom();
}

void RSPConnection::_handleAcceptIDData(const size_t bytes)
{
    DatagramNode* pNode = _getDatagramNode(bytes);
    if (!pNode)
        return;

    DatagramNode& node = *pNode;

    switch (node.type)
    {
    case ID_HELLO:
        _checkNewID(node.connectionID);
        break;

    case ID_HELLO_REPLY:
        _addConnection(node.connectionID, node.data);
        break;

    case ID_DENY:
        // a connection refused my ID, try another ID
        if (node.connectionID == _id)
        {
            _timeouts = 0;
            _sendSimpleDatagram(ID_HELLO, _buildNewID());
            LBLOG(LOG_RSP) << "Announce " << _id << std::endl;
        }
        break;

    case ID_EXIT:
        _removeConnection(node.connectionID);
        break;

    default:
        LBERROR << "Got unexpected datagram type " << node.type << std::endl;
        LBUNIMPLEMENTED;
        break;
    }
}

void RSPConnection::_handleInitData(const size_t bytes, const bool connected)
{
    DatagramNode* pNode = _getDatagramNode(bytes);
    if (!pNode)
        return;

    DatagramNode& node = *pNode;

    switch (node.type)
    {
    case ID_HELLO:
        if (!connected)
            _timeouts = 0;
        _checkNewID(node.connectionID);
        return;

    case ID_CONFIRM:
        if (!connected)
            _timeouts = 0;
        _addConnection(node.connectionID, node.data);
        return;

    case COUNTNODE:
        LBLOG(LOG_RSP) << "Got " << node.data << " nodes from "
                       << node.connectionID << std::endl;
        return;

    case ID_HELLO_REPLY:
        _addConnection(node.connectionID, node.data);
        return;

    case ID_EXIT:
        _removeConnection(node.connectionID);
        return;

    default:
        LBERROR << "Got unexpected datagram type " << node.type << std::endl;
        LBUNIMPLEMENTED;
        break;
    }
}

void RSPConnection::_handleConnectedData(const size_t bytes)
{
    if (bytes < sizeof(uint16_t))
        return;

    void* data = _recvBuffer.getData();
    uint16_t type = *reinterpret_cast<uint16_t*>(data);
    switch (type)
    {
    case DATA:
        LBCHECK(_handleData(bytes));
        break;

    case ACK:
        LBCHECK(_handleAck(bytes));
        break;

    case NACK:
        LBCHECK(_handleNack());
        break;

    case ACKREQ: // The writer asks for an ack/nack
        LBCHECK(_handleAckRequest(bytes));
        break;

    case ID_HELLO:
    case ID_HELLO_REPLY:
    case ID_CONFIRM:
    case ID_EXIT:
    case ID_DENY:
    case COUNTNODE:
        _handleInitData(bytes, true);
        break;

    default:
        LBASSERTINFO(false, "Don't know how to handle packet of type " << type);
    }
}

void RSPConnection::_asyncReceiveFrom()
{
    _read->async_receive_from(
        boost::asio::buffer(_recvBuffer.getData(), _mtu), _readAddr,
        boost::bind(&RSPConnection::_handlePacket, this,
                    boost::asio::placeholders::error,
                    boost::asio::placeholders::bytes_transferred));
}

bool RSPConnection::_handleData(const size_t bytes)
{
    if (bytes < sizeof(DatagramData))
        return false;
    DatagramData& datagram =
        *reinterpret_cast<DatagramData*>(_recvBuffer.getData());

#ifdef CO_INSTRUMENT_RSP
    ++nReadData;
#endif
    const uint16_t writerID = datagram.writerID;
#ifdef Darwin
    // There is occasionally a packet from ourselves, even though multicast loop
    // is not set?!
    if (writerID == _id)
        return true;
#else
    LBASSERT(writerID != _id);
#endif

    RSPConnectionPtr connection = _findConnection(writerID);

    if (!connection) // unknown connection ?
    {
        LBASSERTINFO(false, "Can't find connection with id " << writerID);
        return false;
    }
    LBASSERT(connection->_id == writerID);

    const uint16_t sequence = datagram.sequence;
    //  LBLOG( LOG_RSP ) << "rcvd " << sequence << " from " << writerID
    //  <<std::endl;

    if (connection->_sequence == sequence) // in-order packet
    {
        Buffer* newBuffer = connection->_newDataBuffer(_recvBuffer);
        if (!newBuffer) // no more data buffers, drop packet
            return true;

        lunchbox::ScopedWrite mutex(connection->_mutexEvent);
        connection->_pushDataBuffer(newBuffer);

        while (!connection->_recvBuffers.empty()) // enqueue ready pending data
        {
            newBuffer = connection->_recvBuffers.front();
            if (!newBuffer)
                break;

            connection->_recvBuffers.pop_front();
            connection->_pushDataBuffer(newBuffer);
        }

        if (!connection->_recvBuffers.empty() &&
            !connection->_recvBuffers.front()) // update for new _sequence
        {
            connection->_recvBuffers.pop_front();
        }

        connection->_event->set();
        return true;
    }

    const uint16_t max = std::numeric_limits<uint16_t>::max();
    if ((connection->_sequence > sequence &&
         max - connection->_sequence + sequence > _numBuffers) ||
        (connection->_sequence < sequence &&
         sequence - connection->_sequence > _numBuffers))
    {
        // ignore it if it's a repetition for another reader
        return true;
    }

    // else out of order

    const uint16_t size = sequence - connection->_sequence;
    LBASSERT(size != 0);
    LBASSERTINFO(size <= _numBuffers, size << " > " << _numBuffers);

    ssize_t i = ssize_t(size) - 1;
    const bool gotPacket = (connection->_recvBuffers.size() >= size &&
                            connection->_recvBuffers[i]);
    if (gotPacket)
        return true;

    Buffer* newBuffer = connection->_newDataBuffer(_recvBuffer);
    if (!newBuffer) // no more data buffers, drop packet
        return true;

    if (connection->_recvBuffers.size() < size)
        connection->_recvBuffers.resize(size, 0);

    LBASSERT(!connection->_recvBuffers[i]);
    connection->_recvBuffers[i] = newBuffer;

    // early nack: request missing packets before current
    --i;
    Nack nack = {connection->_sequence, uint16_t(sequence - 1)};
    if (i > 0)
    {
        if (connection->_recvBuffers[i]) // got previous packet
            return true;

        while (i >= 0 && !connection->_recvBuffers[i])
            --i;

        const Buffer* lastBuffer = i >= 0 ? connection->_recvBuffers[i] : 0;
        if (lastBuffer)
        {
            nack.start = connection->_sequence + i;
        }
    }

    LBLOG(LOG_RSP) << "send early nack " << nack.start << ".." << nack.end
                   << " current " << connection->_sequence << " ooo "
                   << connection->_recvBuffers.size() << std::endl;

    if (nack.end < nack.start)
        // OPT: don't drop nack 0..nack.end, but it doesn't happen often
        nack.end = std::numeric_limits<uint16_t>::max();

    _sendNack(writerID, &nack, 1);
    return true;
}

RSPConnection::Buffer* RSPConnection::_newDataBuffer(Buffer& inBuffer)
{
    LBASSERT(static_cast<int32_t>(inBuffer.getMaxSize()) == _mtu);

    Buffer* buffer = 0;
    if (_threadBuffers.pop(buffer))
    {
        buffer->swap(inBuffer);
        return buffer;
    }

    // we do not have a free buffer, which means that the receiver is slower
    // then our read thread. This is bad, because now we'll drop the data and
    // will send a NAck packet upon the ack request, causing retransmission even
    // though we'll probably drop it again
    LBLOG(LOG_RSP) << "Reader too slow, dropping data" << std::endl;

    // Set the event if there is data to read. This shouldn't be needed since
    // the event should be set in this case, but it'll increase the robustness
    lunchbox::ScopedWrite mutex(_mutexEvent);
    if (!_appBuffers.isEmpty())
        _event->set();
    return 0;
}

void RSPConnection::_pushDataBuffer(Buffer* buffer)
{
    LBASSERT(_parent);
#ifndef NDEBUG
    DatagramData* dgram = reinterpret_cast<DatagramData*>(buffer->getData());
    LBASSERTINFO(dgram->sequence == _sequence, dgram->sequence << " != "
                                                               << _sequence);
#endif

    if (((_sequence + _parent->_id) % _ackFreq) == 0)
        _parent->_sendAck(_id, _sequence);

    LBLOG(LOG_RSP) << "post buffer " << _sequence << std::endl;
    ++_sequence;
    _appBuffers.push(buffer);
}

bool RSPConnection::_handleAck(const size_t bytes)
{
    if (bytes < sizeof(DatagramAck))
        return false;
    DatagramAck& ack = *reinterpret_cast<DatagramAck*>(_recvBuffer.getData());

#ifdef CO_INSTRUMENT_RSP
    ++nAcksRead;
#endif

    if (ack.writerID != _id)
        return true;

    LBLOG(LOG_RSP) << "got ack from " << ack.readerID << " for " << ack.writerID
                   << " sequence " << ack.sequence << " current " << _sequence
                   << std::endl;

    // find destination connection, update ack data if needed
    RSPConnectionPtr connection = _findConnection(ack.readerID);
    if (!connection)
    {
        LBUNREACHABLE;
        return false;
    }

    if (connection->_acked >= ack.sequence &&
        connection->_acked - ack.sequence <= _numBuffers)
    {
        // I have received a later ack previously from the reader
        LBLOG(LOG_RSP) << "Late ack" << std::endl;
        return true;
    }

#ifdef CO_INSTRUMENT_RSP
    ++nAcksAccepted;
#endif
    connection->_acked = ack.sequence;
    _timeouts = 0; // reset timeout counter

    // Check if we can advance _acked
    uint16_t acked = ack.sequence;

    for (RSPConnectionsCIter i = _children.begin(); i != _children.end(); ++i)
    {
        RSPConnectionPtr child = *i;
        if (child->_id == _id)
            continue;

        const uint16_t distance = child->_acked - acked;
        if (distance > _numBuffers)
            acked = child->_acked;
    }

    RSPConnectionPtr selfChild = _findConnection(_id);
    const uint16_t distance = acked - selfChild->_acked;
    if (distance <= _numBuffers)
        _finishWriteQueue(acked);
    return true;
}

bool RSPConnection::_handleNack()
{
    DatagramNack& nack =
        *reinterpret_cast<DatagramNack*>(_recvBuffer.getData());

#ifdef CO_INSTRUMENT_RSP
    ++nNAcksRead;
#endif

    if (_id != nack.writerID)
    {
        LBLOG(LOG_RSP) << "ignore " << nack.count << " nacks from "
                       << nack.readerID << " for " << nack.writerID
                       << " (not me)" << std::endl;
        return true;
    }

    LBLOG(LOG_RSP) << "handle " << nack.count << " nacks from " << nack.readerID
                   << " for " << nack.writerID << std::endl;

    RSPConnectionPtr connection = _findConnection(nack.readerID);
    if (!connection)
    {
        LBUNREACHABLE;
        return false;
        // it's an unknown connection, TODO add this connection?
    }

    _timeouts = 0;
    _addRepeat(nack.nacks, nack.count);
    return true;
}

void RSPConnection::_addRepeat(const Nack* nacks, uint16_t num)
{
    LBLOG(LOG_RSP) << lunchbox::disableFlush << "Queue repeat requests ";
    size_t lost = 0;

    for (size_t i = 0; i < num; ++i)
    {
        const Nack& nack = nacks[i];
        LBASSERT(nack.start <= nack.end);

        LBLOG(LOG_RSP) << nack.start << ".." << nack.end << " ";

        bool merged = false;
        for (RepeatQueue::iterator j = _repeatQueue.begin();
             j != _repeatQueue.end() && !merged; ++j)
        {
            Nack& old = *j;
            if (old.start <= nack.end && old.end >= nack.start)
            {
                if (old.start > nack.start)
                {
                    lost += old.start - nack.start;
                    old.start = nack.start;
                    merged = true;
                }
                if (old.end < nack.end)
                {
                    lost += nack.end - old.end;
                    old.end = nack.end;
                    merged = true;
                }
                LBASSERT(lost < _numBuffers);
            }
        }

        if (!merged)
        {
            lost += uint16_t(nack.end - nack.start) + 1;
            LBASSERT(lost <= _numBuffers);
            _repeatQueue.push_back(nack);
        }
    }

    ConstConnectionDescriptionPtr description = getDescription();
    if (_sendRate >
        (description->bandwidth >>
         Global::getIAttribute(Global::IATTR_RSP_MIN_SENDRATE_SHIFT)))
    {
        const float delta =
            float(lost) * .001f *
            Global::getIAttribute(Global::IATTR_RSP_ERROR_DOWNSCALE);
        const float maxDelta =
            .01f *
            float(Global::getIAttribute(Global::IATTR_RSP_ERROR_MAXSCALE));
        const float downScale = LB_MIN(delta, maxDelta);
        _sendRate -= 1 + int64_t(_sendRate * downScale);
        LBLOG(LOG_RSP) << ", lost " << lost << " slowing down "
                       << downScale * 100.f << "% to " << _sendRate << " KB/s"
                       << std::endl
                       << lunchbox::enableFlush;
    }
    else
        LBLOG(LOG_RSP) << std::endl << lunchbox::enableFlush;
}

bool RSPConnection::_handleAckRequest(const size_t bytes)
{
    if (bytes < sizeof(DatagramAckRequest))
        return false;
    DatagramAckRequest& ackRequest =
        *reinterpret_cast<DatagramAckRequest*>(_recvBuffer.getData());

    const uint16_t writerID = ackRequest.writerID;
#ifdef Darwin
    // There is occasionally a packet from ourselves, even though multicast loop
    // is not set?!
    if (writerID == _id)
        return true;
#else
    LBASSERT(writerID != _id);
#endif
    RSPConnectionPtr connection = _findConnection(writerID);
    if (!connection)
    {
        LBUNREACHABLE;
        return false;
    }

    const uint16_t reqID = ackRequest.sequence;
    const uint16_t gotID = connection->_sequence - 1;
    const uint16_t distance = reqID - gotID;

    LBLOG(LOG_RSP) << "ack request " << reqID << " from " << writerID << " got "
                   << gotID << " missing " << distance << std::endl;

    if ((reqID == gotID) || (gotID > reqID && gotID - reqID <= _numBuffers) ||
        (gotID < reqID && distance > _numBuffers))
    {
        _sendAck(connection->_id, gotID);
        return true;
    }
    // else find all missing datagrams

    const uint16_t max = CO_RSP_MAX_NACKS - 2;
    Nack nacks[CO_RSP_MAX_NACKS];
    uint16_t i = 0;

    nacks[i].start = connection->_sequence;
    LBLOG(LOG_RSP) << lunchbox::disableFlush << "nacks: " << nacks[i].start
                   << "..";

    std::deque<Buffer*>::const_iterator j = connection->_recvBuffers.begin();
    std::deque<Buffer*>::const_iterator first = j;
    for (; j != connection->_recvBuffers.end() && i < max; ++j)
    {
        if (*j) // got buffer
        {
            nacks[i].end = connection->_sequence + std::distance(first, j);
            LBLOG(LOG_RSP) << nacks[i].end << ", ";
            if (nacks[i].end < nacks[i].start)
            {
                LBASSERT(nacks[i].end < _numBuffers);
                nacks[i + 1].start = 0;
                nacks[i + 1].end = nacks[i].end;
                nacks[i].end = std::numeric_limits<uint16_t>::max();
                ++i;
            }
            ++i;

            // find next hole
            for (++j; j != connection->_recvBuffers.end() && (*j); ++j)
                /* nop */;

            if (j == connection->_recvBuffers.end())
                break;

            nacks[i].start =
                connection->_sequence + std::distance(first, j) + 1;
            LBLOG(LOG_RSP) << nacks[i].start << "..";
        }
    }

    if (j != connection->_recvBuffers.end() || i == 0)
    {
        nacks[i].end = reqID;
        LBLOG(LOG_RSP) << nacks[i].end;
        ++i;
    }
    else if (uint16_t(reqID - nacks[i - 1].end) < _numBuffers)
    {
        nacks[i].start = nacks[i - 1].end + 1;
        nacks[i].end = reqID;
        LBLOG(LOG_RSP) << nacks[i].start << ".." << nacks[i].end;
        ++i;
    }
    if (nacks[i - 1].end < nacks[i - 1].start)
    {
        LBASSERT(nacks[i - 1].end < _numBuffers);
        nacks[i].start = 0;
        nacks[i].end = nacks[i - 1].end;
        nacks[i - 1].end = std::numeric_limits<uint16_t>::max();
        ++i;
    }

    LBLOG(LOG_RSP) << std::endl
                   << lunchbox::enableFlush << "send " << i << " nacks to "
                   << connection->_id << std::endl;

    LBASSERT(i > 0);
    _sendNack(connection->_id, nacks, i);
    return true;
}

void RSPConnection::_checkNewID(uint16_t id)
{
    // look if the new ID exist in another connection
    if (id == _id || _findConnection(id))
    {
        LBLOG(LOG_RSP) << "Deny " << id << std::endl;
        _sendSimpleDatagram(ID_DENY, _id);
    }
    else
        _sendSimpleDatagram(ID_HELLO_REPLY, _id);
}

RSPConnectionPtr RSPConnection::_findConnection(const uint16_t id)
{
    for (RSPConnectionsCIter i = _children.begin(); i != _children.end(); ++i)
    {
        if ((*i)->_id == id)
            return *i;
    }
    return 0;
}

bool RSPConnection::_addConnection(const uint16_t id, const uint16_t sequence)
{
    if (_findConnection(id))
        return false;

    LBDEBUG << "add connection " << id << std::endl;
    RSPConnectionPtr connection = new RSPConnection();
    connection->_id = id;
    connection->_parent = this;
    connection->_setState(STATE_CONNECTED);
    connection->_setDescription(_getDescription());
    connection->_sequence = sequence;
    LBASSERT(connection->_appBuffers.isEmpty());

    // Make all buffers available for reading
    for (BuffersCIter i = connection->_buffers.begin();
         i != connection->_buffers.end(); ++i)
    {
        Buffer* buffer = *i;
        LBCHECK(connection->_threadBuffers.push(buffer));
    }

    _children.push_back(connection);
    _sendCountNode();

    lunchbox::ScopedWrite mutex(_mutexConnection);
    _newChildren.push_back(connection);

    lunchbox::ScopedWrite mutex2(_mutexEvent);
    _event->set();
    return true;
}

void RSPConnection::_removeConnection(const uint16_t id)
{
    LBDEBUG << "remove connection " << id << std::endl;
    if (id == _id)
        return;

    for (RSPConnectionsIter i = _children.begin(); i != _children.end(); ++i)
    {
        RSPConnectionPtr child = *i;
        if (child->_id == id)
        {
            _children.erase(i);

            lunchbox::ScopedWrite mutex(child->_mutexEvent);
            child->_appBuffers.push(0);
            child->_event->set();
            break;
        }
    }

    _sendCountNode();
}

int64_t RSPConnection::write(const void* inData, const uint64_t bytes)
{
    if (_parent)
        return _parent->write(inData, bytes);

    LBASSERT(isListening());
    if (!_write)
        return -1;

    // compute number of datagrams
    uint64_t nDatagrams = bytes / _payloadSize;
    if (nDatagrams * _payloadSize != bytes)
        ++nDatagrams;

    // queue each datagram (might block if buffers are exhausted)
    const uint8_t* data = reinterpret_cast<const uint8_t*>(inData);
    const uint8_t* end = data + bytes;
    for (uint64_t i = 0; i < nDatagrams; ++i)
    {
        size_t packetSize = end - data;
        packetSize = LB_MIN(packetSize, _payloadSize);

        if (_appBuffers.isEmpty())
            // trigger processing
            _postWakeup();

        Buffer* buffer;
        if (!_appBuffers.timedPop(_writeTimeOut, buffer))
        {
            LBERROR << "Timeout while writing" << std::endl;
            buffer = 0;
        }

        if (!buffer)
        {
            close();
            return -1;
        }

        // prepare packet header (sequence is done by thread)
        DatagramData* header =
            reinterpret_cast<DatagramData*>(buffer->getData());
        header->type = DATA;
        header->size = uint16_t(packetSize);
        header->writerID = _id;

        memcpy(header + 1, data, packetSize);
        data += packetSize;

        LBCHECK(_threadBuffers.push(buffer));
    }
    _postWakeup();
    LBLOG(LOG_RSP) << "queued " << nDatagrams << " datagrams, " << bytes
                   << " bytes" << std::endl;
    return bytes;
}

void RSPConnection::finish()
{
    if (_parent.isValid())
    {
        LBASSERTINFO(!_parent, "Writes are only allowed on RSP listeners");
        return;
    }
    LBASSERT(isListening());
    _appBuffers.waitSize(_buffers.size());
}

void RSPConnection::_sendCountNode()
{
    if (!_findConnection(_id))
        return;

    LBLOG(LOG_RSP) << _children.size() << " nodes" << std::endl;
    DatagramNode count = {COUNTNODE, CO_RSP_PROTOCOL_VERSION, _id,
                          uint16_t(_children.size())};
    _write->send(boost::asio::buffer(&count, sizeof(count)));
}

void RSPConnection::_sendSimpleDatagram(const DatagramType type,
                                        const uint16_t id)
{
    DatagramNode simple = {uint16_t(type), CO_RSP_PROTOCOL_VERSION, id,
                           _sequence};
    _write->send(boost::asio::buffer(&simple, sizeof(simple)));
}

void RSPConnection::_sendAck(const uint16_t writerID, const uint16_t sequence)
{
    LBASSERT(_id != writerID);
#ifdef CO_INSTRUMENT_RSP
    ++nAcksSend;
#endif

    LBLOG(LOG_RSP) << "send ack " << sequence << std::endl;
    DatagramAck ack = {ACK, _id, writerID, sequence};
    _write->send(boost::asio::buffer(&ack, sizeof(ack)));
}

void RSPConnection::_sendNack(const uint16_t writerID, const Nack* nacks,
                              const uint16_t count)
{
    LBASSERT(count > 0);
    LBASSERT(count <= CO_RSP_MAX_NACKS);
#ifdef CO_INSTRUMENT_RSP
    ++nNAcksSend;
#endif
    /* optimization: use the direct access to the reader. */
    if (writerID == _id)
    {
        _addRepeat(nacks, count);
        return;
    }

    const size_t size =
        sizeof(DatagramNack) - (CO_RSP_MAX_NACKS - count) * sizeof(Nack);

    // set the header
    DatagramNack packet;
    packet.set(_id, writerID, count);
    memcpy(packet.nacks, nacks, count * sizeof(Nack));
    _write->send(boost::asio::buffer(&packet, size));
}

void RSPConnection::_sendAckRequest()
{
#ifdef CO_INSTRUMENT_RSP
    ++nAckRequests;
#endif
    LBLOG(LOG_RSP) << "send ack request for " << uint16_t(_sequence - 1)
                   << std::endl;
    DatagramAckRequest ackRequest = {ACKREQ, _id, uint16_t(_sequence - 1)};
    _write->send(boost::asio::buffer(&ackRequest, sizeof(DatagramAckRequest)));
}

std::ostream& operator<<(std::ostream& os, const RSPConnection& connection)
{
    os << lunchbox::disableFlush << lunchbox::disableHeader
       << "RSPConnection id " << connection.getID() << " send rate "
       << connection.getSendRate();

#ifdef CO_INSTRUMENT_RSP
    const int prec = os.precision();
    os.precision(3);

    const float time = instrumentClock.getTimef();
    const float mbps = 1048.576f * time;
    os << ": " << lunchbox::indent << std::endl
       << float(nBytesRead) / mbps << " / " << float(nBytesWritten) / mbps
       << " MB/s r/w using " << nDatagrams << " dgrams " << nRepeated
       << " repeats " << nMergedDatagrams << " merged" << std::endl;

    os.precision(prec);
    os << "sender: " << nAckRequests << " ack requests " << nAcksAccepted << "/"
       << nAcksRead << " acks " << nNAcksRead << " nacks, throttle "
       << writeWaitTime << " ms" << std::endl
       << "receiver: " << nAcksSend << " acks " << nNAcksSend << " nacks"
       << lunchbox::exdent;

    nReadData = 0;
    nBytesRead = 0;
    nBytesWritten = 0;
    nDatagrams = 0;
    nRepeated = 0;
    nMergedDatagrams = 0;
    nAckRequests = 0;
    nAcksSend = 0;
    nAcksRead = 0;
    nAcksAccepted = 0;
    nNAcksSend = 0;
    nNAcksRead = 0;
    writeWaitTime = 0.f;
#endif
    os << std::endl << lunchbox::enableHeader << lunchbox::enableFlush;

    return os;
}
}
