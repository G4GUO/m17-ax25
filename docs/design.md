# Design notes: AX.25 over M17

## 1. Why it fits

M17's packet mode is a frame-based replacement for HDLC: fixed 40 ms frames, a
sync word on every frame, mandatory FEC, and a CRC over the payload. What it does
not provide is any link control — no acknowledgement, no retransmission, no
window, no connection state, and no channel-access algorithm in the protocol
itself. AX.25 supplies all of that, and the M17 application layer reserves a data
type identifier for it:

| Identifier | Protocol |
|---|---|
| 0x00 | RAW |
| **0x01** | **AX.25** |
| 0x02 | APRS |
| 0x03 | 6LoWPAN |
| 0x04 | IPv4 |
| 0x05 | SMS |
| 0x06 | Winlink |

(M17 spec 3.0 §3.3.2, Table 3.15; identical table as Table 19 in the 2.0.10
Application Layer chapter.)

So the M17 terminal takes the role of a KISS-mode TNC: opaque frames in, opaque
frames out, radio duties in between. The host keeps AX.25.

## 2. Packet mode geometry

A packet-mode transmission is:

    preamble (40 ms) | LSF (40 ms) | 1..33 packet frames (40 ms each) | EoT (40 ms)

* Application data: 1..823 bytes, plus an appended 2-byte CRC = 25..825 bytes.
* Data is split into 25-byte chunks. Each frame carries a 200-bit chunk plus a
  6-bit metadata field: EOF flag in bit 7, and either the frame counter (EOF=0)
  or the count of valid bytes in the frame (EOF=1) in bits 6..2.
* Short final chunks are null-padded, after the CRC.
* 206 type-1 bits → rate-1/2 K=5 convolutional → P3 puncture (368 bits) →
  interleave → randomise → 16-bit packet sync (0x75FF) → 384-bit frame.
* Base throughput 5 kbps; spec quotes >4.5 kbps net for large payloads.

Airtime for real AX.25 frames, counting preamble + LSF + frames + EoT:

| AX.25 frame | with type byte | frames | airtime | AX.25-layer rate |
|---|---|---|---|---|
| 823 B | 824 | 34 | 1480 ms | — exceeds the 33-frame ceiling |
| 512 B | 513 | 21 | 960 ms | 4.3 kbps |
| 256 B | 257 | 11 | 560 ms | 3.7 kbps |
| 60 B (APRS) | 61 | 3 | 240 ms | 2.0 kbps |

Consequence: keep `paclen` at 256 or below. It bounds a single burst to about
half a second, which matters on a shared simplex channel.

## 3. LSF

Every transmission opens with a Link Setup Frame: 40 ms, sync 0x55F7, carrying
30 bytes of Link Setup Data.

| Field | Bytes |
|---|---|
| DST | 6 |
| SRC | 6 |
| TYPE | 2 |
| META | 14 |
| CRC | 2 |

TYPE bitfields: byte 0 bits 3..0 = Channel Access Number, bit 4 = signed-stream
flag, bits 7..5 reserved. Byte 1 bit 0 = Packet/Stream, bits 2..1 = data type,
bits 4..3 = encryption type, bits 6..5 = encryption subtype.

For packet mode only the P/S bit and the CAN are defined; all other bits are
reserved (§3.3.1). META is optional and carried once, in the LSF: put a GNSS
position there if the station has one, otherwise zeroes.

The 30 bytes are exactly what M17-KISS full packet mode prefixes to a frame, and
what the radio application already keeps as its LSF buffer.

## 4. Addressing

* **DST = BROADCAST** (`FF FF FF FF FF FF`, spec A.3: valid only as a
  destination). On a simplex channel everyone hears everything anyway, so the
  broadcast costs nothing and avoids inventing a mapping from AX.25 addresses
  (which may be aliases, group calls or digipeater-less destinations) to M17
  addresses.
* **SRC = the callsign of the station keying the burst.** The spec agrees: "The
  source address is always the callsign of the station transmitting, be it a
  client, repeater, or gateway." On a relayed path the M17 SRC becomes the last
  transmitter, which is why the AX.25 header inside the payload stays
  authoritative for who originated the packet. Never derive or rewrite AX.25
  addresses from the LSF; use it only for filtering, priority and display.
* **SSIDs** live in the AX.25 header. If an SSID is also encoded in the M17
  address as a suffix (`G4GUO-9`), note that 9 characters is the limit and long
  calls truncate.
* **Type identifier**: transmit `0x00` RAW, as M17-KISS §5.1 prescribes for basic
  mode. The payload really is AX.25, so the semantic identifier would be `0x01`;
  the station therefore accepts `0x00`, `0x01` and `0x02` on receive and treats
  them alike.

## 5. Host interface

**M17-KISS basic packet mode only** (port 0, command 0). The host sends frame
bytes and receives frame bytes; the radio owns the LSF entirely. That LSF is
constant — broadcast DST, configured callsign, type RAW, CAN 0, zeroed META — so
it is built once at startup and reused for every burst, which the spec permits
for back-to-back packets. Full packet mode (port 1, host-supplied LSF) is
deliberately not implemented: it would only serve a bespoke M17-aware host, and
BPQ cannot use it.

KISS over TCP. The radio **listens**; the host connects (this mirrors Direwolf's
KISSPORT on 8001 and UZ7HO's 8100).

Byte stream: `0xC0` FEND delimiters, `0xDB` escapes (`0xDB 0xDC` = 0xC0,
`0xDB 0xDD` = 0xDB), first byte after FEND = `(port << 4) | command`. Port 0
command 0 is data. Parsing must be a byte-wise state machine that survives
partial and coalesced reads.

LinBPQ port configuration:

    PORT
     PORTNUM=1
     ID=M17 70cm
     TYPE=ASYNC
     PROTOCOL=KISS
     IPADDR=127.0.0.1
     TCPPORT=8001
     FRACK=5000
     RESPTIME=3000
     RETRIES=10
     MAXFRAME=1
     PACLEN=256
     KISSOPTIONS=NOPARAMS

`NOPARAMS` stops BPQ sending TXDELAY/SLOTTIME/PERSIST at port start; M17's
preamble is a fixed 40 ms and cannot honour a TXDELAY. SLOTTIME/PERSIST may be
honoured if sent anyway.

## 6. Radio-side contract

* **Channel access is the radio's job.** The kernel AX.25 stack has no backoff to
  delegate to — `af_ax25.c` hands UI frames straight to `ax25_queue_xmit()` and
  `mkiss` is only a SLIP framer; the PERSIST/SLOTTIME/TXDELAY knobs in
  `kissparms` are commands addressed to the TNC. So: wait for clear, wait a
  random 0..40 ms (one M17 frame), transmit.
* **DCD** counts any valid M17 sync (LSF, stream, packet), not just the packet
  sync, plus an energy gate; hold it for one frame period after the last sync.
* **One frame outstanding**, immutable once queued. A KISS frame is one burst.
* **Duplicate suppression by byte equality only** — compare the incoming frame
  with what is already queued and drop the copy. No parsing of AX.25 contents,
  which keeps the terminal content-agnostic.
* **Queue of 3..4**, FIFO, drop-newest on overflow with a log line. Never
  drop-oldest: it reorders and starves.
* **Voice has priority to start, data to finish.** Packet mode has no abort, so a
  burst in flight must complete; ignore mic PTT while keyed and indicate busy.
* **Ignore DCD for ~20 ms after our own EoT**, or the key-down tail re-arms the
  busy flag and the queue stalls.
* Data transmission should be an operator-enabled state, not always-on, and
  ideally gated by a quiet period (3..5 s since the last voice activity) so bursts
  do not land in the gaps of someone else's QSO.

## 7. Timers

BPQ starts T1 (`FRACK`) when it writes to KISS, not when the burst is radiated.
An 823-byte frame is 1.3 s of air *after* any channel wait, so:

* `PACLEN` 256, `MAXFRAME` 1, `FRACK` 5000 ms, `RETRIES` 8..10.
* Do not restart the channel backoff when a frame is replaced or re-queued; that
  is how a frame gets postponed forever behind a persistently busy channel.

## 8. Open questions

* Full packet mode (port 1) is not planned; revisit only if a host appears that
  can supply its own LSF and wants directed addressing.
* Whether to expose DCD/TX-complete on a side channel for hosts that can use it.
  KISS has no standard mechanism, and the kernel stack has nowhere to put it.
