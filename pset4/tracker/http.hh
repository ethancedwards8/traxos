#pragma once

#include "cotamer/cotamer.hh"
#include "cotamer/http.hh"
#include "tracker/types.hh"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

std::optional<uint32_t> parse_ipv4(std::string_view value);
std::string format_ipv4(uint32_t ip);
std::optional<uint32_t> peer_ipv4(cotamer::fd& cfd);

void parse_announce_request(cotamer::http_message& req,
                            bt_tracker_announce_request& bt_req,
                            std::string& failure);

std::string tracker_success_response(const bt_tracker_state& state,
                                     const bt_tracker_announce_request& bt_req);
std::string tracker_debug_response(const bt_tracker_state& state);
