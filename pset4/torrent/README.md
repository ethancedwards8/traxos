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


