"""Local MQTT service framing and network OTA evidence checks."""
import sys
import threading
import unittest
from unittest.mock import Mock
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import network_check


class NetworkServiceTests(unittest.TestCase):
    def test_http_server_can_close_before_start(self):
        server = network_check.HttpUpdateServer.__new__(network_check.HttpUpdateServer)
        server.stop_event = threading.Event()
        server.listener = Mock()
        server.thread = threading.Thread(target=lambda: None)
        server.close()

    def test_mqtt_parser_handles_fragmented_and_coalesced_packets(self):
        connect = network_check.encode_packet(0x10, b'connect fixture')
        ping = network_check.encode_packet(0xc0)
        parser = network_check.MqttStreamParser()
        self.assertEqual(parser.feed(connect[:1]), [])
        self.assertEqual(parser.feed(connect[1:3]), [])
        self.assertEqual(parser.feed(connect[3:] + ping), [(0x10, b'connect fixture'), (0xc0, b'')])
        large = network_check.encode_packet(0x30, b'x' * 130)
        self.assertEqual(large[:3], b'\x30\x82\x01')
        self.assertEqual(parser.feed(large[:2]), [])
        self.assertEqual(parser.feed(large[2:40]), [])
        self.assertEqual(parser.feed(large[40:]), [(0x30, b'x' * 130)])
        self.assertEqual(parser.buffer, bytearray())

    def test_mqtt_parser_rejects_overlong_or_oversized_remaining_length(self):
        parser = network_check.MqttStreamParser(max_packet=32)
        with self.assertRaisesRegex(network_check.ProtocolError, 'more than four'):
            parser.feed(b'\x30\x80\x80\x80\x80\x00')
        parser = network_check.MqttStreamParser(max_packet=32)
        with self.assertRaisesRegex(network_check.ProtocolError, 'size limit'):
            parser.feed(b'\x30\x7f')

    def test_subscribe_and_publish_payloads_are_decoded(self):
        payload = b'\x00\x07\x00\x0cemulator/ota\x00'
        packet_id, topics = network_check.parse_subscribe(payload)
        self.assertEqual(packet_id, b'\x00\x07')
        self.assertEqual(topics, [(b'emulator/ota', 0)])
        topic, value = network_check.decode_publish(
            b'\x00\x10emulator/versionA')
        self.assertEqual((topic, value), (b'emulator/version', b'A'))

    def test_evidence_requires_boot_two_b_publish_on_new_connection_and_exact_flash(self):
        events = [
            {'t': 'stage', 's': 'rom', 'boot': 1},
            {'t': 'stage', 's': 'loop', 'boot': 1},
            {'t': 'stage', 's': 'rom', 'boot': 2},
            {'t': 'stage', 's': 'loop', 'boot': 2},
        ]
        broker = {'versions': ['A', 'B'], 'version_connections': [
            {'connection': 1, 'version': 'A'}, {'connection': 2, 'version': 'B'}],
            'subscribed': True, 'subscription_connections': [1], 'ota_sent': True,
            'version_b_seen_at': 10.0}
        http = {'requests': 1, 'status': 200, 'flash_prefix': b'\xe9new image'}
        uart = ' ets Jan  8 2013\nold\n ets Jan  8 2013\nnetwork reference firmware B\n'
        report = {'end': 'quit', 'halt': '', 'boots': 2}
        checks = network_check.build_checks(events, report, broker, http, uart,
                                            b'\xe9new image', 12.0)
        self.assertTrue(all(checks.values()))

        checks = network_check.build_checks(events, report, broker, http, uart,
                                            b'\xe9new image', 11.99)
        self.assertFalse(checks['version_b_stable_before_emulator_exit'])

        same_connection = dict(broker, version_connections=[
            {'connection': 1, 'version': 'A'}, {'connection': 1, 'version': 'B'}])
        checks = network_check.build_checks(events, report, same_connection, http, uart,
                                            b'\xe9new image', 12.0)
        self.assertFalse(checks['version_b_used_new_mqtt_connection'])

        later_reset = events + [{'t': 'stage', 's': 'rom', 'boot': 3}]
        checks = network_check.build_checks(later_reset, report, broker, http, uart,
                                            b'\xe9new image', 12.0)
        self.assertFalse(checks['exactly_two_rom_boots'])
        self.assertFalse(checks['no_later_boot'])


if __name__ == '__main__':
    unittest.main()
