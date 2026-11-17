"""OEM aliases and Unicode long names with independently known collisions."""
from duplicate_fixture import long_name
from short_name_fixture import entries


def fixture(original, bits, mode):
    data = bytearray(original)
    names = entries(original, bits)
    parts, first = names[0]
    other_parts, second = names[1]
    for part in parts:
        data[part] = 0xe5
    first_alias = b'\x82FILE   TXT'
    second_alias = b'\x90FILE   TXT'
    if mode == 'distinct-pages':
        first_alias, second_alias = b'\x9bFILE   TXT', b'\x9dFILE   TXT'
    elif mode == 'punctuation':
        first_alias = b'\x82FI?E   TXT'
    elif mode == 'display':
        first_alias = b'\x9bFILE   TXT'
    elif mode == 'escaped-lead':
        first_alias = b'\x05FILE   TXT'
    data[first:first + 11] = first_alias
    if mode in ['alias-alias', 'distinct-pages']:
        for part in other_parts:
            data[part] = 0xe5
        data[second:second + 11] = second_alias
    elif mode == 'alias-lfn':
        long_name(data, other_parts, second, 'ÉFILE.TXT')
    return data, first


def lowercase_alias(data, bits, flags, index=0):
    parts, entry = entries(data, bits)[index]
    for part in parts:
        data[part] = 0xe5
    data[entry:entry + 11] = b'\x90LOWER  \x99XT'
    data[entry + 12] = flags
    return entry


# Independent reference pairs for Central European, Portuguese and Cyrillic DOS.
EXTENDED_PAGES = {
    852: (b'\xa4\xa5\x9d', 'Ą', 'ą', 'ł'),
    860: (b'\x8e\x84\x99', 'Ã', 'ã', 'õ'),
    866: (b'\x80\xa0\xf0', 'А', 'а', 'ё'),
}


def extended_fixture(original, bits, cp, lfn=False):
    data = bytearray(original)
    names = entries(original, bits)
    codes, upper, lower, display = EXTENDED_PAGES[cp]
    for index, lead in enumerate(codes):
        parts, entry = names[index]
        for part in parts:
            data[part] = 0xe5
        data[entry:entry + 11] = bytes([lead]) + (b'FILE   TXT' if index < 2 else b'LOWER  TXT')
        if index == 2:
            data[entry + 12] = 24
    if lfn:
        parts, entry = names[1]
        # Restore the group's ordinal bytes before installing the Unicode name.
        for part in parts:
            data[part] = original[part]
        data[entry:entry + 11] = b'OTHER   TXT'
        long_name(data, parts, entry, lower + 'FILE.TXT')
    return data, [upper + 'FILE.TXT', lower + 'FILE.TXT', display + 'lower.txt']
