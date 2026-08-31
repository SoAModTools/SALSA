from __future__ import annotations

import os
import ctypes
from typing import Union

aklz_lib = 'AKLZ.dll'


class BufWSize(ctypes.Structure):
    _fields_ = [("size", ctypes.c_int),
                ("buffer", ctypes.POINTER(ctypes.c_char))]

# ################ #
# # AKLZ Methods # #
# ################ #


def get_dll_path():
    dll_path = os.path.join(os.path.dirname(os.path.realpath(__file__)), aklz_lib)
    if not os.path.exists(dll_path):
        return ''
    return dll_path


# ----------------- #
# Compression Check #
# ----------------- #

def is_compressed(buffer_in: bytearray):
    dll_path = os.path.join(os.path.dirname(os.path.realpath(__file__)), aklz_lib)
    if not os.path.exists(dll_path):
        raise FileExistsError(dll_path + ' does not exist')
    dll = ctypes.cdll.LoadLibrary(dll_path)

    IsCompressed = dll.is_compressed
    IsCompressed.argtypes = (ctypes.POINTER(ctypes.c_char),)
    IsCompressed.restype = ctypes.c_int

    p = ctypes.c_char_p(bytes(buffer_in))
    p2 = ctypes.cast(p, ctypes.POINTER(ctypes.c_char))
    result = IsCompressed(p2)
    return result == 0


# ------------------ #
# Decompress Methods #
# ------------------ #

def decompress(buffer_in: bytearray):
    return _decompress(buffer_in)


def decompress_from_file(filepath_in) -> bytearray:
    return _file_decompress(source=filepath_in)


def decompress_to_file(bytes_in: bytearray, filepath_out: str) -> None:
    _file_decompress(source=bytes_in, filepath_out=filepath_out)


def decompress_from_file_to_file(filepath_in: str, filepath_out: str) -> None:
    _file_decompress(source=filepath_in, filepath_out=filepath_out)


def _file_decompress(source: Union[bytearray, str] = None, filepath_out: Union[None, str] = None) -> Union[None, bytearray]:
    if isinstance(source, str):
        with open(source, 'rb') as file_h:
            source = file_h.read()
    result = _decompress(source)
    if filepath_out is None:
        return result
    with open(filepath_out, 'wb') as file_h:
        file_h.write(bytes(result))


def _decompress(buffer_in):
    dll_path = os.path.join(os.path.dirname(os.path.realpath(__file__)), aklz_lib)
    if not os.path.exists(dll_path):
        raise FileExistsError(dll_path + ' does not exist')
    dll = ctypes.cdll.LoadLibrary(dll_path)

    Decompress = dll.decompress
    Decompress.argtypes = ctypes.c_void_p, ctypes.c_void_p
    Decompress.restype = ctypes.c_int

    p = ctypes.c_char_p(bytes(buffer_in))
    p2 = ctypes.cast(p, ctypes.POINTER(ctypes.c_char))
    buf_in = BufWSize(len(buffer_in), p2)
    buf_out = BufWSize()
    result = Decompress(ctypes.addressof(buf_in), ctypes.addressof(buf_out))
    if result != 0:
        print("Decompressed failed")
    decompressed = bytearray(ctypes.string_at(buf_out.buffer, size=buf_out.size))
    return decompressed

# ---------------- #
# Compress Methods #
# ---------------- #


def compress(buffer_in: bytearray):
    return _compress(buffer_in)


def compress_from_file(filepath_in) -> bytearray:

    return _file_compress(source=filepath_in)


def compress_to_file(bytes_in: bytearray, filepath_out: str) -> None:
    _file_compress(source=bytes_in, filepath_out=filepath_out)


def compress_from_file_to_file(filepath_in: str, filepath_out: str) -> None:
    _file_compress(source=filepath_in, filepath_out=filepath_out)


def _file_compress(source: Union[bytearray, str] = None, filepath_out: Union[None, str] = None) -> Union[None, bytearray]:
    if isinstance(source, str):
        with open(source, 'rb') as file_h:
            source = file_h.read()
    result = _compress(source)
    if filepath_out is None:
        return result
    with open(filepath_out, 'wb') as file_h:
        file_h.write(bytes(result))


def _compress(buffer_in):
    dll_path = os.path.join(os.path.dirname(os.path.realpath(__file__)), aklz_lib)
    dll = ctypes.cdll.LoadLibrary(dll_path)

    Compress = dll.compress
    Compress.argtypes = ctypes.c_void_p, ctypes.c_void_p
    Compress.restype = ctypes.c_int

    p = ctypes.c_char_p(bytes(buffer_in))
    p2 = ctypes.cast(p, ctypes.POINTER(ctypes.c_char))
    buf_in = BufWSize(len(buffer_in), p2)
    buf_out = BufWSize()
    result = Compress(ctypes.addressof(buf_in), ctypes.addressof(buf_out))
    if result != 0:
        print("Compression failed")
    compressed = bytearray(ctypes.string_at(buf_out.buffer, size=buf_out.size))
    return compressed