#!/usr/bin/env python3
#
# Lir - The Message Broker
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# Writes the fuzz targets' seeds, lir/fuzz/corpus/<format>/<name>: valid
# inputs a mutator starts from to reach past the first check. Every packet
# here is built by hand from the specification, not by Lir's encoder. Run it
# after changing a seed:
#
#   python3 -I lir/fuzz/seeds.py

import pathlib
import struct

corpus = pathlib.Path(__file__).resolve().parent / "corpus"


def varint(value):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        out.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(out)


def u16(value):
    return struct.pack(">H", value)


def u32(value):
    return struct.pack(">I", value)


def text(value):
    data = value.encode()
    return u16(len(data)) + data


def binary(value):
    return u16(len(value)) + value


def props(*entries):
    block = b"".join(entries)
    return varint(len(block)) + block


def packet(first, body):
    return bytes([first]) + varint(len(body)) + body


user = b"\x26" + text("a") + text("b")

connect_properties = props(b"\x11" + u32(60), b"\x21" + u16(10), user)
will_properties = props(b"\x18" + u32(5), b"\x03" + text("t"))
connack_properties = props(b"\x24\x01", b"\x25\x00", b"\x22" + u16(5))
publish_properties = props(b"\x01\x01", b"\x02" + u32(10), b"\x23" + u16(1), b"\x08" + text("r"), b"\x09" + binary(b"x"), b"\x0b" + varint(7))
subscribe_properties = props(b"\x0b" + varint(1), user)
auth_properties = props(b"\x15" + text("m"), b"\x16" + binary(b"d"))

packets = {
    # Username, password, will retained at QoS 1, clean start.
    "connect-5": packet(0x10, text("MQTT") + b"\x05\xee" + u16(60) + connect_properties + text("c1") + will_properties + text("w/t") + binary(b"bye") + text("u") + binary(b"p")),
    "connect-311": packet(0x10, text("MQTT") + b"\x04\x02" + u16(30) + text("c")),
    "connect-311-will": packet(0x10, text("MQTT") + b"\x04\x2e" + u16(30) + text("c") + text("w") + binary(b"x")),
    "connect-31": packet(0x10, text("MQIsdp") + b"\x03\x02" + u16(30) + text("c")),
    "connack-5": packet(0x20, b"\x00\x00" + connack_properties),
    "connack-311": packet(0x20, b"\x01\x00"),
    "publish-5-qos1": packet(0x32, text("a/b") + u16(1) + publish_properties + b"hi"),
    "publish-311-qos0-retain": packet(0x31, text("a") + b"x"),
    "publish-311-qos2": packet(0x34, text("a/b") + u16(2) + b"y"),
    "puback-5": packet(0x40, u16(1) + b"\x10" + props(b"\x1f" + text("s"))),
    "pubrec-5-short": packet(0x50, u16(1)),
    "pubrel-311": packet(0x62, u16(1)),
    "pubcomp-5": packet(0x70, u16(1) + b"\x92"),
    # QoS 2, no local, retain as published, retain handling 1; a shared filter.
    "subscribe-5": packet(0x82, u16(1) + subscribe_properties + text("a/+") + b"\x1e" + text("$share/g/x/#") + b"\x01"),
    "subscribe-311": packet(0x82, u16(2) + text("a/#") + b"\x01"),
    "suback-5": packet(0x90, u16(1) + props() + b"\x01\x80"),
    "suback-311": packet(0x90, u16(1) + b"\x01"),
    "unsubscribe-5": packet(0xA2, u16(3) + props() + text("a/+")),
    "unsubscribe-311": packet(0xA2, u16(3) + text("a")),
    "unsuback-5": packet(0xB0, u16(3) + props() + b"\x00\x11"),
    "unsuback-311": packet(0xB0, u16(3)),
    "pingreq": packet(0xC0, b""),
    "pingresp": packet(0xD0, b""),
    "disconnect-5": packet(0xE0, b"\x04" + props(b"\x11" + u32(0), b"\x1f" + text("s"))),
    "disconnect-311": packet(0xE0, b""),
    "auth-5": packet(0xF0, b"\x18" + auth_properties),
}

# The place's index in PropertyPlace, then the block.
properties = {
    "connect": b"\x00" + connect_properties,
    "connack": b"\x01" + connack_properties,
    "publish": b"\x02" + publish_properties,
    "will": b"\x03" + will_properties,
    "subscribe": b"\x08" + subscribe_properties,
    "auth": b"\x0d" + auth_properties,
}

# Granted filter, requested filter or name, and a name, split at U+0000.
topics = {
    "level-wildcard": b"a/+\0a/b",
    "dollar": b"#\0$SYS/x",
    "shared": b"$share/g/a/+\0a/b",
    "chain": b"sport/#\0sport/tennis/+\0sport/tennis/player1",
    "empty-levels": b"/+/\0//\0/a/",
}

for format, seeds in (("packet", packets), ("properties", properties), ("topic", topics)):
    directory = corpus / format
    directory.mkdir(parents=True, exist_ok=True)
    for name, data in seeds.items():
        (directory / name).write_bytes(data)
