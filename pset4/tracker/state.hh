#pragma once

#include "tracker/types.hh"

bt_info_hash make_info_hash(const char (&bytes)[20]);
bt_peer_id make_peer_id(const char (&bytes)[20]);

void apply_announce(bt_tracker_state& state, const bt_tracker_announce_request& bt_req);
