#include "ipc/protocol.hpp"
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

int main() {
    using namespace choir;

    // Encode then decode recovers the same (type, payload).
    {
        std::string payload = "{\"x\":1}";
        std::vector<uint8_t> out;
        encode_frame(MsgType::Snapshot, payload, out);

        // Frame = [uint32 LE total_len][uint8 type][payload].
        // total_len = 1 (type byte) + payload.size().
        assert(out.size() == 4 + 1 + payload.size());

        DecodedFrame f;
        size_t consumed = try_decode_frame(out.data(), out.size(), f);
        assert(consumed == out.size());
        assert(f.type == MsgType::Snapshot);
        assert(f.payload == payload);
    }

    // Partial buffer (only first 3 bytes) -> consume 0.
    {
        std::string payload = "{\"x\":1}";
        std::vector<uint8_t> out;
        encode_frame(MsgType::Snapshot, payload, out);

        DecodedFrame f;
        size_t consumed = try_decode_frame(out.data(), 3, f);
        assert(consumed == 0);

        // Header present but payload incomplete -> still 0.
        consumed = try_decode_frame(out.data(), 5, f);
        assert(consumed == 0);
    }

    // Two concatenated frames decode correctly, in order.
    {
        std::vector<uint8_t> buf;
        encode_frame(MsgType::Ping, "first", buf);
        size_t boundary = buf.size();
        encode_frame(MsgType::Pong, "second", buf);

        DecodedFrame f1;
        size_t c1 = try_decode_frame(buf.data(), buf.size(), f1);
        assert(c1 == boundary);
        assert(f1.type == MsgType::Ping);
        assert(f1.payload == "first");

        DecodedFrame f2;
        size_t c2 = try_decode_frame(buf.data() + c1, buf.size() - c1, f2);
        assert(c2 == buf.size() - boundary);
        assert(f2.type == MsgType::Pong);
        assert(f2.payload == "second");
    }

    // Empty payload round-trips (total_len == 1).
    {
        std::vector<uint8_t> out;
        encode_frame(MsgType::Disabled, "", out);
        assert(out.size() == 5);
        DecodedFrame f;
        size_t consumed = try_decode_frame(out.data(), out.size(), f);
        assert(consumed == 5);
        assert(f.type == MsgType::Disabled);
        assert(f.payload == "");
    }

    return 0;
}
