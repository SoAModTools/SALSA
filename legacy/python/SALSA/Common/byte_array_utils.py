import math
import re
import struct
from typing import Union


def getWord(file: bytearray, pos: int, out='int'):
    if pos + 4 > len(file):
        raise IndexError(f'This the input bytearray is not long enough to contain a word')
    posByte1 = file[pos]
    posByte2 = file[pos + 1]
    posByte3 = file[pos + 2]
    posByte4 = file[pos + 3]
    word = posByte1 * 16777216 + posByte2 * 65536 + posByte3 * 256 + posByte4
    if out == 'int':
        return word
    if out == 'hex':
        return padded_hex(word, 8)


def getHalf(file: bytearray, pos: int, out='int'):
    posByte1 = file[pos]
    posByte2 = file[pos + 1]
    half = posByte1 * 256 + posByte2
    if out == 'int':
        return half
    if out == 'hex':
        return padded_hex(half, 4)


def getByte(file: bytearray, pos: int, out='int'):
    byte = file[pos]
    if out == 'int':
        return byte
    if out == 'hex':
        return padded_hex(byte)


def getString(file: bytearray, pos: int, enc="UTF-8", replace=True):
    lengths = deriveStringLength(file, pos)
    if lengths[0] == -1:
        return ''
    stringBytes = []
    for l in lengths:
        tempStringBytes = file[pos:pos + l]
        tempStringBytes = tempStringBytes.replace(b'\x7f', b'\x20')
        stringBytes.append(tempStringBytes)
        pos += l + 1
    string = ''
    for s in range(0, len(stringBytes)):
        if s > 0:
            string += '*'
        tempString = stringBytes[s].decode(encoding=enc, errors='backslashreplace')
        if re.search('/.*', tempString):
            tempString = '*'
        string += tempString
    if replace:
        string = string.replace('\\', '/')
        string = string.replace('//', '/')
        string = string.replace('/x81', '*')
        string = string.replace('*c', '... ')
        string = string.replace('*s', '<<')
        string = string.replace('*t', '>>')
        string = string.replace('/n', '\n')
    if re.search('<<.+>>', string):
        pos = re.search('<<.+>>', string).span()[1]
        string = '{0}\n{1}'.format(string[:pos+1], string[pos+1:])
        # print(string)
    return string


def deriveStringLength(file: bytearray, pos: int, terminators=None):

    if terminators is None:
        terminators = [0]
    currentCharIndex = pos
    currentChar = file[currentCharIndex]
    if currentChar in terminators:
        return [-1]
    lengths = []
    length = 0

    while currentChar not in terminators:
        currentCharIndex += 1
        length += 1
        if currentCharIndex == len(file):
            length -= 1
            break
        currentChar = file[currentCharIndex]

    lengths.append(length)

    return lengths


def hex2Float(s: str):
    if not isinstance(s, str):
        print("unable to convert {0} to float - Not a string,  {1}".format(s, type(s)))
        return s
    sLen = len(s)
    if sLen not in (8, 10):
        print("Unable to convert {0} to float - Not the correct size, [1]".format(s, sLen))
        return s
    h = s
    if re.search("^0x", s):
        h = s[2:]
    f = struct.unpack('!f', bytes.fromhex(h))[0]
    return f


def float2Hex(f: float, form: str):
    return bytearray(struct.pack(form, f))


def word2SignedInt(val):
    if isinstance(val, bytearray) or isinstance(val, bytes):
        val = '0x' + val.hex()
    if val[:2] != '0x':
        val = '0x'+val

    uintval = int(val, 16)
    bits = 4 * (len(val) - 2)
    if uintval >= math.pow(2, bits - 1):
        uintval = int(0 - (math.pow(2, bits) - uintval))
    return uintval


def applyHexMask(h: str, m: str) -> str:
    """Takes in a hex, applies a mask (hex), returns a hex with the same padding as the original"""
    inputValue = int(h, 16)
    mask = int(m, 16)
    p = len(m) - 2 if m[:2] == '0x' else len(m)

    maskedValue = (inputValue & mask)
    outputHex = padded_hex(maskedValue, p=p)
    return outputHex


def getTypeFromString(string: str):
    """Differentiates between hex and others"""
    if re.search('^0x', string):
        return 'hex'
    else:
        return 'int'


def is_a_number(num: str):
    return re.search('^[0-9,.]+$', num) is not None and 0 < len(num.split('.')) <= 2


def is_hex(num: str):
    return re.search('^[0-9,a-f]+$', num) is not None or re.search('^0x[0-9,a-f]+$', num) is not None


def toInt(string: Union[str, int, float]):
    """Converts either hex, float, or int string into an int. Performs 'Floor' on floats"""
    if isinstance(string, int):
        return string
    if not isinstance(string, str):
        string = str(string)
    if re.search('^0x', string):
        return int(string, 16)
    elif string.find('.') > -1:
        string = string[:string.find('.')]
    return int(string)


def toFloat(string: str):
    """Converts either hex, float, or int string into an int. Performs 'Floor' on floats"""
    if isinstance(string, float):
        return string
    if not isinstance(string, str):
        string = str(string)
    if re.search('^0x', string):
        return int(string, 16)
    return float(string)


def asStringOfType(input_num, input_type: str):
    """Takes in a number and returns a string, either hex style or int style"""
    if input_type == 'hex':
        return hex(input_num)
    else:
        return str(input_num)


def pad_hex(hex: str, digits: int):
    if hex[:2] == '0x':
        hex = hex[2:]
    if digits <= len(hex):
        return '0x'+hex
    padding = '0' * (digits - len(hex))
    return '0x'+padding+hex


def padded_hex(i: int, p: int = -1):
    """Returns -1 if hex is too large to process"""
    lengths = [2, 4, 8]
    hexValue = hex(i)
    hexField = hexValue[2:]
    padding = ''
    if p > 0:
        padding = '0' * (p - len(hexField))
    else:
        size = -1
        for l in lengths:
            if len(hexField) <= l:
                size = l
                break
        if size == -1:
            print('hex too large to process')
            return -1
        padding = '0' * (size - len(hexField))

    paddedHex = '0x{0}{1}'.format(padding, hexField)
    return paddedHex


def is_utf_8_file_character(byte):
    letters = [46, *range(48, 58), *range(65, 91), 95, *range(97, 123)]
    return byte in letters


def run_tests():
    print('Testing padded_hex()')
    paddingTest()
    print('\nTesting applyHexMask()')
    maskTest()


def maskTest():
    inputs = {'0x08010203': '0xffff'}
    expected = {'0x08010203': '0x00000203'}
    for key, value in inputs.items():
        print('\tMask test: input value: {0}, mask: {1}'.format(key, value))
        try:
            out = applyHexMask(key, value)
        except Exception as e:
            print('\tTest Failed\n{}'.format(e))
        else:
            print('\tTest Passed with expected result: {0}, \n\t\texpected, {1} \n\t\toutput: {2}\n'.format(
                out == expected[key],
                expected[key], out))


def paddingTest():
    inputs = {'0x1': -1, '0x10': -1, '0x100': -1, '0x1000': -1, '0x10000': -1, '0x100000': -1, '0x1000000': -1,
              '0x10000000': -1, '0x100000000': -1, '0x2': 2, '0x6': 6, '0x4': 4, '0x8': 8}
    expected = {'0x1': '0x01', '0x10': '0x10', '0x100': '0x0100', '0x1000': '0x1000', '0x10000': '0x00010000',
                '0x100000': '0x00100000', '0x1000000': '0x01000000', '0x10000000': '0x10000000',
                '0x100000000': -1, '0x2': '0x02', '0x6': '0x000006', '0x4': '0x0004', '0x8': '0x00000008'}

    for key, value in inputs.items():
        print('\tPadding test: input value: {0}, padding level: {1}'.format(key, value))
        try:
            out = padded_hex(int(key, 16), value)
        except Exception as e:
            print('\tTest Failed\n{}'.format(e))
        else:
            print('\tTest Passed with expected result: {0}, \n\t\texpected, {1} \n\t\toutput: {2}\n'.format(
                out == expected[key],
                expected[key], out))


if __name__ == '__main__':
    run_tests()
