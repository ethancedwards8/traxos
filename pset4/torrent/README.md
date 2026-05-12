# Tracker information

Link: https://github.com/pobrn/mktorrent/wiki
Link: https://wiki.theory.org/BitTorrentSpecification#Metainfo_File_Structure
Tracker reference implementation: https://github.com/troydm/udpt
Tracker reference implementation: https://erdgeist.org/gitweb/opentracker/
Client reference implementation: https://github.com/veggiedefender/torrent-client/
Client reference implementation: https://github.com/sujanan/tnt/

mktorrent -v -a 127.0.0.1:6000 ./cs61hello.jpg 
mktorrent 1.1 (c) 2007, 2009 Emil Renner Berthing

Options:
  Announce URLs:
    1 : 127.0.0.1:6000
  Torrent name: cs61hello.jpg
  Metafile:     /Users/ece/git/cs2620/cs2620-s26-psets-ethancedwards8/pset4/torrent/cs61hello.jpg.torrent
  Piece length: 262144
  Threads:      10
  Be verbose:   yes
  Write date:   yes
  Web Seed URL: none
  Comment:      none


69083 bytes in all.
That's 1 pieces of 262144 bytes each.

Hashed 1 of 1 pieces.
Writing metainfo file... done.

Also read these instructive blog posts:
Warning, likely AI written: https://medium.com/@abhijitkad62/beyond-the-progress-bar-how-i-visualized-bittorrents-hidden-dance-in-real-time-914038c77423
Pretty good: https://blog.jse.li/posts/torrent/

I tried to store ip as a char[24] earlier (like a string), but then decided
that was a terrible idea because then what happens when i get a short ip like 123.1.4.2.
what do i do with the extra chars? so i changed to a uint32 to just store the bits like
a proper networking fan

anyways

after reading through multiple implementations of trackers (see above) and clients (above)
i came to the conclusion that trackers are just state machines. wow. who would've thought it!

anyways i don't think it maps properly onto pancydb, so rewiring my paxos to eschew pancydb
is probably necessary. but i think lots can be kept?

i downloaded a bencode library because i thought it would need it, but it seems they're all mostly
just decoders. i need an encoder. not really sure what to do here than just be kinda janky and
use std::format or something to create strings for what i need.

speaking of, this site: https://chocobo1.github.io/bencode_online/ came in handy
I got this from a debian tracker: d8:intervali900e5:peersld2:ip12:185.23.80.924:porti8999eed2:ip14:129.101.57.1324:porti50413eed2:ip11:79.155.57.94:porti50114eed2:ip13:94.31.119.2194:porti6881eed2:ip12:108.48.1.2054:porti51413eed2:ip13:87.58.176.2384:porti62010eed2:ip14:163.252.249.874:porti51413eed2:ip12:31.18.147.944:porti51413eed2:ip13:172.100.98.294:porti17717eed2:ip14:93.103.218.1504:porti6984eed2:ip13:76.193.65.2474:porti51413eed2:ip13:216.25.249.884:porti6881eed2:ip13:87.58.176.2384:porti62014eed2:ip14:157.97.134.1304:porti7518eed2:ip14:100.15.132.1914:porti51765eed2:ip13:188.25.58.1064:porti51520eed2:ip15:107.200.112.2214:porti31000eed2:ip14:71.174.126.1254:porti53140eed2:ip10:176.0.76.84:porti62286eed2:ip12:66.56.81.1514:porti6881eed2:ip13:82.64.162.2124:porti34073eed2:ip14:178.66.129.1564:porti26926eed2:ip12:79.165.32.934:porti34724eed2:ip13:71.161.110.914:porti60000eed2:ip12:78.122.11.374:porti6964eed2:ip13:212.201.8.1164:porti51711eed2:ip13:87.58.176.2384:porti62008eed2:ip11:75.76.13.424:porti54857eed2:ip15:176.226.159.1764:porti56897eed2:ip14:217.231.73.2164:porti57688eed2:ip10:81.2.80.534:porti51413eed2:ip11:5.18.88.1724:porti40932eed2:ip14:109.242.232.514:porti56845eed2:ip11:80.78.21.364:porti20271eed2:ip12:50.106.27.544:porti32294eed2:ip14:108.53.120.1574:porti40792eed2:ip12:89.149.52.684:porti24156eed2:ip14:103.119.63.1864:porti6881eed2:ip12:89.98.162.174:porti6996eed2:ip12:136.50.60.594:porti51413eed2:ip12:92.63.67.1504:porti51413eed2:ip11:82.64.2.1904:porti6881eed2:ip14:68.253.128.1754:porti51413eed2:ip15:188.212.112.1634:porti32767eed2:ip14:122.189.78.1964:porti25017eed2:ip12:66.70.179.334:porti51413eed2:ip13:87.58.176.2384:porti62015eed2:ip13:83.148.245.514:porti51414eed2:ip13:200.217.35.274:porti51413eed2:ip13:87.58.176.2384:porti62017eeee
curl "http://bttracker.debian.org:6969/announce?info_hash=2%5DI%01%A2j%88C-%3E%20%C0l%5D%3B%7C%05%0B%A9%B3&peer_id=ABCDEFGHIJKLMNOPQRST&ip=216.234.197.127&port=6881&downloaded=1234&left=98765&event=started"

i spent a frustrating amount of time trying to deal with the info_hash (which is the torrent hash) before
i realized i wasn't properly doing the url encoding. ew.

anyways I'm using curl to test my implementation thus far.
curl "http://127.0.0.1:9000/announce?info_hash=2%5DI%01%A2j%88C-%3E%20%C0l%5D%3B%7C%05%0B%A9%B3&peer_id=ABCDEFGHIJKLMNOPQRST&ip=216.234.197.127&port=6881&downloaded=1234&left=98765&event=started"

to test the full support, i ran multiple clients of tnt across my homelab network

then i realized that some clients (tnt namely) don't actually supply the peer_id in the announce but instead
assume that the tracker server takes it implicitly from the tcp connection. i guess this makes sense from
a security perspective/dos because its probably much harder to make tcp lie than just passing a fake ip

## BUGGG!!!!!!!! bane of my existence

so

while testing that the tracker was working and that peers could auto-discover each other, here's a list of
the issues i encountered and how i debugged them:


1. added a /debug endpoint to my tracker server to see extra info:
  curl http://10.0.20.236:9000/debug
  torrents: 1
  info_hash 45d84be32728d481a0895c1056554f0dc41f63b3 peers: 2
    peer_id 2d5452343131302d37353969347a6b3239387875 ip 10.0.20.227 port 51413 left 69083 complete no
    peer_id 2d5452343131302d7170797470366f39716b7876 ip 10.0.10.175 port 51413 left 69083 complete no

2. double checked that the torrent file was valid, multiple times
    transmission-show pset4/torrent/cs61hello.jpg.torrent

3. Checked whether Transmission was really seeding:
    i learned that transmission can just hid files on your filesystem and seed them unless you
    restore old resume state unless run with a fresh -g config dir. [1.00] means complete; [0.00] means incomplete,
    even though the cli confusingly still prints “Seeding”.
5. Verified the exact tracker response sent to tnt:
    temporarily logged the announce response body. For tnt, the tracker sent:

d8:intervali900e5:peersld2:ip11:10.0.20.2274:porti51413eeee

That is valid bencode and includes the Transmission seeder. but tnt DIDN'T LIKE IT. i don't know why

6. Compared with Debian:
    Debian also returns peers as a list of dictionaries, not compact bytes, so our response shape was not obviously wrong.
7. Tried Connection: close:
    Debian sends Connection: Close header. so tried adding it. tnt then failed with Socket is closed, sooo idk whats going on here.
    
8. Checked network reachability:
    nc showed the seeder port could be reachable from the relevant machines, so basic TCP routing/firewall was not the main issue.

9. Used tcpdump:
    On the Transmission seeder, i watched for TCP from tnt to 51413. Nothing appeared. only saw UDP from Transmission to tnt:
    tcpdump: data link type PKTAP
tcpdump: verbose output suppressed, use -v[v]... for full protocol decode
listening on any, link-type PKTAP (Apple DLT_PKTAP), snapshot length 524288 bytes
04:48:56.154903 IP 10.0.20.227.51413 > 10.0.10.175.6881: UDP, length 20
04:48:59.606183 IP 10.0.20.227.51413 > 10.0.10.175.6881: UDP, length 20
04:49:05.854798 IP 10.0.20.227.51413 > 10.0.10.175.6881: UDP, length 20


Conclusion: the tracker gave tnt a valid seeder, but tnt never attempted the TCP peer connection. that means tnt was BAD! and i don't care
enough to fix it. transmission works. i trust its right.
