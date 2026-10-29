# m17-ax25

AX.25 over M17 packet mode — a KISS bridge and packet framing library for the
[G4GUO M17 SDR](https://github.com/G4GUO/m17_webapp) and any other M17 terminal.

M17 reserves application-layer packet data type `0x01` for AX.25 (spec Table
3.15), so an AX.25 frame travels as the payload of an M17 packet-mode burst. The
M17 terminal therefore does what a KISS-mode TNC used to do: accept opaque frame
bytes from the host, get them on the air, and hand received bytes back. It knows
nothing about AX.25.

This repository holds the protocol code only — no audio, no SDR, no UI, no
threads — so it can be built and tested off-air.

## Scope

In scope:

* KISS framing: `0xC0` FEND delimiters, `0xDB` escaping, port/command byte.
* M17 packet-mode LSF construction (P/S bit clear, CAN, broadcast or unicast DST).
* Packet payload assembly: type byte, CRC-16, 25-byte chunking with EOF/byte-count
  metadata, 33-frame ceiling.
* The reverse: reassembly, CRC check, type byte strip, KISS framing out.
* Golden-vector tests for all of the above.

Out of scope (stays in the radio application):

* Channel access, DCD, PTT, queueing, the voice/packet arbitration.
* The socket, the threads, the modulator.
* AX.25 itself — addresses, ARQ, connected mode belong to the host (BPQ, kernel
  stack, Xrouter).

## Design decisions

These were settled before any code was written; see `docs/design.md` for the
reasoning and the spec citations.

| Decision | Choice |
|---|---|
| Payload type identifier | `0x00` RAW, per M17-KISS basic mode (accept `0x01`/`0x02` on RX) |
| LSF destination | BROADCAST `FF:FF:FF:FF:FF:FF` for unproto traffic |
| LSF source | the callsign of the station keying the burst, not the AX.25 originator |
| FCS | none — KISS carries the frame without it, M17's CRC-16 covers the burst |
| Fragmentation | 25-byte chunks, EOF bit + valid-byte count in the last frame |
| Max payload | 823 bytes total; frames over 822 are dropped |
| Host API | KISS over TCP (radio listens, host connects), M17-KISS **basic** mode only |
| Outstanding frames | one in flight, immutable once queued |
| Duplicate suppression | byte-equality only; no parsing of frame contents |

## Layout

    src/kiss.h          KISS framing API (decoder state machine, encoder)
    src/kiss.c          implementation
    src/m17_packet.h    LSF, payload chunking and reassembly API
    src/m17_packet.c    CRC-16, base-40 addresses, 25-byte framing
    src/m17_bridge.h    queue and channel-access state machine API
    src/m17_bridge.c    duplicate suppression, backoff, voice priority, guard
    tests/test_kiss.c   golden vectors, escaping, overrun, max-size round trip
    tests/test_m17_packet.c  CRC and address vectors, LSF layout, chunking,
                             round trip, reassembly error handling
    tests/test_m17_bridge.c  queueing, backoff, voice priority, time wrap
    docs/design.md      protocol notes and the reasoning behind the decisions

## Building and testing

    make          # build and run the tests
    make san      # same tests under ASan/UBSan

No dependencies beyond a C11 compiler.

## Status

KISS framing, M17 packet framing and the bridge state machine are implemented
and tested (256 checks, clean under ASan/UBSan). The bridge takes time, DCD, PTT
and a random word as arguments, so it needs no hardware.

What is deliberately absent is the socket and the modulator. Integration is:

    kiss_decoder_push(&dec, bytes, n, on_frame, &bridge)   /* socket thread */
    m17_bridge_tick(&bridge, now_ms, dcd, ptt, rand32)     /* main loop      */
      -> m17_bridge_head() -> m17_pkt_build() -> radio modulator
      -> m17_bridge_sent() when the EoT has been sent
    m17_rx_chunk() -> kiss_encode() -> socket write        /* receive path   */

## Licence

0BSD — see `LICENSE`.
