#!/usr/bin/env bash

set -euo pipefail
set -x

RND="${1:-20}"

build/traxos-paxos-test -R "${RND}"
build/traxos-paxos-test --loss 0.05 -R "${RND}"
build/traxos-paxos-test --failure-mode failed_leader -R "${RND}"
build/traxos-paxos-test --failure-mode failed_replica --failed-replica 1 -R "${RND}"
build/traxos-paxos-test --failure-mode failed_replica --failed-replica 2 -R "${RND}"
build/traxos-paxos-test --failure-mode delayed_leader_failure -R "${RND}"
build/traxos-paxos-test --failure-mode disruptive_isolate -R "${RND}"
build/traxos-paxos-test --failure-mode multiple_random_up_down --failed-replica 1 -R "${RND}"
