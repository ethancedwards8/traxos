#include "tracker/state.hh"

#include <cstring>

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

void apply_announce(bt_tracker_state& state, const bt_tracker_announce_request& bt_req) {
    bt_info_hash info_hash = make_info_hash(bt_req.info_hash);
    bt_peer_id peer_id = make_peer_id(bt_req.peer_id);
    auto& peers = state.torrents[info_hash];

    if (bt_req.event == stopped) {
        peers.erase(peer_id);
        return;
    }

    bt_peer peer;

    peer.peer_id = peer_id;
    peer.ip = bt_req.ip;
    peer.port = bt_req.port;
    peer.uploaded = bt_req.uploaded;
    peer.downloaded = bt_req.downloaded;
    peer.left = bt_req.left;
    peer.complete = bt_req.event == completed || bt_req.left == 0;

    peers[peer_id] = peer;
}
