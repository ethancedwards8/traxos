#include "tracker-state.hh"
#include <charconv>
#include <cstring>
#include <format>

template <typename T>
static std::optional<T> parse_number(std::string_view value) {
    T result{};
    auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (ec != std::errc() || ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return result;
}

bt_info_hash make_info_hash(const char (&bytes)[20]) {
    bt_info_hash key;
    memcpy(key.data(), bytes, key.size());
    return key;
}

bt_peer_id make_peer_id(const char (&bytes)[20]) {
    bt_peer_id key;
    memcpy(key.data(), bytes, key.size());
    return key;
}

template <size_t N>
static std::string hex_bytes(const std::array<char, N>& bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(N * 2);
    for (char byte : bytes) {
        auto value = static_cast<unsigned char>(byte);
        out.push_back(digits[value >> 4]);
        out.push_back(digits[value & 0xf]);
    }
    return out;
}

std::optional<uint32_t> parse_ipv4(std::string_view value) {
    std::string dotted(value);
    dotted += ".";

    uint32_t ip = 0;
    size_t start = 0;

    for (int i = 0; i != 4; ++i) {
        size_t dot = dotted.find('.', start);

        auto part = parse_number<uint32_t>(std::string_view(dotted).substr(start, dot - start));
        if (!part || *part > 255) {
            return std::nullopt;
        }

        ip = (ip << 8) | *part;
        start = dot + 1;
    }

    if (start != dotted.size()) {
        return std::nullopt;
    }

    return ip;
}

std::string format_ipv4(uint32_t ip) {
    return std::format("{}.{}.{}.{}",
                       (ip >> 24) & 0xff,
                       (ip >> 16) & 0xff,
                       (ip >> 8) & 0xff,
                       ip & 0xff);
}

void apply_announce(bt_tracker_state& state, const bt_tracker_announce_request& req) {
    bt_info_hash info_hash = make_info_hash(req.info_hash);
    bt_peer_id peer_id = make_peer_id(req.peer_id);
    auto& peers = state.torrents[info_hash];

    if (req.event == stopped) {
        peers.erase(peer_id);
        return;
    }

    bt_peer peer;
    peer.peer_id = peer_id;
    peer.ip = req.ip;
    peer.port = req.port;
    peer.uploaded = req.uploaded;
    peer.downloaded = req.downloaded;
    peer.left = req.left;
    peer.complete = req.event == completed || req.left == 0;

    peers[peer_id] = peer;
}

std::string tracker_success_response(const bt_tracker_state& state,
                                     const bt_tracker_announce_request& req) {
    bt_info_hash info_hash = make_info_hash(req.info_hash);
    auto torrent = state.torrents.find(info_hash);

    std::string body = "d8:intervali900e5:peersl";
    if (torrent != state.torrents.end()) {
        int32_t count = 0;
        for (auto& [peer_id, peer] : torrent->second) {
            if (peer_id == make_peer_id(req.peer_id)) {
                continue;
            }
            if (count == req.numwant) {
                break;
            }

            std::string ip = format_ipv4(peer.ip);
            body += std::format("d2:ip{}:{}4:porti{}ee", ip.size(), ip, peer.port);
            ++count;
        }
    }
    body += "ee";
    return body;
}

std::string tracker_debug_response(const bt_tracker_state& state) {
    std::string body;
    body += std::format("torrents: {}\n", state.torrents.size());
    for (auto& [info_hash, peers] : state.torrents) {
        body += std::format("info_hash {} peers: {}\n", hex_bytes(info_hash), peers.size());
        for (auto& [peer_id, peer] : peers) {
            body += std::format("  peer_id {} ip {} port {} left {} complete {}\n",
                                hex_bytes(peer_id),
                                format_ipv4(peer.ip),
                                peer.port,
                                peer.left,
                                peer.complete ? "yes" : "no");
        }
    }
    return body;
}

