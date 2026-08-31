import os.path
from typing import List, Literal


_MATCH_BEG: int = 3
_MATCH_SIZE: int = 15
_BUFFER_SIZE: int = 4096
_DICT_SIZE: int = 256
_FILE_SIGNATURE: bytearray = bytearray(b'AKLZ~?Qd=\xcc\xcc\xcd')
_ENDIAN: Literal['big', 'little'] = 'big'


def decompress(file_in: bytearray) -> bytearray:

    file_in_ptr = 0
    file_sig = file_in[file_in_ptr:file_in_ptr + 12]
    file_in_ptr += 12

    if not file_sig == _FILE_SIGNATURE:
        return bytearray(file_in)

    file_in_size = len(file_in)
    file_out_size = int.from_bytes(bytes=file_in[file_in_ptr:file_in_ptr + 4], byteorder=_ENDIAN)
    file_in_ptr += 4
    cur_out_size = 0
    file_out = bytearray(b'\x00' * file_out_size)

    # loop_counter = 0
    while file_in_ptr < file_in_size and cur_out_size < file_out_size:
        # loop_counter += 1
        # if loop_counter % 100 == 0:
        #     print(f'Loop: {loop_counter} - Bytes read: {file_in_ptr}')

        flag_byte = file_in[file_in_ptr]
        file_in_ptr += 1

        for i in range(8):
            flag: bool = not (flag_byte & 1) == 0
            flag_byte >>= 1

            if flag:
                file_out[cur_out_size] = file_in[file_in_ptr]
                file_in_ptr += 1
                cur_out_size += 1

            else:
                raw_match_pos1 = file_in[file_in_ptr]
                raw_match_pos2 = (file_in[file_in_ptr + 1] & 0xF0) << 4
                raw_match_pos = (raw_match_pos1 | raw_match_pos2) + 18  # rename raw_match_pos
                match_len = (file_in[file_in_ptr + 1] & _MATCH_SIZE) + _MATCH_BEG
                file_in_ptr += 2
                match_loc = raw_match_pos
                num8 = int(cur_out_size / _BUFFER_SIZE)

                for j in range(num8):
                    if raw_match_pos + j * _BUFFER_SIZE < cur_out_size:
                        match_loc += _BUFFER_SIZE

                if match_loc > cur_out_size:
                    match_loc -= _BUFFER_SIZE

                for j in range(match_len):
                    if match_loc < 0:
                        file_out[cur_out_size] = 0
                    else:
                        file_out[cur_out_size] = file_out[match_loc]
                    cur_out_size += 1
                    if cur_out_size >= len(file_out):
                        return file_out

                    match_loc += 1

            if file_in_ptr >= file_in_size or cur_out_size >= file_out_size:
                break

    if not len(file_out) == file_out_size:
        print(f'Incorrect file size: expected {file_out_size}, got {len(file_out)}')

    return file_out


def compress(file_in: bytearray) -> bytearray:

    if file_in[:len(_FILE_SIGNATURE)] == _FILE_SIGNATURE:
        return file_in

    file_size = len(file_in)  # rename file_in_size
    file_out = bytearray(b'')
    array = bytearray(file_in)
    lzWindowDictionary = LzWindowDictionary()
    lzWindowDictionary.SetWindowSize(4096)
    lzWindowDictionary.SetMaxMatchAmount(18)
    file_out += _FILE_SIGNATURE
    file_out_size = bytearray(file_size.to_bytes(length=4, byteorder=_ENDIAN))
    file_out += file_out_size

    mirror_hex_array = [hex(file_out[i]) for i in range(len(file_out))]

    # loop_counter = 0
    cur_byte = 0

    while cur_byte < file_size:
        # loop_counter += 1
        # if loop_counter % 13 == 0:
        #     print(f'Loop: {loop_counter} - Bytes read: {num2}')

        flag_byte = 0
        cur_bytes: bytearray = bytearray(b'')

        for i in range(8):
            array2 = lzWindowDictionary.Search(array, cur_byte, file_size)
            if array2[1] > 0:
                num5: int = array2[1] - 3
                num6: int = array2[0] - 18
                value: bytes = (num6 & 0xFF).to_bytes(length=1, byteorder=_ENDIAN)
                value2: bytes = (num5 | ((num6 & 0xF00) >> 4)).to_bytes(length=1, byteorder=_ENDIAN)
                cur_bytes += value
                cur_bytes += value2
                lzWindowDictionary.AddEntryRange(array, cur_byte, array2[1])
                lzWindowDictionary.SlideWindow(array2[1])
                cur_byte += array2[1]
                # num3 += 2
            else:
                flag_byte = (flag_byte | (1 << i))
                cur_bytes += array[cur_byte].to_bytes(length=1, byteorder=_ENDIAN)
                lzWindowDictionary.AddEntry(array, cur_byte)
                lzWindowDictionary.SlideWindow(1)
                cur_byte += 1

            if cur_byte >= file_size:
                break

        file_out += flag_byte.to_bytes(length=1, byteorder=_ENDIAN)
        file_out += cur_bytes
        mirror_hex_array.append(hex(flag_byte))
        for byte in cur_bytes:
            mirror_hex_array.append(hex(byte))

    return file_out


def is_compressed(file_in: bytearray) -> bool:
    return file_in[:len(_FILE_SIGNATURE)] == _FILE_SIGNATURE


class LzWindowDictionary:
    _max_window_length: int = 4096
    _window_start: int = -18
    _cur_window_length: int = 15
    _min_match_length: int = 3
    _max_match_length: int = 18
    _BlockSize: int = 0
    _offset_lists: List[List[int]]
    _DictionarySize: int = 256
    _WindowLengthIsSet = False

    def __init__(self):
        self._offset_lists = [[] for _ in range(self._DictionarySize)]
        self._cur_window_length -= self._window_start
        for i in range(self._window_start, 0):
            self._offset_lists[0].append(i)

    def Search(self, decomp_data: bytearray, cur_pos: int, file_length: int) -> List[int]:
        self.RemoveOldEntries(decomp_data[cur_pos])

        finished = file_length - cur_pos < self._min_match_length

        if finished:  # removed pt1 to allow for negative zero seeding
            return [0, 0]
        best_match: List[int] = [0, 0]

        for match_start in reversed(self._offset_lists[decomp_data[cur_pos]]):
            new_len: int = 1
            while new_len < self._max_match_length:
                if new_len >= self._cur_window_length:
                    break
                if cur_pos + new_len >= file_length:
                    break
                if match_start + new_len > cur_pos:
                    if decomp_data[match_start + new_len] not in [0, decomp_data[cur_pos + new_len]]:
                        break
                if match_start + new_len >= 0:
                    if decomp_data[cur_pos + new_len] != decomp_data[match_start + new_len]:
                        break
                else:
                    if decomp_data[cur_pos + new_len] != 0:
                        break

                new_len += 1

            if new_len >= self._min_match_length and new_len >= best_match[1]:
                if best_match[0] < 0 and match_start < -1 and new_len == best_match[1]:
                    continue
                best_match = [match_start, new_len]
                if new_len == self._max_match_length:
                    break
        return best_match

    def SlideWindow(self, amount: int):
        if self._cur_window_length == self._max_window_length:
            self._window_start += amount
            return

        if self._cur_window_length + amount <= self._max_window_length:
            self._cur_window_length += amount
            return

        amount -= self._max_window_length - self._cur_window_length
        self._cur_window_length = self._max_window_length
        self._window_start += amount

    def SlideBlock(self):
        self._window_start += self._BlockSize

    def RemoveOldEntries(self, index: int):
        num: int = 0
        while num < len(self._offset_lists[index]) and self._offset_lists[index][num] < self._window_start:
            self._offset_lists[index].pop(0)

    def SetWindowSize(self, size: int):
        self._max_window_length = size

    def SetMinMatchAmount(self, amount: int):
        self._min_match_length = amount

    def SetMaxMatchAmount(self, amount: int):
        self._max_match_length = amount

    def SetBlockSize(self, size: int):
        self._BlockSize = size
        self._cur_window_length = size

    def AddEntry(self, DecompressedData: bytearray, offset: int):
        self._offset_lists[DecompressedData[offset]].append(offset)

    def AddEntryRange(self, DecompressedData: bytearray, offset: int, length: int):
        for i in range(length):
            self.AddEntry(DecompressedData, offset + i)
