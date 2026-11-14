#pragma once
#include "NetworkProtocol.h"

namespace FrostMP::Network {

class PacketSerializer {
public:
    // Serialize packet to byte buffer
    static std::vector<u8> Serialize(const PacketHeader& header, const void* payload) {
        std::vector<u8> buffer(sizeof(PacketHeader) + header.payloadSize);
        std::memcpy(buffer.data(), &header, sizeof(PacketHeader));
        if (payload && header.payloadSize > 0) {
            std::memcpy(buffer.data() + sizeof(PacketHeader), payload, header.payloadSize);
        }
        return buffer;
    }

    // Create packet with auto-filled header
    template<typename PayloadType>
    static std::vector<u8> CreatePacket(PacketType type, const PayloadType& payload,
                                        u64 senderSteamID, u32 sequence, bool reliable = true) {
        PacketHeader header{};
        header.type = type;
        header.payloadSize = static_cast<u32>(sizeof(PayloadType));
        header.senderSteamID = senderSteamID;
        header.timestampMs = GetTimestampMs();
        header.sequence = sequence;

        return Serialize(header, &payload);
    }

    // Deserialize packet from buffer
    static bool Deserialize(const u8* data, size_t size, PacketHeader& outHeader, const void*& outPayload) {
        if (size < sizeof(PacketHeader)) return false;

        std::memcpy(&outHeader, data, sizeof(PacketHeader));
        if (!ValidateHeader(outHeader)) return false;

        if (size < sizeof(PacketHeader) + outHeader.payloadSize) return false;

        outPayload = data + sizeof(PacketHeader);
        return true;
    }

    // Helper to cast payload
    template<typename PayloadType>
    static const PayloadType* GetPayload(const void* payload) {
        return static_cast<const PayloadType*>(payload);
    }
};

// Packet builder for fluent API
class PacketBuilder {
    PacketHeader header_;
    std::vector<u8> payload_;

public:
    PacketBuilder(PacketType type, u64 senderSteamID, u32 sequence) {
        header_.type = type;
        header_.senderSteamID = senderSteamID;
        header_.timestampMs = GetTimestampMs();
        header_.sequence = sequence;
    }

    template<typename T>
    PacketBuilder& AddPayload(const T& payload) {
        payload_.resize(sizeof(T));
        std::memcpy(payload_.data(), &payload, sizeof(T));
        header_.payloadSize = static_cast<u32>(payload_.size());
        return *this;
    }

    PacketBuilder& AddRawPayload(const void* data, size_t size) {
        payload_.resize(size);
        std::memcpy(payload_.data(), data, size);
        header_.payloadSize = static_cast<u32>(size);
        return *this;
    }

    std::vector<u8> Build() {
        std::vector<u8> buffer(sizeof(PacketHeader) + payload_.size());
        std::memcpy(buffer.data(), &header_, sizeof(PacketHeader));
        if (!payload_.empty()) {
            std::memcpy(buffer.data() + sizeof(PacketHeader), payload_.data(), payload_.size());
        }
        return buffer;
    }

    const PacketHeader& GetHeader() const { return header_; }
    const std::vector<u8>& GetPayload() const { return payload_; }
};

} // namespace FrostMP::Network