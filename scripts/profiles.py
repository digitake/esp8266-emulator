"""Optional GPIO display profiles, independent of firmware names and flash sizes."""
import json
from pathlib import Path
import re

MAX_PROFILE_BYTES = 64 * 1024


def _pin(value, field):
    if type(value) is not int or not 0 <= value <= 16:
        raise ValueError('%s must be a GPIO number from 0 through 16' % field)
    return value


def _boolean(value, field):
    if type(value) is not bool:
        raise ValueError('%s must be true or false' % field)
    return value


def validate_profile(data):
    """Return a normalized profile; reject unknown fields and unsafe display values."""
    if not isinstance(data, dict):
        raise ValueError('profile must be a JSON object')
    allowed = {'name', 'title', 'kind', 'gangs', 'status_led', 'status_led_active_low'}
    if set(data) - allowed:
        raise ValueError('unknown profile fields: %s' % ', '.join(sorted(set(data) - allowed)))
    name = data.get('name')
    if not isinstance(name, str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_-]{0,63}', name):
        raise ValueError('profile name must be 1–64 letters, digits, hyphens or underscores')
    title = data.get('title', name)
    if not isinstance(title, str) or not 1 <= len(title) <= 80 or any(ord(c) < 32 for c in title):
        raise ValueError('profile title must be 1–80 printable characters')
    kind = data.get('kind', 'generic')
    if kind not in ('generic', 'switch', 'plug'):
        raise ValueError('profile kind must be generic, switch or plug')
    gangs = data.get('gangs', [])
    if not isinstance(gangs, list) or len(gangs) > 8:
        raise ValueError('profile gangs must be a list of at most 8 groups')
    normalized = []
    for i, group in enumerate(gangs):
        fields = {'button', 'relay', 'led', 'button_active_low', 'led_active_low'}
        if not isinstance(group, dict) or set(group) - fields:
            raise ValueError('group %d has invalid fields' % (i + 1))
        row = {role: _pin(group.get(role), 'group %d %s' % (i + 1, role))
               for role in ('button', 'relay', 'led')}
        for field in ('button_active_low', 'led_active_low'):
            row[field] = _boolean(group.get(field, True), 'group %d %s' % (i + 1, field))
        normalized.append(row)
    status_led = data.get('status_led')
    if status_led is not None:
        status_led = _pin(status_led, 'status_led')
    return {'name': name, 'title': title, 'kind': kind, 'gangs': normalized,
            'status_led': status_led,
            'status_led_active_low': _boolean(data.get('status_led_active_low', True), 'status_led_active_low')}


def load_profile(path=None):
    """Read a bounded JSON profile, or return the generic profile with no pin roles."""
    if path is None:
        return validate_profile({'name': 'generic', 'title': 'Generic ESP8266'})
    with Path(path).open('rb') as stream:
        raw = stream.read(MAX_PROFILE_BYTES + 1)
    if len(raw) > MAX_PROFILE_BYTES:
        raise ValueError('profile exceeds 64 KiB')
    return validate_profile(json.loads(raw.decode('utf-8')))


def pin_roles(profile):
    """GPIO labels for evidence; later roles win if a pin serves multiple purposes."""
    names = {}
    for role in ('relay', 'led', 'button'):
        for i, group in enumerate(profile['gangs'], 1):
            names[group[role]] = '%s%d' % (role, i)
    return names
