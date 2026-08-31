import copy
import math
import queue
import re
import struct
from typing import Dict, Tuple, List, Callable, Literal, Union

from SALSA.Common.script_string_utils import fix_string_decoding_errors
from SALSA.BaseInstructions.bi_facade import BaseInstLibFacade
from SALSA.Project.project_container import SCTScript, SCTSection, SCTLink, SCTInstruction, SCTParameter
from SALSA.BaseInstructions.bi_container import BaseInstLib, BaseParam
from SALSA.Scripts.scpt_param_codes import SCPTParamCodes
from SALSA.Scripts.default_variables import default_aliases
from SALSA.Common.byte_array_utils import word2SignedInt, is_a_number, pad_hex, applyHexMask
from SALSA.Common.constants import sep, footer_str_group_name, footer_str_id_prefix, override_str
from SALSA.Scripts import scpt_arithmetic_fxns as scpt_arithmetic, scpt_compare_fxns as scpt_compare

ind_entry_len = 0x14
ind_name_offset = 0x4
ind_name_max_len = 0x10

endian: Dict[str, Literal['big', 'little']] = {'gc': 'big', 'dc': 'little'}


class SCTDecoder:
    log_key = 'SCTDecoder'
    _overwriteCheck = False
    _name: str
    _cursor = 0
    _inst_lib = BaseInstLib
    _index: Dict[str, Tuple[int, int]] = {}
    _sctLength = 0
    _sctStart = 0
    _errorCount: int = 0
    _sct: bytearray
    _p_codes: SCPTParamCodes
    _enc = 'shiftjis'
    _str_sect_links: List[SCTLink] = []
    _decoded_str_sect_links = []

    _str_foot_links: List[SCTLink] = []
    _decoded_str_foot_links = []

    _scpt_links: List[SCTLink] = []
    _decoded_scpt_links = []

    _jmp_if_falses: Dict[str, List[str]]
    _decoded_jmp_if_falses: Dict[str, List[str]] = {}

    _switches: Dict[str, List[str]]
    _decoded_switches: Dict[str, List[str]] = {}

    _last_sect_pos: int
    _cur_endian: Literal['big', 'little'] = endian['gc']
    _strings_only: bool = False
    _is_validation: bool = False

    _scpt_arithmetic_fxns: Dict[str, Callable] = {
        '*': scpt_arithmetic.mult,
        '/': scpt_arithmetic.div,
        '+': scpt_arithmetic.add,
        '-': scpt_arithmetic.sub,
        '%': scpt_arithmetic.mod
    }

    _loop_cond_tests: Dict[str, Callable] = {
        '!=': scpt_compare.not_equal,
        '==': scpt_compare.is_equal
    }

    _debug_log: List[str] = []
    _EU_encoding = False

    def _init(self):
        self._str_sect_links = []
        self._str_foot_links = []
        self._scpt_links = []
        self._index = {}
        self._jmp_if_falses = {}
        self._switches = {}
        self._variables = {}
        self._instruction_groups = {}
        self._footer_dialog_locs = []
        self._EU_encoding = False
        self._base_endian: Literal['big', 'little']
        self._other_endian: Literal['big', 'little']
        self._section_groups = {}
        self._section_group_keys = {}
        self._links_to_sections = {}
        self._p_codes = SCPTParamCodes(is_decoder=True)
        self._name = ''
        self._cursor = 0

    @staticmethod
    def generate_index(index_bytearray: bytearray) -> Dict[int, str]:
        i = 0
        first = True
        index = {}
        while i + 0x14 < len(index_bytearray):
            cur_offset = int.from_bytes(index_bytearray[i:i+4], byteorder='big')
            if cur_offset == 0:
                if not first:
                    break
                else:
                    first = False
            first_zero_ind = index_bytearray.index(b'\x00', i+4)
            sect_name_bytes = index_bytearray[i+4:first_zero_ind]
            if len(sect_name_bytes) > 0x10:
                sect_name_bytes = sect_name_bytes[:0x10]
            sect_name = sect_name_bytes.decode(encoding='shiftjis', errors='ignore')
            index[cur_offset] = sect_name
            i += 0x14
        return index

    @classmethod
    def decode_section_from_bytes(cls, name, sect_bytes: bytearray, sect_offset: int, inst_lib: BaseInstLibFacade):
        decoder = cls()
        decoder._init()
        decoder._sct = sect_bytes
        decoder._sctLength = len(sect_bytes)
        decoder._last_sect_pos = len(sect_bytes)
        decoder._base_endian = 'big'
        decoder._other_endian = 'little'
        decoder._cur_endian = decoder._base_endian
        decoder._inst_lib = inst_lib
        section = decoder._decode_sct_section(name, bounds=(0, len(sect_bytes)), additional_offset=sect_offset)
        # May want to generate links to make sure all potential instructions are decoded
        decoder._setup_scpt_links(sect_info={'section': section, 'offset': sect_offset, 'bounds': (sect_offset, len(sect_bytes) + sect_offset)})
        return section

    @classmethod
    def decode_sct_from_file(cls, name, sct, inst_lib: BaseInstLibFacade, status: queue.SimpleQueue = None,
                             strings_only=False, is_validation=False):
        sct_decoder = cls()
        if status is not None:
            status.put({'sub_msg': 'Decoding...'})
        decoded_sct = sct_decoder._decode_sct(name, sct, inst_lib, strings_only=strings_only,
                                              is_validation=is_validation)
        if strings_only:
            decoded_sct = sct_decoder._organize_strings(decoded_sct)
            return decoded_sct
        decoded_sct = sct_decoder._organize_sct(decoded_sct)
        if status is not None:
            status.put({'sub_msg': 'Organizing...'})
        sct_decoder._finalize_sct(decoded_sct)
        if status is not None:
            status.put({'sub_msg': 'Finalizing...'})
        return decoded_sct

    def _decode_sct(self, script_name: str, sct: bytearray, inst_lib: BaseInstLibFacade,
                    strings_only=False, is_validation=False) -> SCTScript:
        print(f'{self.log_key}: Decoding {script_name}')
        self._strings_only = strings_only
        self._is_validation = is_validation
        self._init()
        self._name = script_name
        self._sct = sct
        self._inst_lib = inst_lib

        if (int.from_bytes(self._sct[8:12], byteorder='big') <=
                int.from_bytes(self._sct[8:12], byteorder='little')):
            self._base_endian: Literal['big', 'little'] = 'big'
            self._other_endian: Literal['big', 'little'] = 'little'
            self._cur_endian = self._base_endian
        else:
            self._base_endian: Literal['big', 'little'] = 'little'
            self._other_endian: Literal['big', 'little'] = 'big'
            self._cur_endian = self._base_endian

        header = self._sct[:8]
        ind_entries: int = int.from_bytes(self._sct[8:12], byteorder=self._cur_endian)
        self._sct = self._sct[12:]
        self._index = {}
        used_names = []
        for i in range(ind_entries):
            start = self.getInt(pos=i * ind_entry_len)
            sect_name = self.getString(pos=i * ind_entry_len + ind_name_offset, max_len=ind_name_max_len)
            if sect_name in used_names:
                j = 0
                test_name = f'{sect_name}({j})'
                while test_name in used_names:
                    j += 1
                    test_name = f'{sect_name}({j})'
                sect_name = test_name
            used_names.append(sect_name)
            if i < ind_entries - 1:
                next_start = self.getInt(pos=(i + 1) * ind_entry_len)
            else:
                next_start = -1
                self._last_sect_pos = start
            self._index[sect_name] = (start, next_start)
        index_len = ind_entries * 0x14
        self._sct = self._sct[index_len:]
        self._sctStart = 12 + index_len
        decoded_sct = SCTScript(name=script_name, index=self._index, header=header)

        self._cursor = 0
        self._sctLength = len(self._sct)
        for sect_name, bounds in self._index.items():
            print(f'{self.log_key}: Decoding {sect_name}...', end='\r')
            change_bounds = False
            if bounds[1] == -1:
                start = None
                if len(self._str_foot_links) != 0:
                    min_footer_link = self._sctLength
                    for link in self._str_foot_links:
                        min_footer_link = min(link.target, min_footer_link)
                    start = min_footer_link
                footer_start = self._find_EOF(sect_name, start=start)
                if footer_start is None:
                    footer_start = -1
                    self._cursor = int(bounds[0] / 4)
                    if not self.test_for_string(bounds):
                        raise EOFError(f'{self.log_key}: Final Section was not a String...')
                bounds = (bounds[0], footer_start + 4)
                change_bounds = True

            if change_bounds:
                self._index[sect_name] = bounds

            new_section = self._decode_sct_section(sect_name, bounds)

            decoded_sct.add_section(new_section)

        print(f'{self.log_key}: All sections decoded!!!')

        return decoded_sct

    def _decode_sct_section(self, sect_name, bounds, additional_offset=0) -> Union[SCTSection, None]:

        sct_start_pos = bounds[0]
        length = bounds[1] - bounds[0]
        section = SCTSection()
        section.set_name(sect_name)
        section.set_details(length, sct_start_pos)
        self._cursor = int(sct_start_pos / 4)

        if self.test_for_string(bounds):
            currWord = self.getInt(self._cursor * 4)
            while not currWord == 0x0000001d:
                self._cursor += 1
                currWord = self.getInt(self._cursor * 4)
            self._cursor += 1
            string = self.getString(self._cursor * 4, encoding=self._enc)
            if string == '':
                section.add_error('Length of string == 0')
            if '\\h' not in string:
                string = '\\h()' + string
            section.set_type('String')
            section.set_string(self._cursor * 4, string)
            garbage = self._get_garbage_after_string(bounds=bounds)
            if len(garbage) > 0:
                section.add_garbage('end', garbage)
            if section.string[:2] == '//':
                section.set_type('Label')
            return section

        section = self._create_insts_from_region(bounds, section, 0, additional_offset)

        if len(section.insts) == 1:
            section.set_type('Label')
        else:
            section.set_type('Script')

        return section

    def _create_insts_from_region(self, bounds, section: SCTSection, inst_list_id_start, additional_offset):
        self._cursor = bounds[0] // 4
        self.end = bounds[1]
        inst_list_id = inst_list_id_start
        sect_name = section.name

        while (self._cursor * 4) < bounds[1]:
            self._cur_endian = self._base_endian
            currWord = self.getWord(self._cursor * 4)
            currWord_int = int.from_bytes(currWord, byteorder=self._cur_endian)

            is_inst = 0 <= currWord_int <= 265
            if not is_inst:
                self._cur_endian = self._other_endian
                currWord_int = int.from_bytes(currWord, byteorder=self._cur_endian)
                is_inst = 0 <= currWord_int <= 265

            if is_inst:
                inst_pos = self._cursor * 4

                # Test for the dunder code to try to not go to the next frame after this instruction
                try_no_refresh = False
                if currWord_int == 13:
                    try_no_refresh = True
                    self._cursor += 1
                    currWord_int = self.getInt(self._cursor * 4)

                # Test for the dunder code to delay execution of an instruction
                delay_inst = None
                if currWord_int == 129:
                    delay_inst = self._decode_instruction(currWord_int, inst_pos, [sect_name, inst_list_id])
                    delay_inst.length = self._cursor * 4 - inst_pos
                    currWord_int = self.getInt(self._cursor * 4)

                instResult = self._decode_instruction(currWord_int, inst_pos, [sect_name])
                instResult.absolute_offset += additional_offset

                if instResult.base_id == 9:
                    instResult.label = sect_name

                if try_no_refresh:
                    instResult.skip_refresh = True

                if delay_inst is not None:
                    instResult.delay_param = delay_inst.params[0]
                    numeric = True
                    if instResult.delay_param.arithmetic_value is not None:
                        if not isinstance(instResult.delay_param.arithmetic_value, int):
                            numeric = False
                    elif isinstance(instResult.delay_param.value, dict):
                        numeric = False
                    elif not isinstance(instResult.delay_param.value, int):
                        if not instResult.delay_param.value.isnumeric():
                            numeric = False
                    if not numeric:
                        instResult.add_error(('frame_delay', 'Non-numeric frame delay given'))

                    inst_size = self._cursor * 4 - instResult.absolute_offset
                    if inst_size != (delay_inst.params[1].value + delay_inst.length):
                        garbage_size = ((delay_inst.params[1].value + delay_inst.length) - inst_size) // 4
                        garbage = bytearray(b'')
                        for i in range(self._cursor, self._cursor + garbage_size):
                            garbage += self.getWord(i * 4)
                        instResult.add_error(('Garbage', garbage))
                        instResult.add_error(('frame_delay', 'improper instruction size given, put into garbage'))
                        self._cursor += garbage_size

                section.add_instruction(instResult)

                if instResult.base_id in (24, 25):
                    self._footer_dialog_locs.append([sect_name, instResult.ID])

                if instResult.base_id in [0, 3]:
                    instResult.generate_condition(default_aliases)

                if instResult.base_id == 3:
                    if sect_name not in self._switches.keys():
                        self._switches[sect_name] = []
                    self._switches[sect_name].append(instResult.ID)

                if instResult.base_id == 0:
                    if sect_name not in self._jmp_if_falses.keys():
                        self._jmp_if_falses[sect_name] = []
                    self._jmp_if_falses[sect_name].append(instResult.ID)

                if instResult.base_id == 0x3:

                    # Cleanup garbage entries
                    garbage_entry_found = False
                    remove_params_from = None
                    for i, paramset in enumerate(instResult.l_params):
                        if not garbage_entry_found and paramset[3].value == 0:
                            garbage_entry_found = True
                            remove_params_from = i
                        if garbage_entry_found:
                            cur_link = paramset[3].link
                            self._scpt_links.remove(cur_link)
                            instResult.links_out.remove(cur_link)

                    if garbage_entry_found:
                        self._cursor -= (len(instResult.l_params) - remove_params_from) * 2
                        instResult.l_params = instResult.l_params[:remove_params_from]
                        instResult.params[1].set_value(len(instResult.l_params))

                    # Check for garbage before first entry
                    switch_len = (len(instResult.l_params) * 2) + len(instResult.params)
                    min_case_start_offset = None
                    for i, paramset in enumerate(instResult.l_params):
                        case_start_offset = len(instResult.params) + (i * 2 + 1) + int(paramset[3].value // 4)
                        min_case_start_offset = case_start_offset if min_case_start_offset is None else min(
                            min_case_start_offset, case_start_offset)

                    if switch_len < min_case_start_offset:
                        print(f'switch with garbage here {section.name}')
                        garbage_size = min_case_start_offset - switch_len
                        garbage = self._sct[self._cursor * 4: (self._cursor + garbage_size) * 4]
                        instResult.add_error(('Garbage', garbage))
                        self._cursor += garbage_size
                    elif switch_len > min_case_start_offset:
                        raise IndexError(f'{self.log_key}: Switch incomplete, switch end > min case start')

                if instResult.base_id == 0xc:
                    section.add_error('Garbage: garbage instruction(s) after return')
                    garbage = self._sct[self._cursor * 4:bounds[1]]
                    self._cursor = bounds[1] // 4
                    instResult.add_error(('Garbage', garbage))

                if instResult.base_id == 0xa:
                    end_cursor = bounds[1] // 4
                    if self._cursor < end_cursor:
                        if not (0 <= self.getInt(self._cursor * 4) <= 265):
                            start_cursor = self._cursor
                            while self._cursor * 4 < bounds[1]:
                                next_i_id = self.getInt(self._cursor * 4)
                                if (0 <= next_i_id <= 265) and next_i_id not in [2, 4]:
                                    self._cur_endian = self._base_endian
                                    param1_code = self.getInt(self._cursor * 4 + 4)
                                    self._cur_endian = self._other_endian
                                    param1_code_little = self.getInt(self._cursor * 4 + 4)
                                    self._cur_endian = self._base_endian
                                    if next_i_id in self._inst_lib.lib.p1_scpt:
                                        if (param1_code in self._p_codes.primary_keys
                                            or param1_code in self._p_codes.no_loop) and param1_code != 29:
                                            break
                                        if ((param1_code_little in self._p_codes.primary_keys
                                             or param1_code_little in self._p_codes.no_loop)
                                                and next_i_id != 0 and param1_code_little != 29):
                                            break
                                        param_prefix = param1_code & 0xff000000
                                        param_little_prefix = param1_code_little & 0xff000000
                                        if param_prefix in self._p_codes.cutoff_prefixes:
                                            break
                                        if param_little_prefix in self._p_codes.cutoff_prefixes and next_i_id != 0:
                                            break
                                    elif next_i_id in self._inst_lib.lib.p1_int:
                                        mask = self._inst_lib.lib.insts[next_i_id].params[0].mask
                                        p1 = param1_code
                                        p1_little = param1_code_little
                                        if mask is not None:
                                            p1 = int(applyHexMask(hex(p1), hex(mask)), 16)
                                            p1_little = int(applyHexMask(hex(p1_little), hex(mask)), 16)
                                        if self._inst_lib.lib.insts[next_i_id].params[0].is_signed:
                                            p1 = word2SignedInt(hex(p1))
                                            p1_little = word2SignedInt(hex(p1_little))
                                        if 0 <= self._cursor * 4 + p1 < self._sctLength:
                                            break
                                        if 0 <= self._cursor * 4 + p1_little < self._sctLength:
                                            break
                                    else:
                                        break

                                self._cursor += 1

                            garbage = bytearray(b'')
                            for i in range(start_cursor, self._cursor):
                                garbage += self.getWord(i * 4)
                            instResult.add_error(('Garbage', garbage))

                inst_list_id += 1

            else:
                cur_pos = self._cursor * 4
                absolute_pos = cur_pos + self._sctStart
                param = currWord

                # Since 0x04000000 is reserved for SCPTAnalyze, capture any instructions following this code as SCPT parameters
                if currWord_int == 0x04000000:
                    length = 0
                    while currWord_int != 0x0000001d:
                        length += 1
                        self._cursor += 1
                        currWord = self.getWord(self._cursor * 4)
                        currWord_int = int.from_bytes(currWord, byteorder=self._cur_endian)
                        param += currWord
                    error_str = f'{self.log_key}: Extra SCPT parameter found:\n\tSCPT Position: {cur_pos}'
                    error_str += f'\n\tAbsolute Position: {absolute_pos}'
                    if int(inst_list_id):
                        error_str += f'\n\tPrevious Instruction: {section.get_inst_by_index(inst_list_id).base_id}'
                    else:
                        error_str += f'\n\tUnable to determine Previous Instruction: inst_list_id is {inst_list_id}'
                    error_str += f'\n\tLength: {length}\n\tParam: {param.hex()}'

                else:
                    error_str = f'{self.log_key}: Unknown Instruction Code:\n\tInstruction code: {param.hex()}'
                    error_str += f'\n\tSubscript: {cur_pos}\n\tInstruction List Index: {inst_list_id}'
                    if int(inst_list_id):
                        error_str += f'\n\tPrevious Instruction: {section.get_inst_by_index(inst_list_id - 1).base_id}'
                    else:
                        error_str += f'\n\tUnable to determine Instruction: inst_list_id is {inst_list_id}'
                    error_str += f'\n\tSCPT Position: {cur_pos}\n\tAbsolute Position: {absolute_pos}'

                raise IndexError(error_str)

            if (self._cursor * 4) > bounds[1]:
                error = f'{self.log_key}: Read cursor is past the end of current subscript: {sect_name}'
                raise IndexError(error)

            if self._cursor > self._sctLength:
                raise EOFError(f'{self.log_key}: Read cursor is past the end of the file')

        return section

    def _decode_instruction(self, inst_id, inst_pos, trace):
        cur_inst = SCTInstruction()
        cur_inst.set_inst_id(inst_id)
        cur_inst.set_pos(inst_pos)
        trace.append(cur_inst.ID)
        base_inst = self._inst_lib.get_inst(inst_id=inst_id)
        self._cursor += 1

        # decode parameters
        parameters = copy.deepcopy(base_inst.params)
        loop_parameters = []
        hasLoop = base_inst.loop is not None
        if hasLoop:
            loop_parameters = [parameters[_] for _ in base_inst.loop]
            for p_id in base_inst.loop:
                parameters.pop(p_id)

        cur_param_num = 0
        used_params = []
        for p_id, base_param in parameters.items():
            if p_id != cur_param_num:
                break
            param = self._decode_param(base_param, [*trace, f'{p_id}'])
            if param.link is not None:
                cur_inst.links_out.append(param.link)
            cur_inst.add_parameter(p_id, param)
            used_params.append(p_id)
            cur_param_num += 1

        for p in used_params:
            parameters.pop(p)

        willLoop = True
        if base_inst.loop_cond is not None:
            l_c = base_inst.loop_cond
            if l_c['Location'] == 'External':
                if self._loop_cond_tests[l_c['Test']](cur_inst.params[l_c['Parameter']].value, l_c['Value']):
                    willLoop = False

        if hasLoop and willLoop:
            max_iter = cur_inst.params[base_inst.loop_iter].value
            cur_iter = 0
            done = False
            while not done and cur_iter < max_iter:
                param_group = {}
                for p in loop_parameters:
                    param = self._decode_param(p, [*trace, f'{cur_iter}{sep}{p.param_ID}'])
                    param_group[p.param_ID] = param
                    if param.link is not None:
                        cur_inst.links_out.append(param.link)

                cur_inst.add_loop_parameter(param_group)

                if base_inst.loop_cond is not None:
                    l_c = base_inst.loop_cond
                    if l_c['Location'] == 'Internal':
                        if self._loop_cond_tests[l_c['Test']](param_group[l_c['Parameter']].value, l_c['Value']):
                            break

                cur_iter += 1

        if len(parameters) == 0:
            return cur_inst

        for p_id, base_param in parameters.items():
            if p_id != cur_param_num:
                break
            param = self._decode_param(base_param, [*trace, f'{p_id}'])
            cur_inst.add_parameter(base_param.type, param)
            if param.link is not None:
                cur_inst.links_out.append(param.link)

        return cur_inst

    def _decode_param(self, base_param: BaseParam, trace):
        cur_param = SCTParameter(_id=base_param.param_ID, _type=base_param.type)
        param_type = base_param.type
        if 'scpt' in param_type:
            overrideCompare = [0x7f7fffff]
            if 'int' in param_type:
                overrideCompare.append(0x7fffffff)

            scriptCompare = self.getInt(self._cursor * 4)
            if scriptCompare in overrideCompare:
                cur_param.set_value(override_str, self.getWord(self._cursor * 4))
                self._cursor += 1
                return cur_param

            scptResult = self._SCPT_analyze(cur_param)
            done = False
            if isinstance(scptResult, dict) or isinstance(scptResult, bytearray):
                done = True
            elif isinstance(scptResult, str):
                if scptResult[:2] == '0x':
                    done = True
                elif not is_a_number(scptResult):
                    done = True
                else:
                    scptResult = float(scptResult)
            elif scptResult is None:
                done = True
                cur_param.add_error('Error: No value generated...')

            if not done and 'float' not in param_type:
                signed = scptResult < 0
                scptResult = math.floor(scptResult)
                if 'int' not in param_type:
                    # check that these shouldn't be self.cur_endian or self.base_endian
                    scptResult = bytearray(scptResult.to_bytes(4, 'big', signed=signed)).hex()
                    if 'short' in param_type:
                        scptResult = applyHexMask(scptResult, '0xffff')[2:]
                    elif 'byte' in param_type:
                        scptResult = applyHexMask(scptResult, '0xff')[2:]

                    scptResult = int.from_bytes(bytes=bytearray.fromhex(scptResult), byteorder='big', signed=signed)

            cur_param.set_value(scptResult)

            if cur_param.arithmetic_value is not None:
                arithmetic_result = float(cur_param.arithmetic_value)
                if 'float' not in param_type:
                    signed = arithmetic_result < 0
                    arithmetic_result = math.floor(arithmetic_result)
                    if 'int' not in param_type:
                        # check that these shouldn't be self.cur_endian or self.base_endian
                        arithmetic_result = bytearray(arithmetic_result.to_bytes(4, 'big', signed=signed)).hex()
                        if 'short' in param_type:
                            arithmetic_result = applyHexMask(arithmetic_result, '0xffff')[2:]
                        elif 'byte' in param_type:
                            arithmetic_result = applyHexMask(arithmetic_result, '0xff')[2:]

                        arithmetic_result = int.from_bytes(bytes=bytearray.fromhex(arithmetic_result), byteorder='big',
                                                           signed=signed)
                cur_param.arithmetic_value = arithmetic_result

        elif 'int' in param_type:
            currWord = self.getWord(self._cursor * 4)
            if self._cur_endian == 'little':
                currWord = bytearray(reversed(currWord))

            if 'var' in param_type:
                param_value = self._resolve_SCPT_code_only()
                cur_param.set_value(param_value)
            else:
                raw = currWord
                cur_param.add_raw(raw)
                if base_param.mask is not None:
                    cur_param.type += f'{sep}masked'
                    mask = base_param.mask
                    currWord = bytearray.fromhex(applyHexMask(currWord.hex(), hex(mask))[2:])
                if base_param.is_signed:
                    cur_param.type += f'{sep}signed'
                    cur_value = word2SignedInt(currWord)
                else:
                    # check that these shouldn't be self.cur_endian or self.base_endian
                    cur_value = int.from_bytes(currWord, byteorder='big')
                cur_param.set_value(cur_value)
                if base_param.link_type is not None:
                    link_type = base_param.link_type
                    origin = self._cursor * 4
                    target = self._cursor * 4 + cur_value
                    newLink = SCTLink(type=link_type, script=self._name, origin=origin, target=target, origin_trace=trace)
                    cur_param.link = newLink
                    if link_type == 'String':
                        if target > self._last_sect_pos:
                            self._str_foot_links.append(newLink)
                        else:
                            self._str_sect_links.append(newLink)
                    else:
                        self._scpt_links.append(newLink)
                        if cur_value < 0:
                            self._debug_log.append(f'Negative link: {newLink}')

            self._cursor += 1

        else:
            cur_param.type = 'unknown type'
            cur_param.set_value(f'unknown result for 0x{self.getWord(self._cursor * 4).hex()}')
            self._cursor += 1

        return cur_param

    def _resolve_SCPT_code_only(self):
        currWord = self.getInt(self._cursor * 4)
        action = None
        # Determine the input type from the input_cutoffs table
        for key in self._p_codes.input.keys():
            if currWord >= key:
                action = key
                break

        value = ''
        value += self._p_codes.input[action]
        value += str(currWord & 0x00ffffff)

        return value

    def _SCPT_analyze(self, param: SCTParameter):

        scpt_result = {}
        param_key = f'{param.ID}_S'
        done = False
        roundNum = 0
        currentWord = self.getWord(self._cursor * 4)
        raw = currentWord
        if self._cur_endian == 'little':
            raw = bytearray(reversed(currentWord))

        # First check that the first word is not a special value
        if int.from_bytes(currentWord, byteorder=self._cur_endian) in self._p_codes.no_loop.keys():
            param.add_raw(raw)
            self._cursor += 1
            return self._p_codes.no_loop[int.from_bytes(currentWord, byteorder=self._cur_endian)]

        raw = bytearray(b'')
        # Resolve the SCPT analysis
        result_stack: List[Union[None, int, float]] = [None] * 20
        flag_stack = [0] * 20
        stack_index: int = 0
        max_index = 18
        while not done:
            currentWord = self.getWord(self._cursor * 4)
            temp_raw = currentWord
            if self._cur_endian == 'little':
                temp_raw = bytearray(reversed(currentWord))
            raw.extend(temp_raw)
            currentWord = int.from_bytes(currentWord, byteorder=self._cur_endian)

            cur_result = ''

            # Make sure that the SCPT Stack wouldn't overflow
            if stack_index >= max_index:
                param.add_error(f'SCPT Stack overflow: {currentWord}')
                break

            # Check for end of the SCPT Analysis loop
            if currentWord == 0x0000001d:
                # in me010a.sct, this is the only value given with the last loop parameter, so it may just return zero if the result stack is initialized with zeros
                # If there are no other instructions, This seems to return the last valid value that was given...
                # TODO - This needs to be tested further.
                scpt_result[f'{param_key}{roundNum}'] = 'return values (0x1d)'
                break

            # Test for values between the highest compare code and the prefix for a float
            if 0x04000000 > currentWord > 0x00000016:
                pass

            # Test whether the current word is a primary code
            elif currentWord in self._p_codes.compare.keys():
                currVals = {'1': result_stack[stack_index], '2': result_stack[stack_index + 1]}
                cur_result = {self._p_codes.compare[currentWord]: currVals}
                nones: int = 0
                for v in currVals.values():
                    if v is None:
                        nones += 1
                result_stack[int(stack_index + nones)] = cur_result
                stack_index -= 1
                if currentWord == 0x0000000a:
                    stack_index += 1
            elif currentWord in self._p_codes.arithmetic.keys():
                currVals = {'1': result_stack[stack_index], '2': result_stack[stack_index + 1]}
                inputs = []
                cur_result = {self._p_codes.arithmetic[currentWord]: currVals}
                nones = 0
                for v in currVals.values():
                    if v is None:
                        nones += 1
                    elif isinstance(v, str):
                        v = str(v)
                        if is_a_number(v):
                            inputs.append(v)
                    elif not (isinstance(v, dict) or isinstance(v, bytearray)):
                        inputs.append(v)
                if len(inputs) == 2:
                    result = self._scpt_arithmetic_fxns[self._p_codes.arithmetic[currentWord][4]](inputs[0], inputs[1])
                    param.set_arithmetic_result(result)
                result_stack[stack_index + nones] = cur_result
                stack_index -= 1

            # If not, current action is to input a value
            else:
                action = None

                # Determine the input type from the input_cutoffs table
                for key in self._p_codes.input.keys():
                    if currentWord >= key:
                        action = key
                        break

                # Resolve Input
                if not action == 0x50000000:
                    # Generate values from current word
                    if action is None:
                        cur_result = f'Unable to interpret SCPT instruction: {currentWord}'
                    else:
                        if action == 0x04000000:
                            self._cursor += 1
                            word = self.getWord(self._cursor * 4)
                            if self._cur_endian == 'little':
                                word = list(reversed(word))
                            raw.extend(word)
                            result = self.getFloat(self._cursor * 4)
                        elif action == 0x08000000:
                            obtainedValue = str((currentWord & 0xffff00) >> 8)
                            obtainedValue += f'+{(currentWord & 0xff)}/256'
                            result = self._p_codes.input[action] + f'{obtainedValue}'
                        else:
                            obtainedValue = currentWord & 0x00ffffff
                            result = self._p_codes.input[action] + f'{obtainedValue}'
                        result_stack[stack_index + 2] = result
                        cur_result = result
                        stack_index += 1

                # Go through secondary codes and perform a function
                else:
                    masked_currentWord = currentWord & 0xffffff
                    if masked_currentWord in self._p_codes.secondary.keys():
                        result = self._p_codes.secondary[masked_currentWord]
                        result_stack[(stack_index + 2)] = result
                        cur_result = result
                    else:
                        offset = masked_currentWord
                        cur_result = self._p_codes.input[action] + f'{offset}'
                        result_stack[(stack_index + 2)] = cur_result
                        if masked_currentWord == 0xf:
                            flag_stack[(stack_index + 2)] = 1
                    stack_index += 1

            scpt_result[f'{param_key}{roundNum}'] = {pad_hex(hex(currentWord), 8): cur_result}
            self._cursor += 1
            roundNum += 1
            if self._cursor >= self._sctLength:
                param.add_error('SCPTanalyze did not finish before next sct section')
                break

        if result_stack[2] is None:
            param.add_error('Error: No value generated')

        param.add_raw(raw)
        param.set_param_log(scpt_result)

        self._cursor += 1

        return result_stack[2]

    # ------------------------ #
    # Decoder helper functions #
    # ------------------------ #

    def _find_EOF(self, last_sect, start):
        end_cursor = self._sctLength - 4 if start is None else start
        last_sect_start = self._index[last_sect][0]
        while end_cursor > last_sect_start:
            cur_word = self.getInt(end_cursor)
            if cur_word == 10:
                if word2SignedInt(self.getWord(end_cursor + 4), ) < 0:
                    end_cursor += 4
                    break
            if cur_word == 12:
                break
            end_cursor -= 1
        if end_cursor == last_sect_start:
            print('Unable to find the EOF, likely the last section is a string or label')
            end_cursor = None
        return end_cursor

    def test_for_string(self, bounds):
        cursor = self._cursor
        currWord = self.getInt(cursor * 4)
        if currWord != 0x00000009:
            return False
        while not currWord == 0x0000001d:
            cursor += 1
            currWord = self.getInt(cursor * 4)
        if cursor * 4 == bounds[1]:
            return False
        cursor += 1
        testWord = self.getInt(cursor * 4)
        return testWord < 0 or testWord > 265

    def getInt(self, pos: int):
        return int.from_bytes(bytes=self.getWord(pos), byteorder=self._cur_endian)

    def getFloat(self, pos: int):
        word = self.getWord(pos)
        if self._cur_endian == 'little':
            word = bytearray(reversed(word))
        return struct.unpack('!f', word)[0]

    def getString(self, pos: int, max_len=None, encoding='shiftjis', force_jis=False) -> str:
        size = 0
        while True:
            cur_pos = pos + size
            cur_byte = self._sct[cur_pos]
            if cur_byte == 0x0:
                break
            size += 1
            if max_len is not None:
                if size == max_len:
                    break
        str_bytes = self._sct[pos: pos + size]

        if len(str_bytes) > 3 and not self._EU_encoding:
            if str_bytes[3] == 0xab:
                self._EU_encoding = True

        alt_encoding = 'cp1252'
        if (self._EU_encoding and not str_bytes[:2] == bytearray(b'\x81\x83')
                and not str_bytes[3:5] == bytearray(b'\x81\x73')
                and not str_bytes[3:5] == bytearray(b'\x83\x94')
                and not force_jis):
            encoding, alt_encoding = alt_encoding, encoding

        string = str_bytes.decode(encoding=encoding, errors='backslashreplace')
        if '\\x' in string:
            string = fix_string_decoding_errors(string, encoding)

        if '\\x' in string:
            string = str_bytes.decode(encoding=alt_encoding, errors='backslashreplace')

        return string

    def _get_garbage_after_string(self, bounds):
        offset = 0
        end_of_string = -1
        garbage = bytearray()
        first_zero = True
        expected_zeros = -1
        while (self._cursor * 4) + offset < bounds[1]:
            cur_byte = self._sct[self._cursor * 4 + offset].to_bytes(length=1, byteorder=self._cur_endian)

            if end_of_string > 0:
                garbage.extend(cur_byte)

            elif cur_byte == b'\x00':
                if first_zero:
                    first_zero = False
                    expected_zeros = 1
                    if (offset + expected_zeros) % 4 != 0:
                        expected_zeros += (4 - ((offset + expected_zeros) % 4))
                expected_zeros -= 1
                if expected_zeros == 0:
                    end_of_string = offset

            offset += 1

        return garbage

    def getWord(self, pos) -> bytearray:
        return self._sct[pos: pos + 4]

    # -------------------------- #
    # Organize newly decoded sct #
    # -------------------------- #
    def _organize_strings(self, decoded_sct) -> SCTScript:
        self._reassign_empty_strings(decoded_sct)
        self._create_string_groups(decoded_sct=decoded_sct)
        self._setup_string_links(decoded_sct=decoded_sct)
        self._detect_and_move_footer_dialog(decoded_sct=decoded_sct)
        return decoded_sct

    def _organize_sct(self, decoded_sct) -> SCTScript:
        # Identify empty strings and reassign them as strings
        self._reassign_empty_strings(decoded_sct)

        # Setup links
        print(f'{self.log_key}: Setting Up Links...', end='\r')
        done = False
        while not done:
            done = self._setup_scpt_links(decoded_sct=decoded_sct)

        self._resolve_scpt_links(decoded_sct=decoded_sct)
        self._setup_string_links(decoded_sct=decoded_sct)

        while len(self._switches) + len(self._jmp_if_falses) > 0:
            # Group jump if false commands
            print(f'{self.log_key}: Creating Jump Groups...', end='\r')
            self._group_jump_commands(decoded_sct=decoded_sct)

            if len(self._jmp_if_falses) > 0:
                continue

            # Group switches
            print(f'{self.log_key}: Creating Switch Groups...', end='\r')
            self._group_switches(decoded_sct=decoded_sct)

        # Group Subscripts
        print(f'{self.log_key}: Grouping Sections...', end='\r')
        self._detect_logical_sections(decoded_sct=decoded_sct)
        self._resolve_logical_sections(decoded_sct=decoded_sct)
        self._create_string_groups(decoded_sct=decoded_sct)
        self._group_subscripts(decoded_sct=decoded_sct)
        self._detect_unused_sections(decoded_sct=decoded_sct)
        self._create_sect_group_heirarchies(decoded_sct=decoded_sct)

        # Group Instructions
        self._create_inst_group_heirarchies(decoded_sct=decoded_sct)
        self._set_inst_ungrouped_pos(decoded_sct=decoded_sct)

        # Detect and create footer dialog strings for string editor
        self._detect_and_move_footer_dialog(decoded_sct=decoded_sct)

        return decoded_sct

    def _reassign_empty_strings(self, decoded_sct: SCTScript):
        for sect in decoded_sct.sect_list:
            if decoded_sct.sects[sect].type == 'Label' and len(sect) >= 9:
                sect_number = self._get_longest_numerical_substring(sect)
                if len(sect_number) > 5:
                    decoded_sct.sects[sect].set_type('String')
                    decoded_sct.sects[sect].string = ''

    def _get_longest_numerical_substring(self, string: str):
        cur_digits = ''
        max_digits = ''
        cur_char_ind = 0
        while cur_char_ind < len(string):
            if string[cur_char_ind] in '0123456789':
                cur_digits += string[cur_char_ind]
                if len(cur_digits) >= len(max_digits):
                    max_digits = cur_digits
            else:
                cur_digits = ''
            cur_char_ind += 1
        return max_digits

    def _setup_scpt_links(self, decoded_sct=None, sect_info=None):
        if decoded_sct is None and sect_info is None:
            raise ValueError(f'{self.log_key}: setup scpt links requires either a decoded sct or sect info')
        blank_section = SCTSection()
        self.successful_scpt_links = []
        for link in self._scpt_links:
            target_sect = blank_section if decoded_sct is not None else sect_info['section']
            target_pos = link.target
            if sect_info is None:
                for sect_name, bounds in self._index.items():
                    if bounds[0] <= link.target < bounds[1]:
                        target_sect = decoded_sct.sects[sect_name]
                        break
            elif (target_pos - sect_info['offset']) < sect_info['bounds'][0] \
                    or (target_pos - sect_info['offset']) >= sect_info['bounds'][1]:
                continue
            else:
                target_pos -= sect_info['offset']
            target_inst_ind = 0
            inst = None
            while target_inst_ind < len(target_sect.insts):
                inst = target_sect.get_inst_by_index(target_inst_ind)
                if inst.absolute_offset == target_pos:
                    break

                if target_inst_ind == len(target_sect.insts) - 1:
                    inst_end = target_sect.absolute_offset + target_sect.length
                else:
                    inst_end = target_sect.get_inst_by_index(target_inst_ind + 1).absolute_offset

                if inst.absolute_offset < target_pos < inst_end:
                    internal = True
                    new_error = None
                    for i, e in enumerate(inst.errors):
                        if e[0] != 'Garbage':
                            continue
                        internal = False
                        first_start = inst_end - len(e[1])
                        if target_pos < first_start:
                            internal = True
                            break
                        new_error = e[1][:target_pos - first_start]
                        break

                    if internal:
                        print(f'{self.log_key}: link position is internal to an instruction '
                              f'at {target_sect.name}:{target_inst_ind}: \n\t{link}')
                        decoded_sct.errors.append(
                            f'link position is internal to an instruction at {target_sect.name}:{target_inst_ind}: \n\t{link}')
                        break

                    inst_num_before = len(target_sect.inst_list)
                    self._decode_garbage(sect=target_sect, inst=inst, start=target_pos, end=inst_end,
                                         delete_if_insts_created=True)

                    if new_error is not None and inst_num_before < len(target_sect.inst_list):
                        if len(new_error) > 0:
                            inst.errors.append(('Garbage', new_error))

                    print(
                        f'{self.log_key}: New instructions decoded at {target_sect.name}:{target_inst_ind + 1}. Restarting Link setup...',
                        end='\r')
                    return False

                target_inst_ind += 1

            link.target_trace = [target_sect.name, inst.ID]
            self.successful_scpt_links.append(link)

        self._decoded_scpt_links = [*self._decoded_scpt_links, *self._scpt_links]
        self._scpt_links = []
        return True

    def _resolve_scpt_links(self, decoded_sct: SCTScript, links=None):
        if links is None:
            links = self.successful_scpt_links
        for link in links:

            # add the origin of a link to its target
            target_inst = decoded_sct.sects[link.target_trace[0]].insts[link.target_trace[1]]
            target_inst.links_in.append(link)
            origin_inst = decoded_sct.sects[link.origin_trace[0]].insts[link.origin_trace[1]]
            if link not in origin_inst.links_out:
                origin_inst.links_out.append(link)

            # add to dict with places that subscripts are called from
            if link.origin_trace[0] != link.target_trace[0]:
                tr_sect = link.origin_trace[0]
                target_tr_sect = link.target_trace[0]
                if target_tr_sect not in self._links_to_sections.keys():
                    self._links_to_sections[target_tr_sect] = []
                if tr_sect not in self._links_to_sections[target_tr_sect]:
                    self._links_to_sections[target_tr_sect].append(tr_sect)

    def _setup_string_links(self, decoded_sct):
        # setup string links
        sect_keys = list(decoded_sct.sects.keys())
        for link in self._str_sect_links:
            target_sct_str = ''
            target_sct_id = 0
            while target_sct_id < len(decoded_sct.sects):
                section = decoded_sct.sects[sect_keys[target_sct_id]]
                if link.target == section.absolute_offset:
                    target_sct_str = section.name
                    break
                if target_sct_id == len(decoded_sct.sects) - 1:
                    sect_end = section.absolute_offset + section.length
                else:
                    sect_end = decoded_sct.sects[sect_keys[target_sct_id + 1]].absolute_offset
                if section.absolute_offset < link.target < sect_end:
                    print(f'link position is incorrect: \n\t{link}')
                    break
                target_sct_id += 1

            # Fix for strings with '\c' on the end to move the text box fade to a parameter instead of hardcoded
            # in the string.
            origin_inst = decoded_sct.sects[link.origin_trace[0]].insts[link.origin_trace[1]]
            param: SCTParameter = origin_inst.params[int(link.origin_trace[2])]
            param.linked_string = target_sct_str
            if '\\c' in decoded_sct.sects[target_sct_str].string and not self._is_validation:
                if origin_inst.base_id == 144:
                    if origin_inst.params[1].value != 'decimal: 1+0/256':
                        origin_inst.params[1].set_value('decimal: 1+0/256')
                if origin_inst.base_id == 155:
                    if origin_inst.params[2].value != 'decimal: 1+0/256':
                        origin_inst.params[2].set_value('decimal: 1+0/256')

        # setup footer links
        for link in self._str_foot_links:
            foot_str = self.getString(link.target, force_jis=True)
            origin_inst = decoded_sct.sects[link.origin_trace[0]].insts[link.origin_trace[1]]
            if origin_inst.base_id in (24, 25, 144, 155) and '\\h' not in foot_str:
                foot_str = '\\h()' + foot_str
            param: SCTParameter = origin_inst.params[int(link.origin_trace[2])]
            param.linked_string = foot_str
            param.link.type = 'Footer'

        self._decoded_str_sect_links = [*self._decoded_str_sect_links, *self._str_sect_links]
        self._str_sect_links = []
        self._decoded_str_foot_links = [*self._decoded_str_foot_links, *self._str_foot_links]
        self._str_foot_links = []

    def _detect_logical_sections(self, decoded_sct):
        in_sect_group = False
        sect_group_key = None
        for name, sect in decoded_sct.sects.items():
            # Create or add to a logical section group if needed
            if not in_sect_group:
                if sect.type == 'Script':
                    if sect.get_inst_by_index(-1).base_id != 12:
                        in_sect_group = True
                        sect_group_key = f'logical{sep}{sect.name}'
                        self._section_groups[sect_group_key] = [sect.name]
                        self._section_group_keys[sect.name] = sect_group_key
            else:
                if sect.type in ('Script', 'Label'):
                    self._section_groups[sect_group_key].append(sect.name)
                    self._section_group_keys[sect.name] = sect_group_key
                    last_inst = sect.get_inst_by_index(-1)
                    if last_inst.base_id == 12:
                        in_sect_group = False
                        sect_group_key = None
                else:
                    last_sect = decoded_sct.sects[self._section_groups[sect_group_key][-1]]
                    if last_sect.insts[last_sect.inst_list[-1]].base_id == 9:
                        self._section_groups[sect_group_key].pop(-1)
                    in_sect_group = False
                    sect_group_key = None

    def _group_jump_commands(self, decoded_sct):
        processed_jmps = {}
        for sect_name, jump_list in self._jmp_if_falses.items():
            processed_jmps[sect_name] = []
            for jmp_id in jump_list:
                processed_jmps[sect_name].append(jmp_id)
                jmp_inst: SCTInstruction = decoded_sct.sects[sect_name].insts[jmp_id]
                jmp_link = jmp_inst.links_out[0]
                jmp_tgt_sect_name = jmp_link.target_trace[0]
                jmp_tgt_sect = decoded_sct.sects[jmp_tgt_sect_name]
                jmp_tgt_uuid = jmp_link.target_trace[1]

                tgt_prev_sect_name, prev_inst = self.get_prev_inst(sct=decoded_sct, cur_sect=jmp_tgt_sect_name,
                                                                   cur_inst=jmp_tgt_uuid)

                if not prev_inst.base_id == 10:
                    prev_inst_id = prev_inst.ID

                    garbage = None
                    for e in prev_inst.errors:
                        if e[0] == 'Garbage':
                            garbage = e

                    if garbage is not None:
                        prev_inst = self._check_for_goto_in_garbage(decoded_sct, tgt_prev_sect_name, prev_inst)

                    if prev_inst is None or prev_inst_id == prev_inst.ID:
                        print(
                            f'SCPT Decoder: Decode Jumps: No goto inst found in garbage: {sect_name}{sep}{jmp_id}, goto inst added')

                goto_inst = prev_inst

                jmp_inst.my_goto_uuids.append(goto_inst.ID)
                goto_inst.my_master_uuids.append(jmp_inst.ID)

                if sect_name not in self._instruction_groups:
                    self._instruction_groups[sect_name] = {}

                # Check for a loop and add to loops if true
                if goto_inst.params[0].value < 0:
                    group_key = f'{jmp_id}{sep}while'
                    self._instruction_groups[sect_name][group_key] = (f'{sect_name}{sep}{jmp_id}',
                                                                      f'{tgt_prev_sect_name}{sep}{goto_inst.ID}')
                    decoded_sct.sects[sect_name].jump_loops.append(group_key)
                    continue

                # else, get the end of the false option
                goto_tgt_sect = goto_inst.links_out[0].target_trace[0]
                goto_tgt_uuid = goto_inst.links_out[0].target_trace[1]

                group_key = f'{jmp_id}{sep}if'
                self._instruction_groups[sect_name][group_key] = (f'{sect_name}{sep}{jmp_id}',
                                                                  f'{tgt_prev_sect_name}{sep}{goto_inst.ID}')

                if jmp_tgt_uuid == goto_tgt_uuid or jmp_tgt_sect_name != goto_tgt_sect:
                    continue

                goto_tgt_prev_sect_name, goto_tgt_prev_inst = self.get_prev_inst(sct=decoded_sct,
                                                                                 cur_sect=goto_tgt_sect,
                                                                                 cur_inst=goto_tgt_uuid)

                goto_tgt_prev_uuid = goto_tgt_prev_inst.ID

                group_key = f'{jmp_id}{sep}else'
                self._instruction_groups[sect_name][group_key] = (
                    f'{jmp_tgt_sect_name}{sep}{jmp_tgt_uuid}', f'{goto_tgt_prev_sect_name}{sep}{goto_tgt_prev_uuid}')

        for sect_name, jmp_list in processed_jmps.items():
            if sect_name not in self._decoded_jmp_if_falses:
                self._decoded_jmp_if_falses[sect_name] = []

            for jmp in jmp_list:
                self._decoded_jmp_if_falses[sect_name].append(jmp)
                self._jmp_if_falses[sect_name].remove(jmp)

        empties = [s for s in self._jmp_if_falses if len(self._jmp_if_falses[s]) == 0]
        for s in empties:
            self._jmp_if_falses.pop(s)

    def _group_switches(self, decoded_sct):
        completed_switches = {}

        # Go through each section and check for switches
        for sect_name, switch_list in self._switches.items():
            completed_switches[sect_name] = []

            # Go through each switch in each section
            for s_id in switch_list:
                # print(f'SCPT Decoder: Switch Groups: Grouping Switch: {sect_name}:{s_id}')
                switch_inst: SCTInstruction = decoded_sct.sects[sect_name].insts[s_id]

                # Get all switch start positions
                num_entries = switch_inst.params[1].value
                switch_entry_cases = []
                switch_entry_links = {}
                for entry_id in range(num_entries):
                    cur_case = switch_inst.l_params[entry_id][2].value
                    switch_entry_cases.append(cur_case)
                    switch_entry_links[cur_case] = switch_inst.l_params[entry_id][3].link

                # Range of each switch entry should be from the start to the next start-1
                # For all entries except the last one, create instruction groups for switch entries
                # Then if the goto at the end of an entry is negative, it should be a loop, so add that entry to loops
                # If at least one is positive, record the goto, and the max goto should be the end of the last one
                if sect_name not in self._instruction_groups:
                    self._instruction_groups[sect_name] = {}
                prev_case = None
                prev_start_sect = None
                prev_start_uuid = None
                last_goto_tgt = None
                for i, case in enumerate(switch_entry_cases):
                    cur_ttrace = switch_entry_links[case].target_trace
                    if prev_case is not None:

                        prev_sect, prev_inst = self.get_prev_inst(decoded_sct, cur_ttrace[0], cur_ttrace[1])

                        alt_goto = self._check_for_goto_in_garbage(decoded_sct=decoded_sct, sect_name=prev_sect,
                                                                   prev_inst=prev_inst)
                        if alt_goto is not None:
                            prev_inst = alt_goto

                        group_key = f'{s_id}{sep}{prev_case}'
                        self._instruction_groups[sect_name][group_key] = (
                            f'{prev_start_sect}{sep}{prev_start_uuid}', f'{prev_sect}{sep}{prev_inst.ID}')

                        if prev_inst.base_id == 10:

                            if prev_inst.ID not in switch_inst.my_goto_uuids:
                                switch_inst.my_goto_uuids.append(prev_inst.ID)
                            if switch_inst.ID not in prev_inst.my_master_uuids:
                                prev_inst.my_master_uuids.append(switch_inst.ID)

                            if prev_inst.params[0].value < 0:
                                decoded_sct.sects[sect_name].jump_loops.append(group_key)

                            if last_goto_tgt is None:
                                last_goto_tgt = prev_inst.params[0].link.target
                            elif last_goto_tgt <= prev_inst.params[0].link.target:
                                last_goto_tgt = prev_inst.params[0].link.target

                    prev_case = case
                    prev_start_sect = cur_ttrace[0]
                    prev_start_uuid = cur_ttrace[1]

                # Find end of last swtich entry
                last_case = switch_entry_cases[-1]

                # if an end for the last section was found previously, set it
                if last_goto_tgt is not None:
                    goto_tgt_sect = self.find_sect_by_pos(decoded_sct, last_goto_tgt)
                    goto_tgt_inst = self._find_inst_by_pos(decoded_sct, last_goto_tgt)
                    goto_sect, goto_inst = self.get_prev_inst(decoded_sct, goto_tgt_sect.name, goto_tgt_inst.ID)
                    group_key = f'{s_id}{sep}{last_case}'
                    self._instruction_groups[sect_name][group_key] = (
                        f'{prev_start_sect}{sep}{prev_start_uuid}', f'{goto_tgt_sect}{sep}{goto_inst.ID}')
                    switch_inst.my_goto_uuids.append(goto_inst.ID)
                    goto_inst.my_master_uuids.append(switch_inst.ID)

                    completed_switches[sect_name].append(s_id)
                    continue

                cur_sect_name, cur_inst = self.get_next_inst(decoded_sct, prev_start_sect, prev_start_uuid)
                gotos_ignored = 0
                goto_found = False
                # else search for a goto that matches with the switch.
                # May be error-prone if there are random gotos inside a switch entry
                while not goto_found:

                    if cur_inst.base_id == 12:
                        print(f'SCPT Decoder: Switch Groups: End of final switch entry not found: {sect_name}:{s_id}')
                        break
                    elif cur_inst.base_id == 0:
                        cur_uuid = cur_inst.links_out[0].target_trace[1]
                    elif cur_inst.base_id == 3:
                        new_last_case_target = cur_inst.l_params[-1][3].link.target
                        new_last_case_start = self._find_inst_by_pos(decoded_sct, new_last_case_target)
                        cur_uuid = new_last_case_start.ID
                        gotos_ignored += 1
                    elif cur_inst.base_id == 10:
                        if gotos_ignored > 0:
                            gotos_ignored -= 1
                        else:
                            goto_found = True
                            break
                    cur_sect_name, cur_inst = self.get_next_inst(decoded_sct, cur_sect_name, cur_inst.ID)

                group_key = f'{s_id}{sep}{last_case}'
                self._instruction_groups[sect_name][group_key] = (
                    f'{prev_start_sect}{sep}{prev_start_uuid}', f'{cur_sect_name}{sep}{cur_inst.ID}')
                if goto_found:
                    goto_inst = decoded_sct.sects[cur_sect_name].insts[cur_inst.ID]
                    switch_inst.my_goto_uuids.append(goto_inst.ID)
                    goto_inst.my_master_uuids.append(switch_inst.ID)

                completed_switches[sect_name].append(s_id)

        self._remove_completed_switches(completed_switches)

    @staticmethod
    def _create_string_groups(decoded_sct):
        cur_group_name = ''
        cur_group = []
        has_header = False
        i = 0
        new_section_order = []
        new_sections = {}
        for name in decoded_sct.sect_list:

            section = decoded_sct.sects[name]
            if section.type == 'Label':
                new_section_order.append(name)
                has_header = True
                if cur_group_name != '':
                    decoded_sct.string_groups[cur_group_name] = cur_group
                    cur_group = []
                else:
                    if len(cur_group) > 0:
                        strs = '\n'.join(cur_group)
                        print(f'Strings present before string groups will not be assigned a group: {strs}')
                cur_group_name = section.name
            elif section.type == 'String':
                if not has_header:
                    if len(cur_group) > 0:
                        decoded_sct.string_groups[cur_group_name] = cur_group
                        cur_group = []
                    cur_group_name = f'Untitled({i})'
                    new_section_order.append(cur_group_name)
                    new_section = SCTSection()
                    new_section.set_name(cur_group_name)
                    new_section.set_details(0, section.absolute_offset)
                    new_sections[cur_group_name] = new_section
                    new_sections[cur_group_name].set_type('Label')
                    has_header = True
                    i += 1
                    print(
                        f'This string is not located contiguously under a label: {section.name}, new group created: {cur_group_name}')
                cur_group.append(section.name)
                decoded_sct.string_locations[section.name] = cur_group_name
                decoded_sct.strings[section.name] = section.string
                new_section_order.append(name)
            elif section.type == 'Script' or section.type == '':
                has_header = False
                new_section_order.append(name)

        if has_header:
            decoded_sct.string_groups[cur_group_name] = cur_group

        for name in list(decoded_sct.string_groups.keys()):
            if len(decoded_sct.string_groups[name]) == 0:
                decoded_sct.string_groups.pop(name)

        new_sections_dict = {}
        new_sections_ungrouped = []
        for name in new_section_order:
            if name in decoded_sct.sects.keys():
                if decoded_sct.sects[name].type == 'String':
                    decoded_sct.string_garbage[name] = decoded_sct.sects[name].garbage
                    continue
                new_sections_dict[name] = decoded_sct.sects[name]
                new_sections_ungrouped.append(name)
                continue
            new_sections_dict[name] = new_sections[name]
            new_sections_ungrouped.append(name)
        decoded_sct.sects = new_sections_dict
        decoded_sct.sect_list = new_sections_ungrouped

    def _resolve_logical_sections(self, decoded_sct: SCTScript):
        # Prune logical script groups with a single entry
        groups_to_remove = []
        for key, group in self._section_groups.items():
            if len(group) < 2:
                groups_to_remove.append(key)
        for group in groups_to_remove:
            self._section_groups.pop(group)

        inst_groups_modded = {k: v for k, v in self._instruction_groups.items() if
                              k not in decoded_sct.folded_sects.keys()}

        for name, group in self._section_groups.items():
            if 'logical' not in name:
                continue
            new_name = group[0]

            # Setup combined logical section
            new_section: SCTSection = decoded_sct.sects[group[0]]
            new_section.name = new_name
            new_section.is_compound = True
            new_section.internal_sections_inst[group[0]] = 0
            new_section.internal_sections_curs[group[0]] = 0
            for sect_name in group[1:]:
                sect_to_add: SCTSection = decoded_sct.sects[sect_name]
                new_section.internal_sections_inst[sect_name] = len(new_section.insts)
                new_section.internal_sections_curs[
                    sect_name] = sect_to_add.absolute_offset - new_section.absolute_offset
                new_section.insts = {**new_section.insts, **sect_to_add.insts}
                new_section.inst_list.extend(sect_to_add.inst_list)
                for key, value in sect_to_add.insts_used.items():
                    if key not in new_section.insts_used:
                        new_section.insts_used[key] = 0
                    new_section.insts_used[key] += value
                new_section.length += sect_to_add.length
                new_section.jump_loops.extend(sect_to_add.jump_loops)
                new_section.strings = {**new_section.strings, **sect_to_add.strings}
                new_section.errors.extend(sect_to_add.errors)
                new_section.garbage = {**new_section.garbage, **sect_to_add.garbage}
                new_section.inst_errors.extend(sect_to_add.inst_errors)

            keys_before = []
            keys_after = []
            hit_name = False
            for key in decoded_sct.sect_list:
                if key in group:
                    hit_name = True
                    continue
                if not hit_name:
                    keys_before.append(key)
                else:
                    keys_after.append(key)

            sections = {k: decoded_sct.sects[k] for k in keys_before}
            sections[new_name] = new_section
            sections = {**sections, **{k: decoded_sct.sects[k] for k in keys_after}}
            decoded_sct.sects = sections

            decoded_sct.sect_list = [*keys_before, new_name, *keys_after]

            for s in group:
                decoded_sct.folded_sects[s] = new_name

            inst_groups_modded[new_name] = {}
            for old_sect_name in group:
                if old_sect_name in self._instruction_groups.keys():
                    for group_key, t in self._instruction_groups[old_sect_name].items():
                        t1 = f'{decoded_sct.folded_sects[old_sect_name]}{sep}{t[0].split(sep)[1]}'
                        t2 = f'{decoded_sct.folded_sects[old_sect_name]}{sep}{t[1].split(sep)[1]}'
                        inst_groups_modded[new_name][group_key] = (t1, t2)

        self._instruction_groups = inst_groups_modded

        self._section_groups = {}
        self._section_group_keys = {}
        for link in self._decoded_scpt_links:
            self._update_link_traces(link, decoded_sct)

        for link in self._decoded_str_sect_links:
            self._update_link_traces(link, decoded_sct)

        for link in self._decoded_str_foot_links:
            self._update_link_traces(link, decoded_sct)

    def _detect_unused_sections(self, decoded_sct: SCTScript):
        external_section_pattern = '^M[0-9]{4}$'
        called_sections = list(self._links_to_sections.keys())
        special_sections = ['init', 'loop']
        string_group_keys = list(decoded_sct.string_groups.keys())
        for sect_name in decoded_sct.sects.keys():
            if sect_name in [*called_sections, *special_sections, *string_group_keys]:
                continue
            if re.search(external_section_pattern, sect_name):
                continue
            decoded_sct.unused_sections.append(sect_name)

    def _group_subscripts(self, decoded_sct):
        sections = decoded_sct.sects
        script_filename = decoded_sct.name.split('.')[0]
        cur_group = ''
        suffix = ''
        in_group = False
        for sect_name, section in sections.items():

            if in_group and suffix in sect_name.lower():
                self._section_groups[cur_group].append(sect_name)
            else:
                in_group = False

            if script_filename in sect_name and not in_group:
                in_group = True
                cur_group = f'{sect_name}{sep}group'
                suffix = sect_name[len(script_filename):].lower()
                if ')' in suffix:
                    suffix = suffix[:-3]
                self._section_groups[cur_group] = []

            if section.type == 'Label':
                if suffix not in sect_name:
                    in_group = False

        empty_sections = []
        for name, group in self._section_groups.items():
            if len(group) == 0:
                empty_sections.append(name)

        for name in empty_sections:
            self._section_groups.pop(name)

    def _create_sect_group_heirarchies(self, decoded_sct):

        # Create grouped section heirarchy
        groups = self._section_groups

        if len(groups) > 0:
            # sort groups by size with the smallest first
            groups = {k: groups[k] for k in sorted(groups.keys(), key=lambda k: len(groups[k]))}

            new_groups = self._nest_groups(groups)
            new_groups = self._complete_heirarchy(list(decoded_sct.sects.keys()), new_groups)
            decoded_sct.sect_tree = new_groups
        else:
            decoded_sct.sect_tree = copy.deepcopy(decoded_sct.sect_list)

    def _create_inst_group_heirarchies(self, decoded_sct):

        # Create grouped instruction heirarchies
        for section in decoded_sct.sects.values():
            if section.name not in self._instruction_groups:
                section.inst_tree = copy.deepcopy(section.inst_list)
                continue

            new_groups = InstNester.nest(section.inst_list, self._instruction_groups[section.name], section.insts)

            section.inst_tree = new_groups

    def _complete_heirarchy(self, full_list, groups):
        heirarchy = []
        skips = []
        for entry in full_list:
            if entry in skips:
                continue
            matching_groups = []
            for key in groups.keys():
                if str(entry) == key.split(sep)[0]:
                    matching_groups += [{key: groups[key]}]
            if len(matching_groups) == 0:
                heirarchy.append(entry)
                continue
            heirarchy = heirarchy + matching_groups
            skips.extend(self._get_heirarchy_skips(matching_groups))
        return heirarchy

    # ----------------------------- #
    # Organize SCT helper functions #
    # ----------------------------- #

    def _decode_garbage(self, sect, inst, start=None, end=None, delete_if_insts_created=False):
        garbage = None
        garbage_index = -1
        for i, e in enumerate(inst.errors):
            if e[0] == 'Garbage':
                garbage = e
                garbage_index = i

        if garbage is None:
            return

        inst_ind = sect.inst_list.index(inst.ID)

        if end is None:
            if inst_ind == len(sect.insts) - 1:
                end = sect.absolute_offset + sect.length
            else:
                end = sect.get_inst_by_index(inst_ind + 1).absolute_offset

        if start is None:
            start = end - len(garbage[1])

        suffix_insts = sect.inst_list[inst_ind + 1:]
        sect.inst_list = sect.inst_list[:inst_ind + 1]

        bounds = (start, end)
        pref_len = len(sect.inst_list)
        sect = self._create_insts_from_region(bounds, sect, inst_ind + 1, 0)
        insts_made = len(sect.inst_list) - pref_len
        sect.inst_list += suffix_insts

        if delete_if_insts_created and insts_made > 0:
            inst.errors.pop(garbage_index)

    def _check_for_goto_in_garbage(self, decoded_sct, sect_name, prev_inst):
        sect = decoded_sct.sects[sect_name]
        prev_inst_ind = sect.inst_list.index(prev_inst.ID)

        next_inst_id = None if prev_inst_ind + 1 == len(sect.inst_list) else sect.inst_list[prev_inst_ind + 1]

        self._decode_garbage(sect=sect, inst=prev_inst, delete_if_insts_created=True)

        done = False
        while not done:
            done = self._setup_scpt_links(decoded_sct=decoded_sct)

        self._resolve_scpt_links(decoded_sct=decoded_sct)
        self._setup_string_links(decoded_sct=decoded_sct)

        goto_out = None
        if next_inst_id is None:
            insts_created = [i for i in sect.inst_list[prev_inst_ind + 1:]]
        else:
            insts_created = [i for i in sect.inst_list[prev_inst_ind + 1: sect.inst_list.index(next_inst_id)]]

        if len(insts_created) == 0:
            return goto_out

        for inst_id in insts_created:
            if sect.insts[inst_id].base_id == 10:
                goto_out = sect.insts[inst_id]
                break

        return goto_out

    def _create_new_goto(self, decoded_sct, sect_name, after_inst):
        next_sect, next_inst_id = self.get_next_inst(decoded_sct, sect_name, after_inst.ID)
        base_goto = self._inst_lib.get_inst(10)
        new_goto = SCTInstruction()
        new_goto.set_inst_id(10)
        goto_param = SCTParameter(0, base_goto.params[0].type)
        goto_param.set_value(4)
        goto_link = SCTLink(base_goto.link_type, script=self._name, origin=-1, origin_trace=[sect_name, new_goto.ID, 0],
                            target=-1, target_trace=[next_sect, next_inst_id])
        goto_param.link = goto_link
        new_goto.add_parameter(0, goto_param)
        new_goto.add_error(('Validation', 'Do not include in validation SCT exports'))

        if next_sect != sect_name:
            decoded_sct.sects[sect_name].inst_list.append(new_goto.ID)
        else:
            decoded_sct.sects[sect_name].inst_list.insert(decoded_sct.sects[sect_name].inst_list.index(next_inst_id),
                                                          new_goto.ID)

        decoded_sct.sects[sect_name].insts[new_goto.ID] = new_goto

        return new_goto

    @staticmethod
    def _update_link_traces(link, decoded_sct):
        if link.origin_trace[0] in decoded_sct.folded_sects:
            old_sect_name = link.origin_trace[0]
            section = decoded_sct.sects[decoded_sct.folded_sects[old_sect_name]]
            link.origin_trace[0] = section.name

        if link.target_trace is not None:
            if link.target_trace[0] in decoded_sct.folded_sects:
                old_sect_name = link.target_trace[0]
                section = decoded_sct.sects[decoded_sct.folded_sects[old_sect_name]]
                link.target_trace[0] = section.name

    def _nest_groups(self, groups, is_sections=True):
        # replace entries in parent groups with child groups
        new_groups = copy.deepcopy(groups)
        for group_name, group in groups.items():
            group = new_groups[group_name]
            insert_keys = []

            for n_group_name, n_group in new_groups.items():
                if group_name == n_group_name:
                    continue
                all_present = True
                for entry in group:
                    if not self._is_in(entry, n_group):
                        all_present = False
                        break
                if all_present:
                    insert_keys.append(n_group_name)

            for key in insert_keys:
                insert_index = new_groups[key].index(new_groups[group_name][0])
                for entry in group:
                    new_groups[key].remove(entry)
                new_groups[key].insert(insert_index, group_name)

        done = False
        all_group_keys = list(new_groups.keys())
        cur_group_ind = 0
        while not done:
            cur_group_key = all_group_keys[cur_group_ind]
            new_groups = self._replace_group_name_with_group(new_groups, cur_group_key)

            # goto next key
            cur_group_ind += 1
            if cur_group_ind >= len(all_group_keys):
                break

            # if next key is not in new groups, find next key which is present
            while all_group_keys[cur_group_ind] not in new_groups.keys():
                cur_group_ind += 1
                if cur_group_ind >= len(all_group_keys):
                    done = True
                    break

        return new_groups

    def _replace_group_name_with_group(self, all_groups, cur_group_key):
        cur_group = all_groups[cur_group_key]
        new_groups = copy.deepcopy(all_groups)
        for i, entry in enumerate(cur_group):
            if entry == cur_group_key:
                continue
            if isinstance(entry, str) and entry in new_groups.keys():
                new_groups = self._replace_group_name_with_group(new_groups, entry)
                new_groups[cur_group_key].insert(i, {entry: new_groups[entry]})
                new_groups[cur_group_key].pop(i + 1)
                new_groups.pop(entry)

        return new_groups

    def _is_in(self, entry, group):
        if isinstance(entry, dict):
            for entry_key, entry_value in entry.items():
                key_present = False
                value_present = False
                for obj in group:
                    if not isinstance(obj, dict):
                        continue
                    if entry_key in obj.keys():
                        key_present = True
                        if self._is_in(entry_value, obj[entry_key]):
                            value_present = True
                            break
                        return False
                if not key_present or not value_present:
                    return False
            return True

        if isinstance(entry, list):
            if not isinstance(group, list):
                return False
            for entry_item in entry:
                if not self._is_in(entry_item, group):
                    return False
            return True

        return entry in group

    def _get_heirarchy_skips(self, group, skips=None):
        if skips is None:
            skips = []
        if isinstance(group, dict):
            for entry in group.values():
                self._get_heirarchy_skips(entry, skips)
        elif isinstance(group, list):
            for entry in group:
                self._get_heirarchy_skips(entry, skips)
        else:
            skips.append(group)
        return skips

    @staticmethod
    def _find_sect_in_group(group_starts, cur_id):
        group = None
        for g, p in reversed(group_starts.items()):
            if cur_id > p:
                group = g
                break
        return group

    def _replace_pos_with_ids(self, group, instruction_ids_ungrouped):
        if isinstance(group, dict):
            new_groups = {}
            for key, value in group.items():
                if sep in key:
                    key_split = key.split(sep)
                    key = f'{instruction_ids_ungrouped[int(key_split[0])]}{sep}' + sep.join(key_split[1:])
                new_groups[key] = self._replace_pos_with_ids(value, instruction_ids_ungrouped)
        elif isinstance(group, list):
            new_groups = []
            for entry in group:
                new_groups.append(self._replace_pos_with_ids(entry, instruction_ids_ungrouped))
        else:
            new_groups = instruction_ids_ungrouped[group]
        return new_groups

    @staticmethod
    def _set_inst_ungrouped_pos(decoded_sct):
        for section in decoded_sct.sects.values():
            for i, key in enumerate(section.inst_list):
                section.insts[key].ungrouped_position = i

    def _detect_and_move_footer_dialog(self, decoded_sct):
        if len(self._footer_dialog_locs) == 0:
            return

        footer_dialog_strings = []
        for trace in self._footer_dialog_locs:
            sect = trace[0]
            if trace[0] not in decoded_sct.sects:
                sect = self._find_section(trace[0], decoded_sct)
            inst = decoded_sct.sects[sect].insts[trace[1]]
            string = inst.params[int(inst.links_out[0].origin_trace[2])].linked_string
            if '\\e' in string or '\\c' in string:
                footer_dialog_strings.append((trace, string))

        if len(footer_dialog_strings) == 0:
            return

        decoded_sct.string_groups[footer_str_group_name] = []
        for i, item in enumerate(footer_dialog_strings):
            foot_id = f'{footer_str_id_prefix}{i:03d}'
            decoded_sct.strings[foot_id] = item[1]
            decoded_sct.string_groups[footer_str_group_name].append(foot_id)
            decoded_sct.string_locations[foot_id] = footer_str_group_name
            sect = item[0][0]
            if sect not in decoded_sct.sects:
                sect = self._find_section(sect, decoded_sct)
            inst = decoded_sct.sects[sect].insts[item[0][1]]
            inst.params[int(inst.links_out[0].origin_trace[2])].linked_string = foot_id

    @staticmethod
    def _find_section(tgt_name, decoded_sct) -> str:
        for name, sect in decoded_sct.sects.items():
            if len(sect.internal_sections_inst.keys()) > 0:
                for int_sect in sect.internal_sections_inst.keys():
                    if int_sect == tgt_name:
                        return name

    def find_sect_by_pos(self, decoded_sct, pos) -> Union[None, SCTSection]:
        for sect_name, bounds in self._index.items():
            if bounds[0] <= pos < bounds[1]:
                return decoded_sct.sects[sect_name]
        return None

    def _find_inst_by_pos(self, sct: SCTScript, pos: int) -> Union[None, SCTInstruction]:
        sect = self.find_sect_by_pos(sct, pos)
        inst_list = sect.inst_list
        found = False
        pos_range = self._index[sect.name]
        cur_ind = int(len(inst_list) * ((pos - pos_range[0]) / (pos_range[1] - pos_range[0])))
        while -1 < cur_ind < len(inst_list):
            cur_inst = sect.insts[inst_list[cur_ind]]
            if cur_inst.absolute_offset == pos:
                found = True
                break
            if cur_inst.absolute_offset < pos < sect.insts[inst_list[cur_ind + 1]].absolute_offset:
                print(
                    f'SCPT Decoder: Find Inst: Target pos in middle of inst {sect.name}:{cur_inst.ID} - {pos}')
                break
            if cur_inst.absolute_offset < pos:
                cur_ind += 1
            else:
                cur_ind -= 1

        if not found:
            print(
                f'SCPT Decoder: Find Inst: Entry not found: - {pos}')
            return None

        return sect.insts[sect.inst_list[cur_ind]]

    @staticmethod
    def get_prev_inst(sct: SCTScript, cur_sect: str, cur_inst: str) -> (str, SCTInstruction):
        cur_sect = sct.sects[cur_sect]
        if cur_sect.inst_list.index(cur_inst) == 0:
            prev_sect_name = sct.sect_list[sct.sect_list.index(cur_sect.name) - 1]
            prev_id = len(sct.sects[prev_sect_name].inst_list) - 1
        else:
            prev_sect_name = cur_sect.name
            prev_id = cur_sect.inst_list.index(cur_inst) - 1

        prev_inst = sct.sects[prev_sect_name].get_inst_by_index(prev_id)
        return prev_sect_name, prev_inst

    @staticmethod
    def get_next_inst(sct: SCTScript, cur_sect: str, cur_inst: str) -> (str, SCTInstruction):
        cur_sect = sct.sects[cur_sect]
        if cur_sect.inst_list.index(cur_inst) == len(cur_sect.inst_list) - 1:
            next_sect_name = sct.sect_list[sct.sect_list.index(cur_sect) - 1]
            next_id = len(sct.sects[next_sect_name].inst_list) + 1
        else:
            next_sect_name = cur_sect.name
            next_id = cur_sect.inst_list.index(cur_inst) + 1

        next_inst = sct.sects[next_sect_name].get_inst_by_index(next_id)
        return next_sect_name, next_inst

    def _remove_completed_switches(self, completed_switches):
        for sect, switch_list in completed_switches.items():
            for s_id in switch_list:
                self._switches[sect].remove(s_id)

        self._switches = {k: v for k, v in self._switches.items() if len(v) > 0}

    # ---------------------------------------------------------- #
    # Add useful fields for script manipulation and organization #
    # ---------------------------------------------------------- #

    def _finalize_sct(self, decoded_sct):

        self._setup_variables(decoded_sct)

        print(f'{self.log_key}: {decoded_sct.name} finished')

    def _setup_variables(self, sct):
        for s_name, section in sct.sects.items():
            for inst_key, inst in section.insts.items():
                for pID, param in inst.params.items():
                    if isinstance(param.value, dict):
                        var_list = self._extract_variables(param.value)
                        self._add_variable_locations(var_list, (s_name, inst_key, f'{pID}'))
                for loop_id, loop in enumerate(inst.l_params):
                    for pID, param in loop.items():
                        if isinstance(param.value, dict):
                            var_list = self._extract_variables(param.value)
                            self._add_variable_locations(var_list, (s_name, inst_key, f'{loop_id}{sep}{pID}'))

        sct.variables = self._variables

    def _extract_variables(self, cur_element, var_list=None):
        if var_list is None:
            var_list = []
        if isinstance(cur_element, dict):
            for value in cur_element.values():
                var_list = self._extract_variables(value, var_list=var_list)
        elif isinstance(cur_element, str):
            if 'Var:' in cur_element:
                var_list.append(cur_element)
        return var_list

    def _add_variable_locations(self, var_list, trace):
        for var in var_list:
            keys = var.split(': ')
            if keys[0] not in self._variables.keys():
                self._variables[keys[0]] = {}
            key_1 = int(keys[1])
            if key_1 not in self._variables[keys[0]]:
                self._variables[keys[0]][key_1] = {'alias': '', 'usage': []}
            self._variables[keys[0]][key_1]['usage'].append(trace)


class InstNester:

    def __init__(self, inst_list, groups, insts):
        self.inst_list = inst_list
        self.inst_groups = groups
        self.insts: Dict[str, SCTInstruction] = insts
        self.cur_ind = 0
        self.cur_uuid = ''
        self.group_uuids = [k.split(sep)[0] for k in self.inst_groups.keys()]

    @classmethod
    def nest(cls, inst_list, inst_groups, insts):
        nester = cls(inst_list, inst_groups, insts)
        return copy.deepcopy(nester._nest())

    def _nest(self, end=None):
        cur_level = []
        while self.cur_ind < len(self.inst_list):
            self.cur_uuid = self.inst_list[self.cur_ind]
            # check for end of group
            if self.cur_uuid not in self.group_uuids:
                cur_level.append(self.cur_uuid)
                self.cur_ind += 1
            else:
                cur_level += self._add_group()

            if end is not None:
                if self.cur_uuid in end:
                    return cur_level

        return cur_level

    def _add_group(self):
        cur_elements = {g: v for g, v in self.inst_groups.items() if self.inst_list[self.cur_ind] in g}
        cur_inst_uuid = self.inst_list[self.cur_ind]
        self.cur_ind += 1

        if list(cur_elements.keys())[0].split(sep)[1] in ['if', 'else', 'while']:

            cur_level = []
            while len(cur_elements) > 0:
                next_key = None
                for s in ['if', 'else', 'while']:
                    if f'{cur_inst_uuid}{sep}{s}' in cur_elements:
                        next_key = f'{cur_inst_uuid}{sep}{s}'
                        break
                next_element = cur_elements.pop(next_key)
                next_grouping = {next_key: self._nest(end=next_element[1].split(sep)[1])}
                cur_level.append(next_grouping)

        else:
            case_level = {}

            switch = self.insts[cur_inst_uuid]
            cases = [p[2].value for p in switch.l_params]
            cur_case_ind = 0
            while cur_case_ind < len(cases):
                cur_case = cases[cur_case_ind]
                ele_key = f'{cur_inst_uuid}{sep}{cur_case}'

                if cur_case_ind + 1 < len(cases):
                    next_case = cases[cur_case_ind + 1]
                    if cur_elements[ele_key][0] == cur_elements[f'{cur_inst_uuid}{sep}{next_case}'][0]:
                        case_level[str(cur_case)] = []
                        cur_case_ind += 1
                        continue

                case_level[str(cur_case)] = self._nest(end=cur_elements[ele_key][1].split(sep)[1])
                cur_case_ind += 1

            sw_key = f'{cur_inst_uuid}{sep}switch'
            cur_level = [{sw_key: case_level}]

        return cur_level
