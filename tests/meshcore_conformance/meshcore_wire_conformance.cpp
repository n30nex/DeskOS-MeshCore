#include "meshcore_packet_checked.hpp"

extern "C" {
#include "mesh/meshcore_wire.h"
}

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kGuardSize = 16;
constexpr uint8_t kCanary = 0xD7;

struct PathCase {
    uint8_t hash_size;
    uint8_t hash_count;
};

constexpr std::array<uint8_t, 8> kPayloadTypes = {
    PAYLOAD_TYPE_TXT_MSG,
    PAYLOAD_TYPE_ACK,
    PAYLOAD_TYPE_ADVERT,
    PAYLOAD_TYPE_GRP_TXT,
    PAYLOAD_TYPE_PATH,
    PAYLOAD_TYPE_TRACE,
    PAYLOAD_TYPE_MULTIPART,
    PAYLOAD_TYPE_CONTROL,
};
constexpr std::array<uint8_t, 4> kRouteTypes = {
    ROUTE_TYPE_TRANSPORT_FLOOD,
    ROUTE_TYPE_FLOOD,
    ROUTE_TYPE_DIRECT,
    ROUTE_TYPE_TRANSPORT_DIRECT,
};
constexpr std::array<PathCase, 9> kPathCases = {{
    {1, 0}, {1, 1}, {1, 63},
    {2, 0}, {2, 1}, {2, 32},
    {3, 0}, {3, 1}, {3, 21},
}};
constexpr std::array<uint16_t, 2> kPayloadLengths = {1, 184};
constexpr std::size_t kExpectedPerDirection =
    kPayloadTypes.size() * kRouteTypes.size() * kPathCases.size() *
    kPayloadLengths.size();
static_assert(kExpectedPerDirection == 576, "The required vector matrix changed");

template <std::size_t Capacity>
class GuardedBuffer {
  public:
    GuardedBuffer() { bytes_.fill(kCanary); }

    uint8_t *data() { return bytes_.data() + kGuardSize; }
    const uint8_t *data() const { return bytes_.data() + kGuardSize; }

    bool guards_intact() const
    {
        for (std::size_t i = 0; i < kGuardSize; ++i) {
            if (bytes_[i] != kCanary ||
                bytes_[kGuardSize + Capacity + i] != kCanary) {
                return false;
            }
        }
        return true;
    }

    bool data_region_unchanged() const
    {
        for (std::size_t i = 0; i < Capacity; ++i) {
            if (bytes_[kGuardSize + i] != kCanary) {
                return false;
            }
        }
        return true;
    }

  private:
    std::array<uint8_t, Capacity + (2 * kGuardSize)> bytes_{};
};

uint8_t encoded_path_len(const PathCase &path_case)
{
    return static_cast<uint8_t>(((path_case.hash_size - 1U) << 6U) |
                                path_case.hash_count);
}

std::vector<uint8_t> make_bytes(std::size_t size, uint32_t salt)
{
    std::vector<uint8_t> result(size);
    for (std::size_t i = 0; i < size; ++i) {
        result[i] = static_cast<uint8_t>((salt + (i * 37U) + (i >> 1U)) & 0xFFU);
    }
    return result;
}

void add_failure(std::vector<std::string> &failures,
                 std::size_t vector_index,
                 const std::string &direction,
                 const std::string &detail)
{
    std::ostringstream message;
    message << direction << " vector " << vector_index << ": " << detail;
    failures.push_back(message.str());
}

bool transport_route(uint8_t route)
{
    return route == ROUTE_TYPE_TRANSPORT_FLOOD ||
           route == ROUTE_TYPE_TRANSPORT_DIRECT;
}

void verify_upstream_to_local(std::vector<std::string> &failures,
                              std::size_t vector_index,
                              uint8_t type,
                              uint8_t route,
                              const PathCase &path_case,
                              uint16_t payload_len)
{
    mesh::Packet upstream;
    upstream.header = static_cast<uint8_t>((type << PH_TYPE_SHIFT) | route);
    upstream.transport_codes[0] = static_cast<uint16_t>(0x1200U + vector_index);
    upstream.transport_codes[1] = static_cast<uint16_t>(0xA500U ^ vector_index);
    upstream.setPathHashSizeAndCount(path_case.hash_size, path_case.hash_count);
    const std::size_t path_bytes = path_case.hash_size * path_case.hash_count;
    const auto path = make_bytes(path_bytes, static_cast<uint32_t>(vector_index + 11U));
    const auto payload = make_bytes(payload_len, static_cast<uint32_t>(vector_index + 97U));
    if (path_bytes > 0U) {
        std::memcpy(upstream.path, path.data(), path_bytes);
    }
    std::memcpy(upstream.payload, payload.data(), payload.size());
    upstream.payload_len = payload_len;

    GuardedBuffer<MAX_TRANS_UNIT> raw;
    const uint8_t raw_len = upstream.writeTo(raw.data());
    if (!raw.guards_intact()) {
        add_failure(failures, vector_index, "upstream_to_local",
                    "Packet::writeTo crossed a canary");
        return;
    }
    if (raw_len != upstream.getRawLength()) {
        add_failure(failures, vector_index, "upstream_to_local",
                    "Packet::writeTo length differs from getRawLength");
        return;
    }

    d1l_meshcore_wire_packet_t decoded{};
    if (!d1l_meshcore_wire_decode(raw.data(), raw_len, &decoded)) {
        add_failure(failures, vector_index, "upstream_to_local",
                    "production decoder rejected a pinned Packet encoding");
        return;
    }
    if (decoded.header != upstream.header || decoded.route != route ||
        decoded.type != type || decoded.version != 0U ||
        decoded.path_len != upstream.path_len ||
        decoded.path_hash_bytes != path_case.hash_size ||
        decoded.path_hops != path_case.hash_count ||
        decoded.path_byte_len != path_bytes || decoded.payload_len != payload_len) {
        add_failure(failures, vector_index, "upstream_to_local",
                    "decoded scalar fields differ");
        return;
    }
    if (transport_route(route) &&
        (decoded.transport_codes[0] != upstream.transport_codes[0] ||
         decoded.transport_codes[1] != upstream.transport_codes[1])) {
        add_failure(failures, vector_index, "upstream_to_local",
                    "decoded transport codes differ");
        return;
    }
    if ((path_bytes > 0U &&
         std::memcmp(decoded.path, path.data(), path_bytes) != 0) ||
        std::memcmp(decoded.payload, payload.data(), payload.size()) != 0) {
        add_failure(failures, vector_index, "upstream_to_local",
                    "decoded path or payload differs");
    }
}

void verify_local_to_upstream(std::vector<std::string> &failures,
                              std::size_t vector_index,
                              uint8_t type,
                              uint8_t route,
                              const PathCase &path_case,
                              uint16_t payload_len)
{
    const uint8_t header = static_cast<uint8_t>((type << PH_TYPE_SHIFT) | route);
    const uint8_t path_len = encoded_path_len(path_case);
    const std::size_t path_bytes = path_case.hash_size * path_case.hash_count;
    const auto path = make_bytes(path_bytes, static_cast<uint32_t>(vector_index + 23U));
    const auto payload = make_bytes(payload_len, static_cast<uint32_t>(vector_index + 131U));
    d1l_meshcore_wire_packet_t local{};
    local.header = header;
    local.transport_codes[0] = static_cast<uint16_t>(0x2300U + vector_index);
    local.transport_codes[1] = static_cast<uint16_t>(0x5A00U ^ vector_index);
    local.path_len = path_len;
    local.path = path.empty() ? nullptr : path.data();
    local.payload = payload.data();
    local.payload_len = payload_len;

    GuardedBuffer<MAX_TRANS_UNIT> raw;
    std::size_t raw_len = 0U;
    if (!d1l_meshcore_wire_encode(&local, raw.data(), MAX_TRANS_UNIT, &raw_len)) {
        add_failure(failures, vector_index, "local_to_upstream",
                    "production encoder rejected a valid matrix vector");
        return;
    }
    if (!raw.guards_intact() || raw_len > MAX_TRANS_UNIT) {
        add_failure(failures, vector_index, "local_to_upstream",
                    "production encoder crossed a canary or MTU");
        return;
    }

    mesh::Packet decoded;
    if (!decoded.readFrom(raw.data(), static_cast<uint8_t>(raw_len))) {
        add_failure(failures, vector_index, "local_to_upstream",
                    "pinned Packet::readFrom rejected production encoding");
        return;
    }
    if (decoded.header != header || decoded.getRouteType() != route ||
        decoded.getPayloadType() != type || decoded.getPayloadVer() != 0U ||
        decoded.path_len != path_len || decoded.getPathHashSize() != path_case.hash_size ||
        decoded.getPathHashCount() != path_case.hash_count ||
        decoded.getPathByteLen() != path_bytes || decoded.payload_len != payload_len) {
        add_failure(failures, vector_index, "local_to_upstream",
                    "pinned Packet decoded scalar fields differently");
        return;
    }
    if (transport_route(route) &&
        (decoded.transport_codes[0] != local.transport_codes[0] ||
         decoded.transport_codes[1] != local.transport_codes[1])) {
        add_failure(failures, vector_index, "local_to_upstream",
                    "pinned Packet decoded transport codes differently");
        return;
    }
    if ((path_bytes > 0U && std::memcmp(decoded.path, path.data(), path_bytes) != 0) ||
        std::memcmp(decoded.payload, payload.data(), payload.size()) != 0) {
        add_failure(failures, vector_index, "local_to_upstream",
                    "pinned Packet decoded path or payload differently");
    }
}

void expect_decode_rejected(std::vector<std::string> &failures,
                            const std::string &name,
                            const uint8_t *raw,
                            std::size_t size)
{
    d1l_meshcore_wire_packet_t packet;
    std::memset(&packet, 0xA5, sizeof(packet));
    d1l_meshcore_wire_packet_t before;
    std::memcpy(&before, &packet, sizeof(packet));
    if (d1l_meshcore_wire_decode(raw, size, &packet)) {
        failures.push_back("malformed decode accepted: " + name);
    } else if (std::memcmp(&before, &packet, sizeof(packet)) != 0) {
        failures.push_back("malformed decode changed output: " + name);
    }
}

std::size_t verify_malformed_decodes(std::vector<std::string> &failures)
{
    const uint8_t flood_header =
        static_cast<uint8_t>((PAYLOAD_TYPE_TXT_MSG << PH_TYPE_SHIFT) | ROUTE_TYPE_FLOOD);
    const uint8_t transport_header = static_cast<uint8_t>(
        (PAYLOAD_TYPE_TXT_MSG << PH_TYPE_SHIFT) | ROUTE_TYPE_TRANSPORT_FLOOD);
    const std::vector<std::pair<std::string, std::vector<uint8_t>>> cases = {
        {"length_0", {}},
        {"length_1", {flood_header}},
        {"length_2", {flood_header, 0x00}},
        {"transport_truncated_3", {transport_header, 0x01, 0x02}},
        {"transport_truncated_4", {transport_header, 0x01, 0x02, 0x03}},
        {"transport_truncated_5", {transport_header, 0x01, 0x02, 0x03, 0x04}},
        {"transport_no_payload", {transport_header, 0x01, 0x02, 0x03, 0x04, 0x00}},
        {"reserved_path_size_4", {flood_header, 0xC0, 0x01}},
        {"path_size_2_count_33", {flood_header, 0x61, 0x01}},
        {"path_size_3_count_22", {flood_header, 0x96, 0x01}},
        {"truncated_path", {flood_header, 0x02, 0xA1}},
        {"path_ending_without_payload", {flood_header, 0x02, 0xA1, 0xB2}},
    };
    for (const auto &item : cases) {
        expect_decode_rejected(failures, item.first, item.second.data(), item.second.size());
    }
    expect_decode_rejected(failures, "null_source", nullptr, 7U);

    std::vector<uint8_t> payload_185(187U, 0x44);
    payload_185[0] = flood_header;
    payload_185[1] = 0x00;
    expect_decode_rejected(failures, "payload_185", payload_185.data(), payload_185.size());

    std::vector<uint8_t> raw_256(256U, 0x44);
    raw_256[0] = flood_header;
    raw_256[1] = 0x00;
    expect_decode_rejected(failures, "raw_256", raw_256.data(), raw_256.size());
    return cases.size() + 3U;
}

std::size_t verify_malformed_encodes(std::vector<std::string> &failures)
{
    constexpr std::size_t kCaseCount = 7U;
    const uint8_t header =
        static_cast<uint8_t>((PAYLOAD_TYPE_TXT_MSG << PH_TYPE_SHIFT) | ROUTE_TYPE_FLOOD);
    std::array<uint8_t, 185> payload{};
    std::array<uint8_t, 2> path = {0x11, 0x22};
    GuardedBuffer<MAX_TRANS_UNIT> raw;
    d1l_meshcore_wire_packet_t packet{};
    packet.header = header;
    packet.path_len = 0U;
    packet.payload = payload.data();
    packet.payload_len = 1U;

    auto rejected = [&](const std::string &name,
                        const d1l_meshcore_wire_packet_t *candidate,
                        uint8_t *dest,
                        std::size_t capacity) {
        std::size_t out_len = 0xBEEFU;
        if (d1l_meshcore_wire_encode(candidate, dest, capacity, &out_len)) {
            failures.push_back("malformed encode accepted: " + name);
        } else if (out_len != 0xBEEFU) {
            failures.push_back("malformed encode changed output length: " + name);
        }
        if (!raw.guards_intact() || !raw.data_region_unchanged()) {
            failures.push_back("malformed encode changed output or crossed a canary: " + name);
        }
    };

    rejected("null_packet", nullptr, raw.data(), MAX_TRANS_UNIT);
    rejected("null_destination", &packet, nullptr, MAX_TRANS_UNIT);

    packet.payload_len = 0U;
    rejected("zero_payload", &packet, raw.data(), MAX_TRANS_UNIT);
    packet.payload_len = 185U;
    rejected("payload_185", &packet, raw.data(), MAX_TRANS_UNIT);
    packet.payload_len = 1U;
    packet.path_len = 0xC0U;
    rejected("reserved_path_size_4", &packet, raw.data(), MAX_TRANS_UNIT);
    packet.path_len = 2U;
    packet.path = nullptr;
    rejected("missing_path", &packet, raw.data(), MAX_TRANS_UNIT);
    packet.path = path.data();
    rejected("destination_too_small", &packet, raw.data(), 1U);
    return kCaseCount;
}

struct PathEncodingSweepResult {
    std::size_t tested = 0U;
    std::size_t valid = 0U;
    std::size_t invalid = 0U;
};

struct PayloadVersionGateResult {
    std::size_t tested = 0U;
    std::size_t accepted_v1 = 0U;
    std::size_t rejected_future = 0U;
    std::size_t preserved_rejections = 0U;
};

PathEncodingSweepResult verify_all_path_encodings(std::vector<std::string> &failures)
{
    PathEncodingSweepResult result;
    const uint8_t header =
        static_cast<uint8_t>((PAYLOAD_TYPE_TXT_MSG << PH_TYPE_SHIFT) | ROUTE_TYPE_FLOOD);
    for (uint16_t value = 0U; value <= 0xFFU; ++value) {
        const uint8_t path_len = static_cast<uint8_t>(value);
        const uint8_t hash_size = static_cast<uint8_t>((path_len >> 6U) + 1U);
        const uint8_t hash_count = static_cast<uint8_t>(path_len & 63U);
        const std::size_t path_bytes =
            static_cast<std::size_t>(hash_size) * hash_count;
        const bool expected_valid = hash_size < 4U && path_bytes <= MAX_PATH_SIZE;
        const bool local_valid = d1l_meshcore_wire_path_len_valid(path_len);
        const bool upstream_valid = mesh::Packet::isValidPathLen(path_len);
        ++result.tested;
        if (expected_valid) {
            ++result.valid;
        } else {
            ++result.invalid;
        }
        if (local_valid != expected_valid || upstream_valid != expected_valid) {
            std::ostringstream detail;
            detail << "path encoding 0x" << std::hex << value
                   << " validity differs: expected=" << expected_valid
                   << " local=" << local_valid << " upstream=" << upstream_valid;
            failures.push_back(detail.str());
            continue;
        }

        if (expected_valid) {
            std::vector<uint8_t> raw(2U + path_bytes + 1U, 0x5CU);
            raw[0] = header;
            raw[1] = path_len;
            d1l_meshcore_wire_packet_t packet{};
            if (!d1l_meshcore_wire_decode(raw.data(), raw.size(), &packet) ||
                packet.path_len != path_len || packet.path_byte_len != path_bytes ||
                packet.path_hash_bytes != hash_size || packet.path_hops != hash_count ||
                packet.payload_len != 1U) {
                std::ostringstream detail;
                detail << "valid path encoding 0x" << std::hex << value
                       << " did not decode with its exact structural length";
                failures.push_back(detail.str());
            }
        } else {
            const std::vector<uint8_t> raw = {header, path_len, 0x5CU};
            std::ostringstream name;
            name << "path_encoding_0x" << std::hex << value;
            expect_decode_rejected(failures, name.str(), raw.data(), raw.size());
        }
    }
    if (result.tested != 256U || result.valid != 119U || result.invalid != 137U) {
        failures.push_back("path encoding sweep did not produce 256/119/137 counts");
    }
    return result;
}

PayloadVersionGateResult verify_payload_version_gate(
    std::vector<std::string> &failures)
{
    PayloadVersionGateResult result;
    for (uint8_t version = 0U; version <= 3U; ++version) {
        const std::array<uint8_t, 3> raw = {
            static_cast<uint8_t>((version << 6U) |
                                 (PAYLOAD_TYPE_TXT_MSG << PH_TYPE_SHIFT) |
                                 ROUTE_TYPE_FLOOD),
            0x00U,
            0x42U,
        };

        d1l_meshcore_wire_packet_t envelope{};
        if (!d1l_meshcore_wire_decode(raw.data(), raw.size(), &envelope) ||
            envelope.version != version) {
            failures.push_back("permissive envelope decoder rejected payload version " +
                               std::to_string(version));
            continue;
        }

        d1l_meshcore_wire_packet_t gated;
        std::memset(&gated, 0xA5, sizeof(gated));
        d1l_meshcore_wire_packet_t before;
        std::memcpy(&before, &gated, sizeof(gated));
        const bool accepted =
            d1l_meshcore_wire_decode_v1(raw.data(), raw.size(), &gated);
        ++result.tested;

        if (version == D1L_MESHCORE_PAYLOAD_VER_1) {
            if (!accepted || gated.version != D1L_MESHCORE_PAYLOAD_VER_1 ||
                gated.header != envelope.header || gated.payload_len != 1U ||
                gated.payload[0] != 0x42U) {
                failures.push_back("v1 payload gate rejected or changed PAYLOAD_VER_1");
            } else {
                ++result.accepted_v1;
            }
        } else {
            if (accepted) {
                failures.push_back("v1 payload gate accepted future version " +
                                   std::to_string(version));
            } else {
                ++result.rejected_future;
                if (std::memcmp(&before, &gated, sizeof(gated)) == 0) {
                    ++result.preserved_rejections;
                } else {
                    failures.push_back(
                        "v1 payload gate changed output while rejecting version " +
                        std::to_string(version));
                }
            }
        }
    }
    return result;
}

std::size_t verify_undersized_capacity_sweep(std::vector<std::string> &failures)
{
    std::array<uint8_t, 64> path{};
    std::array<uint8_t, D1L_MESHCORE_MAX_PACKET_PAYLOAD> payload{};
    d1l_meshcore_wire_packet_t packet{};
    packet.header = static_cast<uint8_t>(
        (PAYLOAD_TYPE_TXT_MSG << PH_TYPE_SHIFT) | ROUTE_TYPE_TRANSPORT_DIRECT);
    packet.transport_codes[0] = 0x1234U;
    packet.transport_codes[1] = 0xABCDU;
    packet.path_len = 0x60U;  // two-byte hashes, 32 hops, 64 path bytes
    packet.path = path.data();
    packet.payload = payload.data();
    packet.payload_len = payload.size();
    constexpr std::size_t kRequired = 6U + 64U + D1L_MESHCORE_MAX_PACKET_PAYLOAD;
    static_assert(kRequired == 254U, "capacity boundary vector changed");

    for (std::size_t capacity = 0U; capacity < kRequired; ++capacity) {
        GuardedBuffer<MAX_TRANS_UNIT> raw;
        std::size_t out_len = 0xBEEFU;
        if (d1l_meshcore_wire_encode(&packet, raw.data(), capacity, &out_len)) {
            std::ostringstream detail;
            detail << "encoder accepted undersized capacity " << capacity;
            failures.push_back(detail.str());
        }
        if (out_len != 0xBEEFU || !raw.guards_intact() ||
            !raw.data_region_unchanged()) {
            std::ostringstream detail;
            detail << "undersized capacity " << capacity
                   << " changed output or crossed a canary";
            failures.push_back(detail.str());
        }
    }
    return kRequired;
}

}  // namespace

int main()
{
    std::vector<std::string> failures;
    std::size_t upstream_to_local = 0U;
    std::size_t local_to_upstream = 0U;
    std::size_t vector_index = 0U;
    for (const uint8_t type : kPayloadTypes) {
        for (const uint8_t route : kRouteTypes) {
            for (const PathCase &path_case : kPathCases) {
                for (const uint16_t payload_len : kPayloadLengths) {
                    verify_upstream_to_local(failures, vector_index, type, route,
                                             path_case, payload_len);
                    ++upstream_to_local;
                    verify_local_to_upstream(failures, vector_index, type, route,
                                             path_case, payload_len);
                    ++local_to_upstream;
                    ++vector_index;
                }
            }
        }
    }
    const std::size_t malformed_decodes = verify_malformed_decodes(failures);
    const std::size_t malformed_encodes = verify_malformed_encodes(failures);
    const PathEncodingSweepResult path_encodings =
        verify_all_path_encodings(failures);
    const PayloadVersionGateResult version_gate =
        verify_payload_version_gate(failures);
    const std::size_t undersized_capacities =
        verify_undersized_capacity_sweep(failures);
    const bool passed = failures.empty() && upstream_to_local == 576U &&
                        local_to_upstream == 576U &&
                        version_gate.tested == 4U &&
                        version_gate.accepted_v1 == 1U &&
                        version_gate.rejected_future == 3U &&
                        version_gate.preserved_rejections == 3U;

    for (const std::string &failure : failures) {
        std::cerr << failure << '\n';
    }
    std::cout << "{\"passed\":" << (passed ? "true" : "false")
              << ",\"coverage_boundary\":\"wire_envelope_only\""
              << ",\"issue_65_closure_eligible\":false"
              << ",\"vectors\":{\"upstream_to_local\":" << upstream_to_local
              << ",\"local_to_upstream\":" << local_to_upstream
              << ",\"total\":" << (upstream_to_local + local_to_upstream) << "}"
              << ",\"malformed\":{\"decode\":" << malformed_decodes
              << ",\"encode\":" << malformed_encodes
              << ",\"total\":" << (malformed_decodes + malformed_encodes) << "}"
              << ",\"path_length_encodings\":{\"tested\":" << path_encodings.tested
              << ",\"valid\":" << path_encodings.valid
              << ",\"invalid\":" << path_encodings.invalid << "}"
              << ",\"payload_version_gate\":{\"tested\":" << version_gate.tested
              << ",\"accepted_v1\":" << version_gate.accepted_v1
              << ",\"rejected_future\":" << version_gate.rejected_future
              << ",\"preserved_rejections\":"
              << version_gate.preserved_rejections << "}"
              << ",\"undersized_encoder_capacities\":" << undersized_capacities
              << ",\"failures\":" << failures.size() << "}\n";
    return passed ? 0 : 1;
}
