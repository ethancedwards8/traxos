#pragma once

enum bt_event {
    started,
    stopped,
    completed
};

// https://wiki.theory.org/BitTorrent_Tracker_Protocol#Basic_Tracker_Announce_Request
struct bt_tracker_announce_request {
    uint8_t info_hash[20];

    char peer_id[20];

    // only support ipv4 because v6 is lame
    char ip[16] = { };

    uint16_t port;

    // Transfer statistics
    uint64_t uploaded;
    uint64_t downloaded;
    uint64_t left;

    bt_event event;

    int32_t numwant = 50;
};
