import copy
import re
from typing import Union, Literal

from SALSA.Common.constants import alt_sep, footer_str_group_name
from SALSA.Common.script_string_utils import fix_string_encoding_errors
from SALSA.BaseInstructions.bi_facade import BaseInstLibFacade
from SALSA.Common.byte_array_utils import float2Hex
from SALSA.Project.project_container import SCTScript, SCTParameter
from SALSA.Scripts.scpt_param_codes import SCPTParamCodes
from SALSA.Scripts.scpt_compare_fxns import is_equal, not_equal


class SCTEncoder:
    log_key = 'SCTEncoder'
    skip_refresh = 13
    _placeholder = bytearray(b'\x7f\x7f\xff\xff')
    endian_struct_format = {'big': '>', 'little': '<'}
    _header_offset_length = 0x4
    _header_name_length = 0x10
    _default_header_starts = {'little': bytearray(b'\x07\xd2\x06\x00\x0e\x00\x00\x00'),
                              'big': bytearray(b'\x07\xd2\x00\x06\x00\x0e\x00\x00')}

    use_garbage: bool
    combine_footer_links: bool
    add_spurious_refresh: bool

    param_tests = {'==': is_equal, '!=': not_equal}

    def __init__(self, script: SCTScript, base_insts: BaseInstLibFacade, update_inst_pos=True,
                 endian: Literal['little', 'big'] = 'big', validation=False, eu_validation=False):
        self.sct_body = bytearray()

        # for decoder, encoder validation only
        self.validation = validation
        self._EU_validation = eu_validation

        self._default_header_start = self._default_header_starts[endian]

        self._str_label = b'\x00\x00\x00\x09\x04\x00\x00\x00\x3f\x80\x00\x00\x00\x00\x00\x1d'
        self._str_label = b'\x09\x00\x00\x00\x00\x00\x00\x04\x00\x00\x80\x3f\x1d\x00\x00\x00' if endian == 'little' else self._str_label
        if endian == 'little' and re.search('5[0-9]{2}A', script.name) and self.validation:
            self._str_label = b'\x00\x00\x00\x09\x08\x00\x01\x00\x00\x00\x00\x1d'

        self._additions = {}
        if self.validation and endian == 'big':
            if '099' in script.name:
                self.sct_body = bytearray(
                    b'\x4F\x00\x00\x00\x00\x00\x00\x04\x00\x00\x66\x43\x1D\x00\x00\x00\x00\x00\x00'
                    b'\x04\x00\x00\x34\x43\x1D\x00\x00\x00\x00\x00\x00\x04\x00\x00\x16\x43\x1D\x00'
                    b'\x00\x00\x00\x00\x00\x04\x00\x00\x80\x3F\x1D\x00\x00\x00\x00\x00\x00\x04\x00'
                    b'\x00\x00\x00\x1D\x00\x00\x00\x00\x00\x00\x04\x00\xC0\x0F\xC5\x1D\x00\x00\x00'
                    b'\x00\x00\x00\x04\x00\x00\x80\x3F\x1D\x00\x00\x00')
            if '241a' in script.name:
                self.sct_body = bytearray(b'\x0b\x00\x00\x00\xc4\xe2\x00\x00') if not self._EU_validation \
                    else bytearray(b'\x0b\x00\x00\x00\x08\x63\x01\x00')
            if '513a' in script.name or '576a' in script.name:
                self.sct_body = bytearray(
                    b'\x0D\x00\x00\x00\x0D\x00\x00\x00\x0D\x00\x00\x00\x0D\x00\x00\x00\x0D\x00\x00'
                    b'\x00\x0D\x00\x00\x00\x0D\x00\x00\x00\x0D\x00\x00\x00\x0D\x00\x00\x00\x0D\x00'
                    b'\x00\x00\x0D\x00\x00\x00\x0D\x00\x00\x00\x0D\x00\x00\x00\x0D\x00\x00\x00\x0D'
                    b'\x00\x00\x00\x0D\x00\x00\x00\x0D\x00\x00\x00\x0D\x00\x00\x00\x0D\x00\x00\x00')
            additions = {
                'M04523-177-1': b'\x50\x00\x00\x01',
                'M04524-177-1': b'\x50\x00\x00\x01',
                'M04525-177-1': b'\x50\x00\x00\x01',
                'K08cut8-136-1': b'\x50\x00\x00\x01',
                'l01cut6-134-0': b'\x50\x00\x00\x01',
                'l01cut7-134-0': b'\x50\x00\x00\x01',
                'doc00_init01-136-1': b'\x50\x00\x00\x01',
                'hama_big01-136-1': b'\x50\x00\x00\x01',
                'hama_change01-136-1': b'\x50\x00\x00\x01',
                'me260aq02-136-1': {'ind': [1], 'value': b'\x50\x00\x00\x01'},
                'me260aq03-136-1': {'ind': [1], 'value': b'\x50\x00\x00\x01'},
                'q04a_start-136-1': {'ind': [1], 'value': b'\x50\x00\x00\x01'},
                'me260aq05-136-1': {'ind': [1], 'value': b'\x50\x00\x00\x01'},
                'camb02-232-3': {'ind': [8], 'value': b'\x04\x00\x00\x00\x42\x80\x00\x00'},
                'camb02-224-13': {'ind': [10], 'value': b'\x04\x00\x00\x00\x42\xF9\x00\x00'},
                '_EV_VER2-50-0': {'ind': [2], 'value': b'\x50\x00\x00\x01'}
            }

            eu_additions = {
                'camb02-232-3': {'ind': [8], 'value': b'\x04\x00\x00\x00\x42\x80\x00\x00'},
                'camb02-224-13': {'ind': [10], 'value': b'\x04\x00\x00\x00\x42\xF9\x00\x00'},
            }

            self._additions = additions if not self._EU_validation else eu_additions

        elif self.validation and endian == 'little':
            if '099' in script.name:
                self.sct_body = bytearray(
                    b'\x4F\x00\x00\x00\x00\x00\x00\x04\x00\x00\x66\x43\x1D\x00\x00\x00\x00\x00\x00'
                    b'\x04\x00\x00\x34\x43\x1D\x00\x00\x00\x00\x00\x00\x04\x00\x00\x16\x43\x1D\x00'
                    b'\x00\x00\x00\x00\x00\x04\x00\x00\x80\x3F\x1D\x00\x00\x00\x00\x00\x00\x04\x00'
                    b'\x00\x00\x00\x1D\x00\x00\x00\x00\x00\x00\x04\x00\xC0\x0F\xC5\x1D\x00\x00\x00'
                    b'\x00\x00\x00\x04\x00\x00\x80\x3F\x1D\x00\x00\x00')
            if '241A' in script.name:
                self.sct_body = bytearray(b'\x0b\x00\x00\x00\x30\xe2\x00\x00') if not self._EU_validation \
                    else bytearray(b'\x0b\x00\x00\x00\x08\x63\x01\x00')
            if '513A' in script.name or '576A' in script.name:
                self.sct_body = bytearray(
                    b'\x0D\x00\x00\x00\x0D\x00\x00\x00\x0D\x00\x00\x00\x0D\x00\x00\x00\x0D\x00\x00'
                    b'\x00\x0D\x00\x00\x00\x0D\x00\x00\x00\x0D\x00\x00\x00\x0D\x00\x00\x00\x0D\x00'
                    b'\x00\x00\x0D\x00\x00\x00\x0D\x00\x00\x00\x0D\x00\x00\x00\x0D\x00\x00\x00\x0D'
                    b'\x00\x00\x00\x0D\x00\x00\x00\x0D\x00\x00\x00\x0D\x00\x00\x00\x0D\x00\x00\x00')
            additions = {
                'M04523-177-1': b'\x50\x00\x00\x01',
                'M04524-177-1': b'\x50\x00\x00\x01',
                'M04525-177-1': b'\x50\x00\x00\x01',
                'K08cut8-136-1': b'\x50\x00\x00\x01',
                'l01cut6-134-0': b'\x50\x00\x00\x01',
                'l01cut7-134-0': b'\x50\x00\x00\x01',
                'doc00_init01-136-1': b'\x50\x00\x00\x01',
                'hama_big01-136-1': b'\x50\x00\x00\x01',
                'hama_change01-136-1': b'\x50\x00\x00\x01',
                'me260aq02-136-1': {'ind': [1], 'value': b'\x50\x00\x00\x01'},
                'me260aq03-136-1': {'ind': [1], 'value': b'\x50\x00\x00\x01'},
                'q04a_start-136-1': {'ind': [1], 'value': b'\x50\x00\x00\x01'},
                'me260aq05-136-1': {'ind': [1], 'value': b'\x50\x00\x00\x01'},
                'camb02-232-3': {'ind': [8], 'value': b'\x00\x40\x00\x08'},
                'camb02-224-13': {'ind': [10], 'value': b'\x04\x00\x00\x00\x42\xF9\x00\x00'},
                '_EV_VER2-50-0': {'ind': [2], 'value': b'\x50\x00\x00\x01'}
            }

            eu_additions = {
                'camb02-232-3': {'ind': [8], 'value': b'\x04\x00\x00\x00\x42\x80\x00\x00'},
                'camb02-224-13': {'ind': [10], 'value': b'\x04\x00\x00\x00\x42\xF9\x00\x00'},
            }

            self._additions = additions if not self._EU_validation else eu_additions

        self.update_inst_pos = update_inst_pos
        self._EU_encoding = self.detect_encoding(script)

        self.script = script
        self.bi = base_insts
        self.sct = bytearray()
        self.sct_head = copy.deepcopy(self._default_header_start)
        self.sct_foot = bytearray()
        self.header_dict = {}
        self.footer_dict = {}
        self.added_string_groups = []
        self.endian = endian
        self.param_code = SCPTParamCodes(is_decoder=False)

        # Links will start off being a dictionary with origin_offset: (string or section or jmp_target)
        # They will then be iterated upon to replace the value with the location of the target
        # Finally, they will be used to replace the original
        self.sct_links = {}
        self.string_links = {}
        self.footer_links = {}
        self.inst_positions = {}
        self.errors = []

    @classmethod
    def encode_sct_file_from_project_script(cls, project_script: SCTScript, base_insts: BaseInstLibFacade, endian,
                                            use_garbage=True, combine_footer_links=False, add_spurious_refresh=True,
                                            update_inst_pos=True, validation=False, eu_validation=False, separate_index=False):
        print(f'{cls.log_key}: encoding {project_script.name}')
        encoder = cls(script=project_script, base_insts=base_insts, update_inst_pos=update_inst_pos,
                      validation=validation, eu_validation=eu_validation, endian=endian)
        encoder.encode_sct_file(use_garbage=use_garbage, combine_footer_links=combine_footer_links,
                                add_spurious_refresh=add_spurious_refresh)
        print(f'{cls.log_key}: finished encoding for {project_script.name}')
        if separate_index:
            return encoder.sct_head[0xc:], encoder.sct_body + encoder.sct_foot
        return encoder.sct

    def encode_sct_file(self, use_garbage=True, combine_footer_links=False, add_spurious_refresh=False):
        self.use_garbage = use_garbage
        self.combine_footer_links = combine_footer_links
        self.add_spurious_refresh = add_spurious_refresh

        pops = []
        for i, error in enumerate(self.script.errors):
            if 'Encoding' in error:
                pops.append(i)

        for i in reversed(pops):
            self.script.errors.pop(i)

        # encode sections in order
        for name in self.script.sect_list:
            section = self.script.sects[name]
            self._encode_section(name=name, section=section)

            # if string group header is added, add strings below it
            if name in self.script.string_groups.keys():
                self.added_string_groups.append(name)
                self._add_strings(name)

        # Resolve through jmp_links
        for link_offset, (jmp_to, trace) in self.sct_links.items():
            if jmp_to[1] not in self.inst_positions:
                self.script.errors.append(
                    ('Encoding', 'Link', f'No target inst {jmp_to[0]}-{jmp_to[1]}', alt_sep.join(trace)))
                print(f'No target inst{jmp_to[1]}')
                continue
            jmp_to_pos = self.inst_positions[jmp_to[1]]
            jmp_offset = self._make_word(i=(jmp_to_pos - link_offset), signed=True)
            self._sct_body_insert_hex(location=link_offset, value=jmp_offset, validation=self._placeholder)

        # Resolve string links
        for link_offset, (section, trace) in self.string_links.items():
            if 'FOOTER' in section:
                continue
            if section not in self.header_dict:
                self.script.errors.append(('Encoding', 'String', f'No string {section}', alt_sep.join(trace)))
                print(f'No string {section}')
                continue
            str_pos = self.header_dict[section]
            str_offset = str_pos - link_offset
            str_offset_word = self._make_word(i=str_offset, signed=True)
            self._sct_body_insert_hex(location=link_offset, value=str_offset_word, validation=self._placeholder)

        # Add footer entries in order by links while resolving links
        has_footer_dialogue = footer_str_group_name in self.script.string_groups
        added_footer_entries = {}
        for offset, (string, trace) in self.footer_links.items():
            if has_footer_dialogue:
                if string in self.script.string_groups[footer_str_group_name]:
                    string = self.script.strings[string]

            if combine_footer_links and string in added_footer_entries:
                str_pos = added_footer_entries[string]
            else:
                str_pos = len(self.sct_body) + len(self.sct_foot)
                added_footer_entries[string] = str_pos
            str_offset = str_pos - offset
            str_offset_word = self._make_word(i=str_offset)
            self.sct_foot.extend(self._encode_string(string=string, align=False))
            self._sct_body_insert_hex(location=offset, value=str_offset_word, validation=self._placeholder)

        # Build header
        header_len = self._make_word(len(self.header_dict))
        self.sct_head.extend(header_len)
        for name, offset in self.header_dict.items():
            self.sct_head.extend(self._make_word(offset))
            self.sct_head.extend(self._encode_string(string=name, align=False, size=self._header_name_length))

        # Combine file portions together
        self.sct = self.sct_head + self.sct_body + self.sct_foot

        return self.sct

    def _add_strings(self, string_group_name):
        string_group = self.script.string_groups[string_group_name]
        for name in string_group:
            self.header_dict[name] = len(self.sct_body)
            self.sct_body.extend(self._str_label)
            string = self.script.strings[name]

            # Add String using encode string
            self.sct_body.extend(self._encode_string(string=string, align=True))

            if name in self.script.string_garbage:
                garbage = self.script.string_garbage[name]
                if 'end' in garbage:
                    self.sct_body.extend(garbage['end'])

    def _encode_section(self, name, section):
        # If the inst is a label, just add the entry to the header and add a string label
        sect_name = name
        section.absolute_offset = len(self.sct_body)
        for inst_id in section.inst_list:
            inst = section.insts[inst_id]
            if inst.base_id == 9:
                sect_name = inst.label
                self.header_dict[sect_name] = len(self.sct_body)
            self._encode_instruction(inst, a_trace=[sect_name], e_trace=[name])

        # add garbage at end of section if needed
        if self.use_garbage:
            if 'end' in section.garbage.keys():
                self.sct_body.extend(section.garbage['end'])

    def _encode_instruction(self, instruction, a_trace, e_trace):
        self.inst_positions[instruction.ID] = len(self.sct_body)
        if self.update_inst_pos:
            instruction.absolute_offset = len(self.sct_body)

        if not instruction.encode_inst:
            return

        base_inst = self.bi.get_inst(instruction.base_id)
        a_trace.append(str(instruction.base_id))
        e_trace.append(str(instruction.ID))

        # add 13 code if needed or wanted
        if instruction.skip_refresh:
            if self.add_spurious_refresh or (not base_inst.no_new_frame and not base_inst.forced_new_frame):
                self.sct_body.extend(self._make_word(self.skip_refresh))

        delay_pos = None
        inst_pos = -1
        if instruction.delay_param is not None or instruction.delay_param == 0:
            self.sct_body.extend(self._make_word(129))
            base_param = self.bi.get_inst(129).params[0]
            self._encode_param(param=instruction.delay_param, base_param=base_param,
                               a_trace=[*a_trace, str(-1)], e_trace=[*e_trace, 'Delay'])
            delay_pos = len(self.sct_body)
            self.sct_body.extend(b'0000')
            inst_pos = len(self.sct_body)

        # add instruction code to sct_body
        self.sct_body.extend(self._make_word(instruction.base_id))

        loop_iter_param_location = None
        loop_iter_param_value = None
        # cycle through and add parameters
        for p_id in base_inst.params_before:
            if base_inst.loop_iter is not None:
                if p_id == base_inst.loop_iter:
                    loop_iter_param_location = len(self.sct_body)
                    loop_iter_param_value = instruction.params[p_id].value
            self._encode_param(param=instruction.params[p_id], base_param=base_inst.params[p_id],
                               a_trace=[*a_trace, str(p_id)], e_trace=[*e_trace, str(p_id)])

        do_loop = True
        has_loop_cond = False
        # Check for an external loop bypass
        if base_inst.loop_cond is not None:
            if base_inst.loop_cond['Location'] == 'External':
                value1 = instruction.params[base_inst.loop_cond['Parameter']].value
                if not isinstance(value1, int):
                    value1 = int(value1)
                value2 = base_inst.loop_cond['Value']
                if not isinstance(value2, int):
                    value2 = int(value2)
                test = base_inst.loop_cond['Test']
                do_loop = not self.param_tests[test](value1, value2)
            else:
                has_loop_cond = True

        break_loops = False
        loop_iters_performed = 0
        if do_loop:
            for i, loop in enumerate(instruction.l_params):
                for p_id, param in loop.items():
                    self._encode_param(param=loop[p_id], base_param=base_inst.params[p_id],
                                       a_trace=[*a_trace, f'{i}|{p_id}'], e_trace=[*e_trace, f'{i}|{p_id}'])

                    # Check internal loop conditions
                    if not has_loop_cond:
                        continue

                    if p_id == base_inst.loop_cond['Parameter']:
                        value1 = param.value

                        if isinstance(value1, dict) and param.arithmetic_value is not None:
                            value1 = param.arithmetic_value

                        if not isinstance(value1, int):
                            value1 = self.convert_param_to_int(value1)

                        value2 = base_inst.loop_cond['Value']

                        if not isinstance(value2, int):
                            value2 = self.convert_param_to_int(value2)

                        test = base_inst.loop_cond['Test']
                        if self.param_tests[test](value1, value2):
                            break_loops = True
                            break

                loop_iters_performed += 1
                if break_loops:
                    break

        if loop_iter_param_value is not None:
            if loop_iters_performed != loop_iter_param_value:
                self._sct_body_insert_hex(location=loop_iter_param_location,
                                          value=self._make_word(loop_iters_performed))

        for p_id in base_inst.params_after:
            self._encode_param(param=instruction.params[p_id], base_param=base_inst.params[p_id],
                               a_trace=[*a_trace, str(p_id)], e_trace=[*e_trace, str(p_id)])

        # add garbage at then of instruction if needed
        garbage_index = None
        for i, e in enumerate(instruction.errors):
            if e[0] == 'Garbage':
                garbage_index = i
                break

        if garbage_index is not None and self.use_garbage:
            garbage = instruction.errors[garbage_index][1]
            self.sct_body.extend(garbage)

        if delay_pos is not None:
            inst_len = len(self.sct_body) - inst_pos
            self._sct_body_insert_hex(delay_pos, bytearray(self._make_word(inst_len)))

    def _encode_param(self, param: SCTParameter, base_param, a_trace, e_trace):
        # if needed, setup link and use 0x7fffffff as placeholder
        if 'footer' in base_param.type or 'string' in base_param.type or \
                'jump' in base_param.type or 'subscript' in base_param.type:
            if 'footer' in base_param.type or 'string' in base_param.type:
                if param.linked_string is None or param.linked_string == ('',):
                    self.script.errors.append(('Encoding', 'Parameter', 'No string assigned', alt_sep.join(e_trace)))
                    return
                if 'string' in base_param.type:
                    self.string_links[len(self.sct_body)] = (param.linked_string, e_trace)
                if 'footer' in base_param.type:
                    self.footer_links[len(self.sct_body)] = (param.linked_string, e_trace)

            elif 'jump' in base_param.type or 'subscript' in base_param.type:
                if param.link is None:
                    self.script.errors.append(('Encoding', 'Parameter', 'Jump not setup', alt_sep.join(e_trace)))
                    return
                if param.link.target_trace is None:
                    self.script.errors.append(('Encoding', 'Parameter', 'Jump not setup', alt_sep.join(e_trace)))
                    return
                self.sct_links[len(self.sct_body)] = (param.link.target_trace, e_trace)
            self.sct_body.extend(self._placeholder)
            return

        # generate parameter with encode scpt if needed
        if 'scpt' in base_param.type:
            if param.override is not None:
                value = param.override
            else:
                if param.value is None and 'skip' not in base_param.type:
                    self.script.errors.append(('Encoding', 'Parameter', 'Value is None', alt_sep.join(e_trace)))
                    return
                no_loop = False
                if isinstance(param.value, str):
                    if param.value in self.param_code.no_loop.keys():
                        no_loop = True

                if no_loop:
                    value = self._make_word(self.param_code.no_loop[param.value])
                    if self.validation:
                        self._check_additions(a_trace, value)
                else:
                    value = self._encode_scpt_param(param=param.value)
                    if self.validation:
                        self._check_additions(a_trace, value)
                    value.extend(self._make_word(self.param_code.stop_code))
        else:
            if param.value is None:
                self.script.errors.append(('Encoding', 'Parameter', 'Value is None', alt_sep.join(e_trace)))
                return
            if 'var' in base_param.type:
                value = self._encode_scpt_param(param.value)
            else:
                value = self._make_word(param.value, signed=base_param.is_signed)
            if self.validation:
                self._check_additions(a_trace, value)

        # add parameter
        self.sct_body.extend(value)

    def _encode_scpt_param(self, param):
        param_bytes = bytearray()

        if param is None:
            return param_bytes

        # if parameter is an integer, it should be stored as a float within the script files, so convert to float
        if isinstance(param, int):
            param = float(param)

        # If the parameter is a float, enter the float into script file
        if isinstance(param, float):
            param_bytes.extend(self._make_word(self.param_code.input['float: ']))
            form = f'{self.endian_struct_format[self.endian]}f'
            param_bytes.extend(float2Hex(param, form))

        # if the parameter is complex, go through the requisite parameter dictionary encoding values then keys
        elif isinstance(param, dict):
            for key, value in param.items():
                param_bytes.extend(self._encode_scpt_param(value))
                if key in self.param_code.compare:
                    param_bytes.extend(self._make_word(self.param_code.compare[key]))
                if key in self.param_code.arithmetic:
                    param_bytes.extend(self._make_word(self.param_code.arithmetic[key]))

        # If the parameter is a string, it is likely located in input headers or secondary locations
        elif isinstance(param, str):
            if param in self.param_code.secondary:
                cutoff = self.param_code.input['IntVar: ']
                value = self.param_code.secondary[param]
            else:
                param_parts = param.split(' ')
                cutoff = self.param_code.input[param_parts[0] + ' ']
                if cutoff == self.param_code.input['decimal: ']:
                    d_parts = param_parts[1].split('/')[0].split('+')
                    value = int(d_parts[0]) << 8
                    value = value | int(d_parts[1])
                else:
                    value = int(param_parts[1])
            param_bytes.extend(self._make_word(cutoff | value))

        else:
            print(f'unknown code?')

        return param_bytes

    def _check_additions(self, trace, ba: bytearray):
        key = '-'.join(trace)
        if key in self._additions:
            addition = self._additions[key]
            if isinstance(addition, dict):
                will_add = False
                if 0 in addition['ind']:
                    will_add = True
                addition['ind'] = [_ - 1 for _ in addition['ind']]
                if not will_add:
                    return
                addition = addition['value']
            ba.extend(addition)

    def _make_word(self, i: int, signed=None):
        signed = False if signed is None else signed
        return bytearray(i.to_bytes(length=4, byteorder=self.endian, signed=signed))

    def _encode_string(self, string, encoding='shiftjis', align=True, size=-1):
        if '«' in string:
            self._EU_encoding = True

        alt_encoding = 'cp1252'
        if self._EU_encoding and '＜' not in string and '《' not in string:
            encoding, alt_encoding = alt_encoding, encoding

        str_bytes = bytearray(string.encode(encoding=encoding, errors='backslashreplace'))
        if self._EU_encoding and encoding == 'shiftjis':
            str_bytes = fix_string_encoding_errors(str_bytes, encoding=encoding)
        if b'\\x' in str_bytes or b'\\u' in str_bytes:
            str_bytes = bytearray(string.encode(encoding=alt_encoding, errors='backslashreplace'))

        if self._EU_validation:
            str_bytes = str_bytes.replace(b'\x20', b'\x81')

        if size > 0:
            if len(str_bytes) > size:
                return str_bytes[:size]
            extra_bytes = b'\x00' * (size - len(str_bytes))
            str_bytes.extend(extra_bytes)
            return str_bytes

        str_bytes.extend(b'\x00')
        if not align:
            return str_bytes

        extra_bytes = b''
        if len(str_bytes) % 4 != 0:
            extra_bytes = b'\x00' * (4 - len(str_bytes) % 4)
        str_bytes.extend(extra_bytes)
        return str_bytes

    def _sct_body_insert_hex(self, location: int, value: bytearray, validation: Union[None, bytearray] = None):
        if validation is None:
            valid = True
        else:
            valid = validation.hex() == self.sct_body[location: location + len(validation)].hex()

        if not valid:
            print(
                f'{self.log_key}: Validation failed for hex replacement in sct_body: {self.sct_body[location: location + len(validation) - 1].hex()} != {validation.hex()}')
            return

        self.sct_body = self.sct_body[:location] + value + self.sct_body[location + len(value):]

    @staticmethod
    def detect_encoding(script: SCTScript):
        for string in script.strings.values():
            if '«' in string:
                return True
            if '＜' in string or '《' in string:
                return False
        return False

    def convert_param_to_int(self, value):
        if isinstance(value, int):
            return value
        if isinstance(value, float):
            return int(value)
        if 'decimal' in value:
            num_pts = value.split(': ')[1].split('+')
            out_value = int(num_pts[0])
            if int(num_pts[1].split('/')[0]) > 0:
                num_pts += (int(num_pts[1].split('/')[0]) / 256)
            return out_value

