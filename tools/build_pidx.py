#!/usr/bin/env python3
"""
Prototype: build a per-I-frame (time, byte_offset) index for an MPEG-1
Program Stream, mirroring hypseus/VLDP's .dat frame-offset approach so
pl_mpeg can do O(1) seeks instead of plm_demux_seek()'s bitrate-estimate
retry search.

byte_offset recorded is the position immediately AFTER the 4-byte PES
start code (00 00 01 E0), matching what pl_mpeg's plm_demux_buffer_seek()
+ plm_demux_decode_packet() expect (see plm_demux_seek()'s
`packet_start = plm_buffer_tell(...)` right after
`plm_buffer_find_start_code()` consumes the start code).

Bit-level parsing here mirrors plm_demux_decode_packet() in pl_mpeg.h
exactly (sequential bit reads, not independent byte-aligned peeks).

Output format (little-endian):
  uint32   count
  count *  { double time_seconds; uint32 byte_offset }
  optional trailer (readers that predate it ignore these bytes):
    uint32   magic 'PCNT' (bytes 50 43 4E 54)
    uint32   picture_count   exact number of pictures in the video stream,
                             counted from every picture_start_code (00 00 01 00)
                             in the concatenated video payload. Not derivable
                             from timestamps: only the first picture of a PES
                             packet carries a PTS.
"""
import struct
import sys


class BitReader:
    def __init__(self, data, byte_pos):
        self.data = data
        self.bit_index = byte_pos * 8

    def read(self, nbits):
        val = 0
        for _ in range(nbits):
            byte_i = self.bit_index >> 3
            bit_i = 7 - (self.bit_index & 7)
            bit = (self.data[byte_i] >> bit_i) & 1
            val = (val << 1) | bit
            self.bit_index += 1
        return val

    def byte_pos(self):
        return self.bit_index >> 3


def decode_pts_bits(br):
    # The leading 4-bit prefix ('0010'/'0011') was already consumed by the
    # caller's P-STD-check read(2) + pts_dts_marker read(2) -- do NOT
    # re-read it here, only the remaining 36 bits of the PTS value.
    a = br.read(3)
    br.read(1)
    b = br.read(15)
    br.read(1)
    c = br.read(15)
    br.read(1)
    pts = (a << 30) | (b << 15) | c
    return pts / 90000.0


def build_index(path):
    with open(path, "rb") as f:
        data = f.read()

    n = len(data)
    i = 0
    index = []
    last_pts = 0.0
    video_packets = 0
    iframes = 0
    es_chunks = []

    while i < n - 6:
        if data[i] == 0 and data[i + 1] == 0 and data[i + 2] == 1 and data[i + 3] == 0xE0:
            packet_start = i + 4  # matches plm_buffer_tell() right after start code
            if packet_start + 2 > n:
                break
            length = (data[packet_start] << 8) | data[packet_start + 1]
            payload_start = packet_start + 2
            packet_end = payload_start + length
            if packet_end > n:
                packet_end = n

            p = payload_start
            while p < n and data[p] == 0xFF:
                p += 1

            br = BitReader(data, p)

            if br.read(2) == 0x01:  # P-STD buffer scale/size prefix
                br.read(16)

            pts = None
            marker = br.read(2)
            if marker == 0x03:
                pts = decode_pts_bits(br)  # consumes remaining PTS bits
                br.read(40)  # DTS (we don't need it)
            elif marker == 0x02:
                pts = decode_pts_bits(br)
            elif marker == 0x00:
                br.read(4)  # remaining marker bits

            if pts is not None:
                last_pts = pts

            video_packets += 1

            es_start = br.byte_pos()
            es_chunks.append(data[es_start:packet_end])
            j = es_start
            while j + 6 <= packet_end:
                if data[j] == 0 and data[j + 1] == 0 and data[j + 2] == 1 and data[j + 3] == 0:
                    coding_type = (data[j + 5] >> 3) & 0x07
                    if coding_type == 1:  # I-frame
                        index.append((last_pts, packet_start))
                        iframes += 1
                    break
                j += 1

            i = packet_end if packet_end > i else i + 4
        else:
            i += 1

    # A start code can straddle two PES packets, so count on the joined payload.
    picture_count = b"".join(es_chunks).count(b"\x00\x00\x01\x00")

    print(f"scanned {video_packets} video PES packets, found {iframes} I-frames, "
          f"{picture_count} pictures", file=sys.stderr)
    return index, picture_count


def write_index(index, picture_count, out_path):
    with open(out_path, "wb") as f:
        f.write(struct.pack("<I", len(index)))
        for t, off in index:
            f.write(struct.pack("<dI", t, off))
        f.write(b"PCNT")
        f.write(struct.pack("<I", picture_count))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <in.mpg> <out.pidx>", file=sys.stderr)
        sys.exit(1)

    idx, pictures = build_index(sys.argv[1])
    write_index(idx, pictures, sys.argv[2])
    print(f"wrote {len(idx)} I-frame entries and picture_count={pictures} to {sys.argv[2]}", file=sys.stderr)
    for t, off in idx[:15]:
        print(f"  t={t:.3f}s off={off}")
