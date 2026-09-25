#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace rabbit::protocol {

constexpr std::uint8_t kProtocolVersion = 1;
constexpr std::size_t kMaxPayloadBytes = 256;
constexpr std::size_t kMaxEncodedFrameBytes = 320;
constexpr std::size_t kMaxPlanBytes = 12 * 1024;

enum class MessageType : std::uint8_t {
    Hello = 1,
    Heartbeat = 2,
    PrepareBegin = 3,
    PrepareChunk = 4,
    PrepareCommit = 5,
    Start = 6,
    Stop = 7,
    Status = 8,
    Ack = 9,
    Reject = 10,
};

enum class DecodeError : std::uint8_t { None, Overflow, Cobs, Header, Crc };

struct Frame {
    MessageType type{MessageType::Hello};
    std::uint32_t session_id{0};
    std::uint32_t request_id{0};
    std::array<std::uint8_t, kMaxPayloadBytes> payload{};
    std::uint16_t payload_size{0};
};

struct EncodedFrame {
    std::array<std::uint8_t, kMaxEncodedFrameBytes> bytes{};
    std::size_t size{0};
};

[[nodiscard]] std::uint32_t crc32(const std::uint8_t* data, std::size_t size);
[[nodiscard]] bool encode(const Frame& frame, EncodedFrame& output);

class FrameDecoder {
public:
    [[nodiscard]] bool push(std::uint8_t byte, Frame& output, DecodeError& error);
    void reset();

private:
    std::array<std::uint8_t, kMaxEncodedFrameBytes> encoded_{};
    std::size_t size_{0};
};

enum class TransferError : std::uint8_t { None, Busy, Bounds, Sequence, Crc, Incomplete };

class PlanTransfer {
public:
    [[nodiscard]] TransferError begin(std::uint32_t id, std::uint16_t total_size, std::uint32_t expected_crc);
    [[nodiscard]] TransferError append(std::uint32_t id, std::uint16_t offset, const std::uint8_t* data, std::uint16_t size);
    [[nodiscard]] TransferError commit(std::uint32_t id, const std::uint8_t*& data, std::uint16_t& size);
    void abort();

private:
    std::array<std::uint8_t, kMaxPlanBytes> data_{};
    std::uint32_t id_{0};
    std::uint32_t expected_crc_{0};
    std::uint16_t total_size_{0};
    std::uint16_t received_size_{0};
    bool active_{false};
};

}  // namespace rabbit::protocol
