
/* Copyright (c) 2008-2015, Stefan Eilemann <eile@equalizergraphics.com>
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

// Tests PipeConnection throughput
// Usage: ./pipeperf

#define CO_TEST_RUNTIME 600 // seconds, needed for NighlyMemoryCheck
#include <co/buffer.h>
#include <co/connectionSet.h>
#include <co/init.h>
#include <lunchbox/clock.h>
#include <lunchbox/monitor.h>
#include <lunchbox/test.h>

#include <iostream>

#include <co/pipeConnection.h> // private header

#define MAXPACKETSIZE LB_64MB

static lunchbox::Monitor<unsigned> _nextStage;

class Sender : public lunchbox::Thread
{
public:
    explicit Sender(co::ConnectionPtr connection)
        : lunchbox::Thread()
        , _connection(connection)
    {
    }
    virtual ~Sender() {}
protected:
    virtual void run()
    {
        void* buffer = calloc(1, MAXPACKETSIZE);

        unsigned stage = 2;
        for (uint64_t packetSize = MAXPACKETSIZE; packetSize > 0;
             packetSize = packetSize >> 1)
        {
            uint32_t nPackets = 10 * MAXPACKETSIZE / packetSize;
            if (nPackets > 10000)
                nPackets = 10000;
            uint32_t i = nPackets + 1;

            while (--i)
            {
                TEST(_connection->send(buffer, packetSize));
            }
            ++_nextStage;
            _nextStage.waitGE(stage);
            stage += 2;
        }
        free(buffer);
    }

private:
    co::ConnectionPtr _connection;
};

int main(int argc, char** argv)
{
    co::init(argc, argv);
    co::PipeConnectionPtr connection = new co::PipeConnection;

    TEST(connection->connect());
    Sender sender(connection->acceptSync());
    TEST(sender.start());

    co::Buffer buffer;
    co::BufferPtr syncBuffer;
    lunchbox::Clock clock;

    unsigned stage = 2;
    for (uint64_t packetSize = MAXPACKETSIZE; packetSize > 0;
         packetSize = packetSize >> 1)
    {
        const float mBytes = packetSize / 1024.0f / 1024.0f;
        const float mBytesSec = mBytes * 1000.0f;
        uint32_t nPackets = 10 * MAXPACKETSIZE / packetSize;
        if (nPackets > 10000)
            nPackets = 10000;
        uint32_t i = nPackets + 1;

        clock.reset();
        while (--i)
        {
            buffer.setSize(0);
            connection->recvNB(&buffer, packetSize);
            TEST(connection->recvSync(syncBuffer));
            TEST(syncBuffer == &buffer);
        }
        const float time = clock.getTimef();
        if (mBytes > 0.2f)
            std::cerr << nPackets * mBytesSec / time << "MB/s, "
                      << nPackets / time << "p/ms (" << mBytes << "MB)"
                      << std::endl;
        else
            std::cerr << nPackets * mBytesSec / time << "MB/s, "
                      << nPackets / time << "p/ms (" << packetSize << "B)"
                      << std::endl;

        ++_nextStage;
        _nextStage.waitGE(stage);
        stage += 2;
    }

    TEST(sender.join());
    connection->close();

    co::exit();
    return EXIT_SUCCESS;
}
