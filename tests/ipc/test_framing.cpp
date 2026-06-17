#include "ipc/framing.hpp"
#include "ipc/protocol.hpp"
#include <cassert>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

int main() {
    using namespace choir;

    int sv[2];
    int rc = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    assert(rc == 0);

    int wfd = sv[0];
    int rfd = sv[1];

    // Write two frames on one end.
    assert(write_frame(wfd, MsgType::Hello, "{\"pid\":42}"));
    assert(write_frame(wfd, MsgType::Snapshot, "{\"revision\":1}"));

    // Read them on the other end via FrameReader, tolerating short reads.
    FrameReader reader;

    DecodedFrame f1;
    while (!reader.next(f1)) {
        bool alive = reader.feed(rfd);
        assert(alive);
    }
    assert(f1.type == MsgType::Hello);
    assert(f1.payload == "{\"pid\":42}");

    DecodedFrame f2;
    while (!reader.next(f2)) {
        bool alive = reader.feed(rfd);
        assert(alive);
    }
    assert(f2.type == MsgType::Snapshot);
    assert(f2.payload == "{\"revision\":1}");

    // Larger payload to exercise multi-chunk reads.
    std::string big(100000, 'z');
    assert(write_frame(wfd, MsgType::Snapshot, big));

    DecodedFrame f3;
    while (!reader.next(f3)) {
        bool alive = reader.feed(rfd);
        assert(alive);
    }
    assert(f3.type == MsgType::Snapshot);
    assert(f3.payload == big);

    // EOF: closing the writer should make feed() report not-alive once drained.
    close(wfd);
    DecodedFrame f4;
    assert(reader.next(f4) == false); // nothing buffered
    bool alive = reader.feed(rfd);
    assert(alive == false); // EOF

    close(rfd);
    return 0;
}
