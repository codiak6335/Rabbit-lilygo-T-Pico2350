#include "rabbit/protocol/framing.hpp"

#include <algorithm>
#include <cstring>

namespace rabbit::protocol {
namespace {
constexpr std::size_t kHeaderSize = 13;
constexpr std::size_t kCrcSize = 4;

void write_u16(std::uint8_t* destination, const std::uint16_t value) {
    destination[0] = static_cast<std::uint8_t>(value >> 8U);
    destination[1] = static_cast<std::uint8_t>(value);
}

void write_u32(std::uint8_t* destination, const std::uint32_t value) {
    destination[0] = static_cast<std::uint8_t>(value >> 24U);
    destination[1] = static_cast<std::uint8_t>(value >> 16U);
    destination[2] = static_cast<std::uint8_t>(value >> 8U);
    destination[3] = static_cast<std::uint8_t>(value);
}

std::uint16_t read_u16(const std::uint8_t* source) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(source[0]) << 8U) | source[1]);
}

std::uint32_t read_u32(const std::uint8_t* source) {
    return (static_cast<std::uint32_t>(source[0]) << 24U) | (static_cast<std::uint32_t>(source[1]) << 16U) |
        (static_cast<std::uint32_t>(source[2]) << 8U) | source[3];
}

bool cobs_decode(const std::uint8_t* encoded, const std::size_t encoded_size, std::uint8_t* decoded, std::size_t& decoded_size) {
    std::size_t read = 0;
    std::size_t write = 0;
    while (read < encoded_size) {
        const auto code = encoded[read++];
        if (code == 0 || read + code - 1 > encoded_size) return false;
        for (std::uint8_t index = 1; index < code; ++index) decoded[write++] = encoded[read++];
        if (code != 0xFF && read < encoded_size) decoded[write++] = 0;
    }
    decoded_size = write;
    return true;
}
}  // namespace

std::uint32_t crc32(const std::uint8_t* data, const std::size_t size) {
    std::uint32_t result = 0xFFFFFFFFU;
    for (std::size_t index = 0; index < size; ++index) {
        result ^= data[index];
        for (std::uint8_t bit = 0; bit < 8; ++bit) {
            result = (result >> 1U) ^ ((result & 1U) == 0U ? 0U : 0xEDB88320U);
        }
    }
    return ~result;
}

bool encode(const Frame& frame, EncodedFrame& output) {
    if (frame.payload_size > kMaxPayloadBytes) return false;
    std::array<std::uint8_t, kMaxEncodedFrameBytes> raw{};
    const auto raw_size = kHeaderSize + frame.payload_size + kCrcSize;
    raw[0] = kProtocolVersion;
    raw[1] = static_cast<std::uint8_t>(frame.type);
    write_u32(&raw[2], frame.session_id);
    write_u32(&raw[6], frame.request_id);
    write_u16(&raw[10], frame.payload_size);
    raw[12] = 0;
    std::memcpy(&raw[kHeaderSize], frame.payload.data(), frame.payload_size);
    write_u32(&raw[kHeaderSize + frame.payload_size], crc32(raw.data(), kHeaderSize + frame.payload_size));

    std::size_t read = 0;
    std::size_t write = 1;
    std::size_t code_index = 0;
    std::uint8_t code = 1;
    while (read < raw_size) {
        if (write >= output.bytes.size()) return false;
        if (raw[read] == 0) {
            output.bytes[code_index] = code;
            code = 1;
            code_index = write++;
            ++read;
        } else {
            output.bytes[write++] = raw[read++];
            ++code;
            if (code == 0xFF) {
                output.bytes[code_index] = code;
                code = 1;
                code_index = write++;
            }
        }
    }
    output.bytes[code_index] = code;
    if (write >= output.bytes.size()) return false;
    output.bytes[write++] = 0;
    output.size = write;
    return true;
}

bool FrameDecoder::push(const std::uint8_t byte, Frame& output, DecodeError& error) {
    error = DecodeError::None;
    if (byte != 0) {
        if (size_ >= encoded_.size()) {
            reset();
            error = DecodeError::Overflow;
        } else {
            encoded_[size_++] = byte;
        }
        return false;
    }
    if (size_ == 0) return false;
    std::array<std::uint8_t, kMaxEncodedFrameBytes> raw{};
    std::size_t raw_size = 0;
    const bool decoded = cobs_decode(encoded_.data(), size_, raw.data(), raw_size);
    reset();
    if (!decoded) {
        error = DecodeError::Cobs;
        return false;
    }
    if (raw_size < kHeaderSize + kCrcSize || raw[0] != kProtocolVersion) {
        error = DecodeError::Header;
        return false;
    }
    const auto payload_size = read_u16(&raw[10]);
    if (payload_size > kMaxPayloadBytes || raw_size != kHeaderSize + payload_size + kCrcSize) {
        error = DecodeError::Header;
        return false;
    }
    const auto expected = read_u32(&raw[kHeaderSize + payload_size]);
    if (crc32(raw.data(), kHeaderSize + payload_size) != expected) {
        error = DecodeError::Crc;
        return false;
    }
    output.type = static_cast<MessageType>(raw[1]);
    output.session_id = read_u32(&raw[2]);
    output.request_id = read_u32(&raw[6]);
    output.payload_size = payload_size;
    std::memcpy(output.payload.data(), &raw[kHeaderSize], payload_size);
    return true;
}

void FrameDecoder::reset() { size_ = 0; }

TransferError PlanTransfer::begin(const std::uint32_t id, const std::uint16_t total_size, const std::uint32_t expected_crc) {
    if (active_) return TransferError::Busy;
    if (total_size == 0 || total_size > data_.size()) return TransferError::Bounds;
    id_ = id;
    total_size_ = total_size;
    received_size_ = 0;
    expected_crc_ = expected_crc;
    active_ = true;
    return TransferError::None;
}

TransferError PlanTransfer::append(
    const std::uint32_t id, const std::uint16_t offset, const std::uint8_t* data, const std::uint16_t size) {
    if (!active_ || id != id_ || data == nullptr) return TransferError::Sequence;
    if (offset != received_size_) return TransferError::Sequence;
    if (size > total_size_ - received_size_) return TransferError::Bounds;
    std::memcpy(&data_[received_size_], data, size);
    received_size_ = static_cast<std::uint16_t>(received_size_ + size);
    return TransferError::None;
}

TransferError PlanTransfer::commit(const std::uint32_t id, const std::uint8_t*& data, std::uint16_t& size) {
    if (!active_ || id != id_) return TransferError::Sequence;
    if (received_size_ != total_size_) return TransferError::Incomplete;
    if (crc32(data_.data(), total_size_) != expected_crc_) {
        abort();
        return TransferError::Crc;
    }
    data = data_.data();
    size = total_size_;
    active_ = false;
    return TransferError::None;
}

void PlanTransfer::abort() {
    active_ = false;
    received_size_ = 0;
    total_size_ = 0;
    expected_crc_ = 0;
}

}  // namespace rabbit::protocol
