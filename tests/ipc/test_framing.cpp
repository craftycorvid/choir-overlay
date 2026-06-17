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

    // Bogus over-cap header: a peer claims a huge total_len. The reader must declare
    // the stream dead (next() == false, failed() == true) instead of buffering
    // forever, and subsequent feed() must also report not-alive so the caller closes.
    {
        int sv2[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv2) == 0);
        int w2 = sv2[0];
        int r2 = sv2[1];

        // Raw 5-byte garbage header: total_len = 0xFFFFFFFD, type byte.
        uint8_t bogus[5] = {0xFD, 0xFF, 0xFF, 0xFF, 0x02};
        ssize_t wn = ::write(w2, bogus, sizeof(bogus));
        assert(wn == static_cast<ssize_t>(sizeof(bogus)));

        FrameReader bad_reader;
        bool fed = bad_reader.feed(r2);
        assert(fed); // got the bytes fine
        assert(bad_reader.failed() == false); // not yet decoded

        DecodedFrame bf;
        assert(bad_reader.next(bf) == false); // protocol error, not a frame
        assert(bad_reader.failed() == true);  // stream declared dead

        // Once dead, feed() also reports not-alive so the caller drops the socket.
        assert(bad_reader.feed(r2) == false);

        close(w2);
        close(r2);
    }

    return 0;
}
