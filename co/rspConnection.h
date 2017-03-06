
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

#ifndef CO_RSPCONNECTION_H
#define CO_RSPCONNECTION_H

#include <co/connection.h>      // base class
#include <co/eventConnection.h> // member

#include <lunchbox/buffer.h>  // member
#include <lunchbox/clock.h>   // member
#include <lunchbox/lfQueue.h> // member
#include <lunchbox/mtQueue.h> // member

#pragma warning(push)
#pragma warning(disable : 4267)
#include <boost/asio.hpp>
#pragma warning(pop)

namespace co
{
class RSPConnection;
typedef lunchbox::RefPtr<RSPConnection> RSPConnectionPtr;

/**
 * A reliable multicast connection.
 *
 * This connection implements a reliable stream protocol (RSP) over IP V4
 * UDP multicast. The <a
 * href="http://www.equalizergraphics.com/documents/design/multicast.html#RSP">RSP
 * design document</a> describes the high-level protocol.
 */
class RSPConnection : public Connection
{
public:
    /** Create a new RSP-based connection. */
    RSPConnection();

    bool listen() override;
    void close() override;

    /** Identical to listen() for multicast connections. */
    bool connect() override { return listen(); }
    void acceptNB() override { LBASSERT(isListening()); }
    ConnectionPtr acceptSync() override;
    void readNB(void*, const uint64_t) override { /* NOP */}
    int64_t readSync(void* buffer, const uint64_t bytes,
                     const bool ignored) override;
    int64_t write(const void* buffer, const uint64_t bytes) override;

    /** @internal Finish all pending send operations. */
    void finish() override;

    /** @internal @return current send speed in kilobyte per second. */
    int64_t getSendRate() const { return _sendRate; }
    /**
     * @internal
     * @return the unique identifier of this connection within the multicast
     *         group.
     */
    uint16_t getID() const { return _id; }
    Notifier getNotifier() const override { return _event->getNotifier(); }
protected:
    virtual ~RSPConnection();

private:
    /** Thread managing network IO and RSP protocol. */
    class Thread : public lunchbox::Thread
    {
    public:
        explicit Thread(RSPConnectionPtr connection)
            : _connection(connection)
        {
        }
        virtual ~Thread() { _connection = 0; }
    protected:
        void run() override;
        bool init() override { return _connection->_initThread(); }
    private:
        RSPConnectionPtr _connection;
    };

    /** The type of each UDP packet */
    enum DatagramType
    {
        DATA,           //!< the datagram contains data
        ACKREQ,         //!< ask for ack from all readers
        NACK,           //!< negative ack, request missing packets
        ACK,            //!< positive ack all data
        ID_HELLO,       //!< announce a new id
        ID_HELLO_REPLY, //!< reply to hello, transmit cur packet number
        ID_DENY,        //!< deny the id, already used
        ID_CONFIRM,     //!< a new node is connected
        ID_EXIT,        //!< a node is disconnected
        COUNTNODE //!< send to other the number of nodes which I have found
        // NOTE: Do not use more than 255 types here, since the endianness
        // detection magic relies on only using the LSB.
    };

    /** ID_HELLO, ID_DENY, ID_CONFIRM, ID_EXIT or COUNTNODE packet */
    struct DatagramNode
    {
        uint16_t type;
        uint16_t protocolVersion;
        uint16_t connectionID; // clientID for type COUNTNODE
        uint16_t data;
    };

    /** Request receive confirmation of all packets up to sequence. */
    struct DatagramAckRequest
    {
        uint16_t type;
        uint16_t writerID;
        uint16_t sequence;
    };

    /** Missing packets from start..end sequence */
    struct Nack
    {
        uint16_t start;
        uint16_t end;
    };

#define CO_RSP_MAX_NACKS 300 // fits in a single IP frame
    /** Request resend of lost packets */
    struct DatagramNack
    {
        void set(uint16_t rID, uint16_t wID, uint16_t n)
        {
            type = NACK;
            readerID = rID;
            writerID = wID;
            count = n;
        }

        uint16_t type;
        uint16_t readerID;
        uint16_t writerID;
        uint16_t count; //!< number of NACK requests used
        Nack nacks[CO_RSP_MAX_NACKS];
    };

    /** Acknowledge reception of all packets including sequence .*/
    struct DatagramAck
    {
        uint16_t type;
        uint16_t readerID;
        uint16_t writerID;
        uint16_t sequence;
    };

    /** Data packet */
    struct DatagramData
    {
        uint16_t type;
        uint16_t size;
        uint16_t writerID;
        uint16_t sequence;
    };

    typedef std::vector<RSPConnectionPtr> RSPConnections;
    typedef RSPConnections::iterator RSPConnectionsIter;
    typedef RSPConnections::const_iterator RSPConnectionsCIter;

    RSPConnectionPtr _parent;
    RSPConnections _children;

    // a link for all connection in the connecting state
    RSPConnections _newChildren;

    uint16_t _id; //!< The identifier used to demultiplex multipe writers
    bool _idAccepted;
    int32_t _mtu;
    int32_t _ackFreq;
    uint32_t _payloadSize;
    int32_t _timeouts;

    typedef lunchbox::RefPtr<EventConnection> EventConnectionPtr;
    EventConnectionPtr _event;

    boost::asio::io_service _ioService;
    boost::asio::ip::udp::socket* _read;
    boost::asio::ip::udp::socket* _write;
    boost::asio::ip::udp::endpoint _readAddr;
    boost::asio::deadline_timer _timeout;
    boost::asio::deadline_timer _wakeup;

    lunchbox::Clock _clock;
    uint64_t _maxBucketSize;
    size_t _bucketSize;
    int64_t _sendRate;

    Thread* _thread;
    std::mutex _mutexConnection;
    std::mutex _mutexEvent;
    uint16_t _acked; // sequence ID of last confirmed ack

    typedef lunchbox::Bufferb Buffer;
    typedef std::vector<Buffer*> Buffers;
    typedef Buffers::iterator BuffersIter;
    typedef Buffers::const_iterator BuffersCIter;

    Buffers _buffers; //!< Data buffers
    /** Empty read buffers (connected) or write buffers (listening) */
    lunchbox::LFQueue<Buffer*> _threadBuffers;
    /** Ready data buffers (connected) or empty write buffers (listening) */
    lunchbox::MTQueue<Buffer*> _appBuffers;

    Buffer _recvBuffer;               //!< Receive (thread) buffer
    std::deque<Buffer*> _recvBuffers; //!< out-of-order buffers

    Buffer* _readBuffer;     //!< Read (app) buffer
    uint64_t _readBufferPos; //!< Current read index

    uint16_t _sequence; //!< the next usable (write) or expected (read)
    std::deque<Buffer*> _writeBuffers; //!< Written buffers, not acked

    typedef std::deque<Nack> RepeatQueue;
    RepeatQueue _repeatQueue; //!< nacks to repeat

    const unsigned _writeTimeOut;

    uint16_t _buildNewID();

    void _processOutgoing();
    void _writeData();
    void _repeatData();
    void _finishWriteQueue(const uint16_t sequence);

    bool _handleData(const size_t bytes);
    bool _handleAck(const size_t bytes);
    bool _handleNack();
    bool _handleAckRequest(const size_t bytes);

    Buffer* _newDataBuffer(Buffer& inBuffer);
    void _pushDataBuffer(Buffer* buffer);

    /* Run the reader thread */
    void _runThread();

    /* init the reader thread */
    bool _initThread();
    /* Make all buffers available for reading */
    void initBuffers();
    /* handle data about the comunication state */
    void _handlePacket(const boost::system::error_code& error,
                       const size_t bytes);
    void _handleConnectedData(const size_t bytes);
    void _handleInitData(const size_t bytes, const bool connected);
    void _handleAcceptIDData(const size_t bytes);

    /* handle timeout about the comunication state */
    void _handleTimeout(const boost::system::error_code& error);
    void _handleConnectedTimeout();
    void _handleInitTimeout();
    void _handleAcceptIDTimeout();

    void _clearWriteQueues();

    DatagramNode* _getDatagramNode(const size_t bytes);

    /** find the connection corresponding to the identifier */
    RSPConnectionPtr _findConnection(const uint16_t id);

    /** Sleep until allowed to send according to send rate */
    void _waitWritable(const uint64_t bytes);

    /** format and send a datagram count node */
    void _sendCountNode();

    void _addRepeat(const Nack* nacks, const uint16_t num);

    /** format and send an simple request which use only type and id field*/
    void _sendSimpleDatagram(const DatagramType type, const uint16_t id);

    /** format and send an ack request for the current sequence */
    void _sendAckRequest();

    /** format and send a positive ack */
    void _sendAck(const uint16_t writerID, const uint16_t sequence);

    /** format and send a negative ack */
    void _sendNack(const uint16_t toWriterID, const Nack* nacks,
                   const uint16_t num);

    void _checkNewID(const uint16_t id);

    /* add a new connection detected in the multicast network */
    bool _addConnection(const uint16_t id, const uint16_t sequence);
    void _removeConnection(const uint16_t id);

    void _setTimeout(const int32_t timeOut);
    void _postWakeup();
    void _asyncReceiveFrom();
    bool _isWriting() const
    {
        return !_threadBuffers.isEmpty() || !_writeBuffers.empty();
    }
};

std::ostream& operator<<(std::ostream&, const RSPConnection&);
}

#endif // CO_RSPCONNECTION_H
