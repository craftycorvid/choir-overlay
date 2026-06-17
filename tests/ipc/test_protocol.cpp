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

    // Regression: integer-overflow OOB read. A header claiming total_len = 0xFFFFFFFD
    // over a tiny buffer used to wrap (4 + total == 1) and pass the bounds check,
    // then read ~4 GiB OOB. Must now be a protocol error: consume 0, set error,
    // and NOT touch buf[5..]. (Run under ASan to confirm no OOB.)
    {
        uint8_t buf[8] = {0xFD, 0xFF, 0xFF, 0xFF, 0x02, 0x00, 0x00, 0x00};
        DecodedFrame f;
        bool err = false;
        size_t consumed = try_decode_frame(buf, sizeof(buf), f, &err);
        assert(consumed == 0);
        assert(err == true);
    }

    // total_len == 0 -> protocol error (can't even hold the type byte).
    {
        uint8_t buf[8] = {0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00};
        DecodedFrame f;
        bool err = false;
        size_t consumed = try_decode_frame(buf, sizeof(buf), f, &err);
        assert(consumed == 0);
        assert(err == true);
    }

    // total_len == kMaxFrameLen + 1 -> protocol error (over the cap).
    {
        const uint32_t over = static_cast<uint32_t>(kMaxFrameLen) + 1u;
        uint8_t buf[8] = {
            static_cast<uint8_t>(over & 0xFF),
            static_cast<uint8_t>((over >> 8) & 0xFF),
            static_cast<uint8_t>((over >> 16) & 0xFF),
            static_cast<uint8_t>((over >> 24) & 0xFF),
            0x02, 0x00, 0x00, 0x00,
        };
        DecodedFrame f;
        bool err = false;
        size_t consumed = try_decode_frame(buf, sizeof(buf), f, &err);
        assert(consumed == 0);
        assert(err == true);
    }

    // total_len == kMaxFrameLen is at the boundary: NOT a protocol error. With only a
    // small buffer it just means "need more bytes" (consume 0, no error).
    {
        const uint32_t cap = static_cast<uint32_t>(kMaxFrameLen);
        uint8_t buf[8] = {
            static_cast<uint8_t>(cap & 0xFF),
            static_cast<uint8_t>((cap >> 8) & 0xFF),
            static_cast<uint8_t>((cap >> 16) & 0xFF),
            static_cast<uint8_t>((cap >> 24) & 0xFF),
            0x02, 0x00, 0x00, 0x00,
        };
        DecodedFrame f;
        bool err = false;
        size_t consumed = try_decode_frame(buf, sizeof(buf), f, &err);
        assert(consumed == 0);
        assert(err == false); // just needs more bytes, not malformed
    }

    return 0;
}
