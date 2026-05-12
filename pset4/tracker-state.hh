#pragma once

#include "bittorrent.hh"
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

std::optional<uint32_t> parse_ipv4(std::string_view value);
std::string format_ipv4(uint32_t ip);

bt_info_hash make_info_hash(const char (&bytes)[20]);
bt_peer_id make_peer_id(const char (&bytes)[20]);

void apply_announce(bt_tracker_state& state, const bt_tracker_announce_request& req);
std::string tracker_success_response(const bt_tracker_state& state,
                                     const bt_tracker_announce_request& req);
std::string tracker_debug_response(const bt_tracker_state& state);

