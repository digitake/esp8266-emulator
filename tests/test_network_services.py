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
            'config_connections': [{'connection': 1, 'marker': 'persisted-v1'},
                                   {'connection': 2, 'marker': 'persisted-v1'}],
            'subscribed': True, 'subscription_connections': [1], 'ota_sent': True,
            'version_b_seen_at': 10.0}
        http = {'requests': 1, 'status': 200, 'flash_prefix': b'\xe9new image'}
        uart = (' ets Jan  8 2013\nold\nnetwork lifecycle mode-off disconnected\n'
                'network lifecycle station dhcp restored\nconfiguration marker persisted-v1\n'
                ' ets Jan  8 2013\n'
                'network reference firmware B\nnetwork lifecycle mode-off disconnected\n'
                'network lifecycle station dhcp restored\nconfiguration marker persisted-v1\n')
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

    def test_web_upload_requires_real_post_and_no_mqtt_ota_trigger(self):
        events = [
            {'t': 'stage', 's': 'rom', 'boot': 1}, {'t': 'stage', 's': 'loop', 'boot': 1},
            {'t': 'stage', 's': 'rom', 'boot': 2}, {'t': 'stage', 's': 'loop', 'boot': 2},
        ]
        broker = {'versions': ['A', 'B'], 'version_connections': [
            {'connection': 1, 'version': 'A'}, {'connection': 2, 'version': 'B'}],
            'config_connections': [{'connection': 1, 'marker': 'persisted-v1'},
                                   {'connection': 2, 'marker': 'persisted-v1'}],
            'subscribed': True, 'subscription_connections': [1], 'ota_sent': False,
            'version_b_seen_at': 10.0}
        http = {'requests': 0, 'status': 0, 'flash_prefix': b'\xe9new image'}
        upload = {'status': 200, 'response': b'HTTP/1.1 200 OK\r\n\r\nUpdate Success!',
                  'sent': True}
        uart = (' ets Jan  8 2013\nold\nnetwork lifecycle mode-off disconnected\n'
                'network lifecycle station dhcp restored\nconfiguration marker persisted-v1\n'
                ' ets Jan  8 2013\n'
                'network reference firmware B\nnetwork lifecycle mode-off disconnected\n'
                'network lifecycle station dhcp restored\nconfiguration marker persisted-v1\n')
        report = {'end': 'quit', 'halt': '', 'boots': 2}
        checks = network_check.build_checks(events, report, broker, http, uart,
                                            b'\xe9new image', 12.0, 'web', upload)
        self.assertTrue(all(checks.values()))

        missing_boot2_lifecycle = uart.replace(
            'network reference firmware B\nnetwork lifecycle mode-off disconnected\n'
            'network lifecycle station dhcp restored\nconfiguration marker persisted-v1\n',
            'network reference firmware B\nconfiguration marker persisted-v1\n')
        checks = network_check.build_checks(events, report, broker, http, missing_boot2_lifecycle,
                                            b'\xe9new image', 12.0, 'web', upload)
        self.assertFalse(checks['boot2_wifi_mode_off_disconnected'])
        self.assertFalse(checks['boot2_wifi_station_reacquired_dhcp'])

        triggered = dict(broker, ota_sent=True)
        checks = network_check.build_checks(events, report, triggered, http, uart,
                                            b'\xe9new image', 12.0, 'web', upload)
        self.assertFalse(checks['no_mqtt_ota_publish'])

        failed_upload = dict(upload, response=b'HTTP/1.1 200 OK\r\n\r\nUpdate error: invalid image')
        checks = network_check.build_checks(events, report, broker, http, uart,
                                            b'\xe9new image', 12.0, 'web', failed_upload)
        self.assertFalse(checks['web_upload_succeeded'])

        missing_marker = dict(broker, config_connections=[
            {'connection': 1, 'marker': 'persisted-v1'}, {'connection': 2, 'marker': 'missing'}])
        checks = network_check.build_checks(events, report, missing_marker, http, uart,
                                            b'\xe9new image', 12.0, 'web', upload)
        self.assertFalse(checks['configuration_marker_survived_ota'])

        missing_a_marker = dict(broker, config_connections=[
            {'connection': 1, 'marker': 'missing'}, {'connection': 2, 'marker': 'persisted-v1'}])
        checks = network_check.build_checks(events, report, missing_a_marker, http, uart,
                                            b'\xe9new image', 12.0, 'web', upload)
        self.assertFalse(checks['configuration_marker_reported_on_a_connection'])

    def test_multipart_web_upload_has_firmware_field_and_exact_length(self):
        request = network_check.make_multipart_upload(b'\xe9firmware')
        headers, body = request.split(b'\r\n\r\n', 1)
        self.assertIn(b'POST /update HTTP/1.1', headers)
        self.assertIn(b'name="firmware"; filename="reference.bin"', body)
        self.assertIn(b'\r\n\r\n\xe9firmware\r\n', body)
        length = int(next(line.split(b':', 1)[1] for line in headers.split(b'\r\n')
                          if line.startswith(b'Content-Length:')))
        self.assertEqual(length, len(body))

    def test_raw_update_normalizes_only_header_mode_while_gzip_preserves_it(self):
        running = b'\xe9\x01\x02old-image-data'
        replacement = b'\xe9\x01\x00new-image-data'
        raw_image, source_mode, flash_mode, normalized = network_check.expected_flash_image(
            running, replacement, gzip_transfer=False)
        self.assertEqual(raw_image, b'\xe9\x01\x02new-image-data')
        self.assertEqual((source_mode, flash_mode, normalized), (0, 2, True))

        gzip_image, source_mode, flash_mode, normalized = network_check.expected_flash_image(
            running, replacement, gzip_transfer=True)
        self.assertEqual(gzip_image, replacement)
        self.assertEqual((source_mode, flash_mode, normalized), (0, 0, False))


if __name__ == '__main__':
    unittest.main()
