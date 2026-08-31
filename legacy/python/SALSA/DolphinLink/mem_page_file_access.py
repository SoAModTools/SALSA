#!/usr/bin/env python2
# Released as open source by NCC Group Plc - http://www.nccgroup.com/
# Developed by Nikos Laleas, nikos dot laleas at nccgroup dot com
# https://github.com/nccgroup/memaddressanalysis
# Released under AGPL. See LICENSE for more information
#
# Modified by Jahorta
from ctypes import *
from ctypes.wintypes import *
import sys
import time
import platform

list = []

MEMORY_STATES = {0x1000: "MEM_COMMIT", 0x10000: "MEM_FREE", 0x2000: "MEM_RESERVE"}
MEMORY_PROTECTIONS = {0x10: "EXECUTE", 0x20: "EXECUTE_READ", 0x40: "EXECUTE_READWRITE", 0x80: "EXECUTE_WRITECOPY",
                      0x01: "NOACCESS", 0x04: "READWRITE", 0x08: "WRITECOPY", 0x02: "READONLY"}
MEMORY_TYPES = {0x1000000: "MEM_IMAGE", 0x40000: "MEM_MAPPED", 0x20000: "MEM_PRIVATE"}


class MEMORY_BASIC_INFORMATION32 (Structure):
    _fields_ = [
        ("BaseAddress", c_void_p),
        ("AllocationBase", c_void_p),
        ("AllocationProtect", DWORD),
        ("RegionSize", c_size_t),
        ("State", DWORD),
        ("Protect", DWORD),
        ("Type", DWORD)
        ]


class MEMORY_BASIC_INFORMATION64 (Structure):
    _fields_ = [
        ("BaseAddress", c_ulonglong),
        ("AllocationBase", c_ulonglong),
        ("AllocationProtect", DWORD),
        ("__alignment1", DWORD),
        ("RegionSize", c_ulonglong),
        ("State", DWORD),
        ("Protect", DWORD),
        ("Type", DWORD),
        ("__alignment2", DWORD)
        ]


class SYSTEM_INFO(Structure):
    _fields_ = [("wProcessorArchitecture", WORD),
                ("wReserved", WORD),
                ("dwPageSize", DWORD),
                ("lpMinimumApplicationAddress", LPVOID),
                ("lpMaximumApplicationAddress", LPVOID),
                ("dwActiveProcessorMask", DWORD),
                ("dwNumberOfProcessors", DWORD),
                ("dwProcessorType", DWORD),
                ("dwAllocationGranularity", DWORD),
                ("wProcessorLevel", WORD),
                ("wProcessorRevision", WORD)]


class MEMORY_BASIC_INFORMATION:

    def __init__ (self, MBI):
        self.MBI = MBI
        self.BaseAddress = self.MBI.BaseAddress
        self.AllocationBase = self.MBI.AllocationBase
        self.AllocationProtect = MEMORY_PROTECTIONS.get(self.MBI.AllocationProtect, self.MBI.AllocationProtect)
        self.RegionSize = self.MBI.RegionSize
        self.State = MEMORY_STATES.get(self.MBI.State, self.MBI.State)
        self.Protect = MEMORY_PROTECTIONS.get(self.MBI.Protect, self.MBI.Protect)
        self.Type = MEMORY_TYPES.get(self.MBI.Type, self.MBI.Type)
        self.ProtectBits = self.MBI.Protect


class _PSAPI_WORKING_SET_EX_BLOCK32(Union):
    _fields_ = [
        ("Flags", DWORD),
    ]


class _PSAPI_WORKING_SET_EX_BLOCK64(Union):
    _fields_ = [
        ("Flags", c_ulonglong),
    ]


class _PSAPI_WORKING_SET_EX_INFORMATION32(Structure):
    _fields_ = [
        ("VirtualAddress", DWORD),
        ("VirtualAttributes", _PSAPI_WORKING_SET_EX_BLOCK32),
    ]


class _PSAPI_WORKING_SET_EX_INFORMATION64(Structure):
    _fields_ = [
        ("VirtualAddress", c_ulonglong),
        ("VirtualAttributes", _PSAPI_WORKING_SET_EX_BLOCK64),
    ]


class _PSAPI_WORKING_SET_EX_BLOCK():

    def __init__(self, PWSEB):
        self.PWSEB = PWSEB
        temp_flags = PWSEB.Flags
        self.Valid = temp_flags & 0x1
        temp_flags = temp_flags << 1
        self.ShareCount = temp_flags & 0x111
        temp_flags = temp_flags << 3
        self.Win32Protection = temp_flags & 0x11111111111
        temp_flags = temp_flags << 11
        self.Shared = temp_flags & 0x1
        temp_flags = temp_flags << 1
        self.Node = temp_flags & 0x111111
        temp_flags = temp_flags << 6
        self.Locked = temp_flags & 0x1
        temp_flags = temp_flags << 1
        self.LargePage = temp_flags & 1
        temp_flags = temp_flags << 1
        self.Reserved = temp_flags & 7
        temp_flags = temp_flags << 7
        self.Bad = temp_flags & 1


class _PSAPI_WORKING_SET_EX_INFORMATION():

    def __init__(self, PWSEI):
        self.PWSEI = PWSEI
        self.VirtualAddress = self.PWSEI.VirtualAddress
        self.VirtualAttributes = _PSAPI_WORKING_SET_EX_BLOCK(self.PWSEI.VirtualAttributes)


def get_all_process_mem_regions(pid: int):
    min_address = 0x0
    max_address = 0x0
    permissions = None

    OpenProcess = windll.kernel32.OpenProcess
    CloseHandle = windll.kernel32.CloseHandle

    PROCESS_ALL_ACCESS = 0x1F0FFF

    hProc = OpenProcess(PROCESS_ALL_ACCESS, False, pid)

    arch, process_is32 = validate_process(hProc)

    printt('Starting memory analysis...\n')
    printt('Current Process is %s.\n' % arch)
    if process_is32:
        printt('Target Process is 32bit.\n')
    else:
        printt('Target Process is 64bit.\n')

    if arch == "32bit" and not process_is32:
        printt('Error: Can\'t dump a 64-bit process from a 32-bit one.\n')
        printt('Install python 64-bit.\n')
        quit(1)

    sys.stdout.write("Walking memory...\t")
    si = SYSTEM_INFO()
    psi = byref(si)
    windll.kernel32.GetSystemInfo(psi)
    base_address = si.lpMinimumApplicationAddress
    if max_address == 0:
        if process_is32 and si.lpMaximumApplicationAddress > 0xFFFFFFFF:
            max_address = 0xFFFEFFFF
        else:
            max_address = si.lpMaximumApplicationAddress
    page_address = base_address
    while page_address < max_address:
        next_page = scan_page(hProc, page_address, process_is32, min_address, permissions)
        page_address = next_page

    CloseHandle(hProc)

    return list


def validate_process(hProc):
    arch, type = platform.architecture()
    process_is32 = c_bool()
    success = windll.kernel32.IsWow64Process(hProc, byref(process_is32))
    if not success:
        printt('Failed to check whether target process is under WoW64.\nQuitting...\n')
        exit(1)
    return arch, process_is32


def scan_page(process_handle, page_address, process_is32, min, permissions):
    info = VirtualQueryEx(process_handle, page_address, process_is32)
    base_address = info.BaseAddress
    region_size = info.RegionSize
    next_region = base_address + region_size
    if info.Protect != 0 and page_address >= min:
        modifier = None
        protect = info.Protect
        if info.ProtectBits & 0x100:
            modifier = "GUARD"
            perm = info.ProtectBits ^ 0x100
            protect = MEMORY_PROTECTIONS.get(perm, perm)
        elif info.ProtectBits & 0x200:
            modifier = "NOCACHE"
            perm = info.ProtectBits ^ 0x200
            protect = MEMORY_PROTECTIONS.get(perm, perm)
        elif info.ProtectBits & 0x400:
            modifier = "WRITECOMBINE"
            perm = info.ProtectBits ^ 0x400
            protect = MEMORY_PROTECTIONS.get(perm, perm)

        if permissions is None or protect in permissions:
            add_to_array(format(page_address, 'x'), protect, region_size, modifier)
    return next_region


def add_to_array(address, permission, size, modifier):
    if modifier:
        permission = "%s/%s" % (permission, modifier)
    for i, x in enumerate(list):
        if address in x:
            if list[i][2] == permission:
                list[i][1] += 1
                return
    l = [[address, 1, permission, size]]
    list.extend(l)


def VirtualQueryEx(hProcess, lpAddress, process_is32):
    if process_is32:
        lpBuffer = MEMORY_BASIC_INFORMATION32()
    else:
        lpBuffer = MEMORY_BASIC_INFORMATION64()

    success = windll.kernel32.VirtualQueryEx(hProcess, LPVOID(lpAddress), byref(lpBuffer), sizeof(lpBuffer))
    assert success,  "VirtualQueryEx Failed.\n%s" % (WinError(GetLastError()))
    return MEMORY_BASIC_INFORMATION(lpBuffer)


def printt(string):
    ts = time.strftime("%H:%M:%S", time.gmtime())
    sys.stdout.write("[*][%s] %s" % (ts, string))


def QueryWorkingSetEx(handle, address, process_is32=False):
    if process_is32:
        lpBuffer = _PSAPI_WORKING_SET_EX_INFORMATION32()
    else:
        lpBuffer = _PSAPI_WORKING_SET_EX_INFORMATION64()
    lpBuffer.VirtualAddress = address
    success = windll.psapi.QueryWorkingSetEx(handle, byref(lpBuffer), sizeof(lpBuffer))
    assert success,  "VirtualQueryEx Failed.\n%s" % (WinError(GetLastError())[1])
    return _PSAPI_WORKING_SET_EX_INFORMATION(lpBuffer), success

