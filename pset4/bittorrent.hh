#pragma once
#include <array>
#include <cstdint>
#include <map>

enum bt_event {
    started,
    stopped,
    completed
};

// https://stackoverflow.com/questions/60136080/using-character-array-as-a-key-in-map?rq=3
// std::array over char[N] because of std::map struggles
using bt_info_hash = std::array<char, 20>;
using bt_peer_id = std::array<char, 20>;

// https://wiki.theory.org/BitTorrent_Tracker_Protocol#Basic_Tracker_Announce_Request
struct bt_tracker_announce_request {
    char info_hash[20];

    char peer_id[20];

    // only support ipv4 because v6 is lame
    uint32_t ip = 0;

    uint16_t port;

    // Transfer statistics
    uint64_t uploaded;
    uint64_t downloaded;
    uint64_t left;

    bt_event event;

    int32_t numwant = 50;
};

struct bt_peer {
    bt_peer_id peer_id;
    uint32_t ip = 0;
    uint16_t port = 0;
    uint64_t uploaded = 0;
    uint64_t downloaded = 0;
    uint64_t left = 0;
    bool complete = false;
};

struct bt_tracker_state {
    std::map<bt_info_hash, std::map<bt_peer_id, bt_peer>> torrents;
};
