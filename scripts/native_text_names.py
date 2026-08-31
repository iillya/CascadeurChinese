"""Cascadeur 2026.1.2 TextUtils rules recovered from presenter_lib.dll.

Pure offline implementation for ASCII identifiers. Non-ASCII is intentionally
rejected: Python Unicode and Qt 6.5.3 QChar casing are not interchangeable.
See docs/native-text-utils.md and the independent Qt reference probe.
"""
import re

CAMEL_FIRST = re.compile(r'(.)([A-Z][a-z]+)')
CAMEL_SECOND = re.compile(r'([a-z])([A-Z])')
DOMAIN_PREFIX = re.compile(r'([a-zA-Z]+)(::)')
DLL_SHA256 = '2B2FD4043F1F83B92338DC4FE7E0F0866F3C7C554A3BF5D92387A112FA1470F1'


def _ascii(text):
    if not isinstance(text, str) or not text.isascii():
        raise ValueError('Only ASCII identifiers are verified; use the Qt reference probe for Unicode')
    return text


def from_camel_case(text):
    text = _ascii(text)
    if not text:
        return text
    text = CAMEL_FIRST.sub(r'\1 \2', text)
    text = CAMEL_SECOND.sub(r'\1 \2', text).lower()
    return text[:1].upper()+text[1:]


def from_snake_case(text):
    text = _ascii(text).replace('_', ' ')
    return text[:1].upper()+text[1:]


def from_any_case(text):
    return from_camel_case(from_snake_case(text))


def from_domain_object_type(text):
    text = _ascii(text)
    # QString::split does NOT return captured delimiter groups like re.split.
    # KeepEmptyParts is enabled. The native code selects the final segment.
    end = 0
    for match in DOMAIN_PREFIX.finditer(text):
        end = match.end()
    return from_camel_case(text[end:])
