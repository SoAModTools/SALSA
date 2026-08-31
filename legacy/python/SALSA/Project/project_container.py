import uuid
from copy import copy
from itertools import count
from dataclasses import dataclass, field as dc_field
from typing import List, Union, Dict, Tuple, Literal

from SALSA.Common.constants import sep, uuid_sep
from SALSA.Scripts import scpt_condition_changes as cond_changes


@dataclass
class SCTTrace:
    sect: str
    inst: str
    param: Union[None, int]

    def __getitem__(self, item: Literal[0, 1, 2, ]):
        if -1 < item < 3:
            return [self.sect, self.inst, self.param][item]
        else:
            raise IndexError('Index out of bounds: Trace has only 4 attributes')


@dataclass
class SCTLink:
    type: str  # Available types: Jump, Switch, String
    script: str
    origin: int
    origin_trace: List[Union[int, str]]
    target: int
    target_trace: Union[List[Union[int, str]], None] = None
    ID: int = dc_field(default_factory=lambda counter=count(): next(counter))

    def __repr__(self):
        return f'Link: {self.ID}:{self.script} - {self.origin_trace[0]}:{self.origin_trace[1]}:{self.origin_trace[2]}' \
               f'\n\torigin: {self.origin}, target: {self.target}'

    def __eq__(self, other_link):
        if self.type != other_link.type:
            return False
        if self.script != other_link.script:
            return False
        if self.target_trace is None:
            if other_link.target_trace is not None:
                return False
        elif other_link.target_trace is None:
            if self.target_trace is not None:
                return False
        else:
            for i, element in enumerate(self.target_trace):
                if element != other_link.target_trace[i]:
                    return False
        for i, element in enumerate(self.origin_trace):
            if element != other_link.origin_trace[i]:
                return False
        return True


class SCTParameter:

    value: Union[int, float, str, dict, bytearray, None]
    formatted_value: str
    raw_bytes: bytearray
    analyze_log: dict
    skip_refresh: bool
    link_value: (str, str)
    link: Union[None, SCTLink]

    def __init__(self, _id, _type):
        self.ID = _id
        self.type = _type
        self.link = None
        self.errors = []
        self.analyze_log = {}
        self.value = None
        self.formatted_value = ''
        self.raw_bytes = bytearray(b'')
        self.linked_string = None
        self.override = None
        self.arithmetic_value = None

    def set_value(self, value: Union[int, float, str, dict, bytearray], override_value=None):
        self.override = override_value
        self.value = value
        self.formatted_value = self._unpack_result_dict(value, 0) if isinstance(value, dict) else str(value)
        if isinstance(value, int) or isinstance(value, float):
            self.set_arithmetic_result(value)

    def add_raw(self, raw: bytearray):
        self.raw_bytes += raw

    def add_error(self, error: str):
        self.errors.append(error)

    def set_param_log(self, log):
        self.analyze_log = log

    def _unpack_result_dict(self, cur_dict, level):
        returnValue = ''
        for key, value in cur_dict.items():
            if not isinstance(value, dict):
                formatedReturn = '\n'
                tabs = '  ' * level
                formatedReturn += tabs
                returnValue += f'{formatedReturn}{key}: {value}'
            else:
                nextLevel = self._unpack_result_dict(value, level + 1)
                formatedReturn = '\n'
                tabs = '  ' * level
                formatedReturn += tabs
                returnValue += f'{formatedReturn}{key}: {nextLevel}'
        return returnValue

    def __repr__(self):
        if self.override is not None:
            return 'Overridden'
        return f'{self.formatted_value}'

    def set_override(self, o_value: bytearray):
        self.override = o_value

    def set_arithmetic_result(self, result):
        self.arithmetic_value = result


class SCTInstruction:

    errors: List[Tuple[str, Union[int, str, bytearray]]]
    links_out: List[SCTLink]
    links_in: List[SCTLink]
    params: Dict[int, SCTParameter]
    l_params: List[Dict[int, SCTParameter]]
    ungrouped_position: int
    delay_param: Union[None, SCTParameter]

    def __init__(self):
        self.ID: str = str(uuid.uuid4()).replace('-', uuid_sep)
        self.base_id = None
        self.absolute_offset = None
        self.skip_refresh = False
        self.delay_param = None
        self.errors = []
        self.links_out = []
        self.links_in = []
        self.params = {}
        self.l_params = []
        self.condition = ''
        self.synopsis = ''
        self.ungrouped_position = -1
        self.my_goto_uuids = []
        self.my_master_uuids = []
        self.label = ''
        self.encode_inst = True

    def set_inst_id(self, inst_id):
        self.base_id = inst_id

    def set_pos(self, inst_pos: int):
        self.absolute_offset = inst_pos

    def add_error(self, value: Tuple[str, Union[int, str, bytearray]]):
        self.errors.append(value)

    def add_parameter(self, param_id, param: SCTParameter):
        self.params[param_id] = param
        if len(param.errors) > 0:
            self.errors.append(('param', param_id))

    def add_loop_parameter(self, loop_param: Dict[int, SCTParameter]):
        self.l_params.append(loop_param)
        for p_id, param in loop_param.items():
            if len(param.errors) > 0:
                self.errors.append((f'loop{sep}{len(self.l_params) - 1}{sep}param', p_id))

    def generate_condition(self, var_aliases):
        # Only generate conditions for ifs and switches
        if self.base_id not in (0, 3):
            self.condition = ''
            return

        # if the instruction is a switch, just put the value of the choice address
        if self.base_id == 3:
            if self.params[0].value is None:
                self.condition = '???'
            cond = self.params[0].value
            self.condition = cond if isinstance(cond, str) else str(cond)
            return

        # if the instruction is a jmpif, work out a shorthand of the condition
        if self.params[0].value is None:
            self.condition = '???'
            return
        condition, cond_type = self._get_subconditions(self.params[0].value)
        condition = self.replace_vars_with_aliases(condition, var_aliases)
        self.condition = condition

    def _get_subconditions(self, cond_dict):
        result_str = ''
        result_type = 'float'
        if not isinstance(cond_dict, dict):
            return cond_dict if isinstance(cond_dict, str) else str(cond_dict), result_type

        for key, value in cond_dict.items():
            if key in cond_changes.scpt_types.keys():
                result_type = cond_changes.scpt_types[key]
            result_1, result_type_1 = self._get_subconditions(value['1'])
            result_2, result_type_2 = self._get_subconditions(value['2'])
            if key in cond_changes.scpt_log_changes.keys():
                param_key = f'{result_type_1}{sep}{result_type_2}'
                if param_key in cond_changes.scpt_log_changes[key]:
                    key = cond_changes.scpt_log_changes[key][param_key]
            temp_result = key.replace('(1)', result_1)
            result_str = temp_result.replace('(2)', result_2)

        return result_str, result_type

    @staticmethod
    def replace_vars_with_aliases(condition, aliases):
        pieces = condition.split('Var: ')
        if len(pieces) == 1:
            return condition
        prev_piece = ''
        first = True
        cond_out = ''
        for piece in pieces:
            if first:
                first = False
                prev_piece = piece
                cond_out = ' '.join(prev_piece.split(' ')[:-1])
                continue
            cur_piece = piece
            prev_pieces = prev_piece.split(' ')
            cur_pieces = cur_piece.split(' ')
            if prev_pieces[-1]+'Var' in aliases.keys():
                if int(cur_pieces[0]) in aliases[prev_pieces[-1]+'Var']:
                    if isinstance(aliases[prev_pieces[-1]+'Var'][int(cur_pieces[0])], dict):
                        alias = aliases[prev_pieces[-1]+'Var'][int(cur_pieces[0])]['alias']
                    else:
                        alias = aliases[prev_pieces[-1]+'Var'][int(cur_pieces[0])]
                else:
                    alias = ''
            else:
                alias = ''
            alias = alias if alias != '' else f'{prev_pieces[-1]}Var: {cur_pieces[0]}'
            cond_out += f'{alias} {" ".join(cur_pieces[1:])}'
            prev_piece = cur_piece
        return cond_out

    def __repr__(self):
        return f'{self.base_id}'


class SCTSection:

    insts: Dict[str, SCTInstruction]
    inst_tree: List[Union[str, Dict[str, Union[list, dict, str]]]]
    inst_errors: List[int]
    errors: List[str]
    strings: Dict[int, str]
    insts_used: Dict[int, int]
    garbage: Dict[str, bytearray]

    def __init__(self):
        self.name = None
        self.length = None
        self.absolute_offset = None
        self.insts = {}
        self.inst_tree = []
        self.inst_list = []
        self.inst_errors = []
        self.errors = []
        self.strings = {}
        self.insts_used = {}
        self.garbage = {}
        self.string = ''
        self.jump_loops = []
        self.internal_sections_inst = {}
        self.internal_sections_curs = {}
        self.is_compound = False
        self.type = ''

    def set_name(self, name):
        self.name = name

    def set_details(self, length, pos):
        self.length = length
        self.absolute_offset = pos

    def set_string(self, pos: int, string: str):
        self.strings[pos] = string
        self.string = string

    def add_instruction(self, instruction: SCTInstruction):
        self.insts[instruction.ID] = instruction
        self.inst_list.append(instruction.ID)
        if instruction.base_id not in self.insts_used.keys():
            self.insts_used[instruction.base_id] = 0
        self.insts_used[instruction.base_id] += 1

    def set_type(self, t: str):
        self.type = t

    def add_error(self, e: str):
        self.errors.append(e)

    def add_garbage(self, key: str, value: bytearray):
        self.garbage[key] = value

    def get_inst_list(self, style):
        return self.inst_tree if style == 'grouped' else self.inst_list

    def get_inst_by_index(self, pos):
        return self.insts[self.inst_list[pos]]


class SCTScript:
    start_pos = None

    special_addresses = {
        'bit': '0x80310bc',
        'byte': '0x80310a1c'
    }

    sects: Dict[str, SCTSection]
    inst_locations: List[List[str]]
    links: List[SCTLink]
    footer: List[str]
    strings: Dict[str, str]
    error_sections: Dict[str, List[str]]
    links_to_sections: Dict[str, List[str]]
    unused_sections: List[str]

    def __init__(self, name: str, index: Union[None, Dict[str, Tuple[int, int]]] = None,
                 header: Union[None, bytearray] = None):
        self.folded_sects = {}
        self.name = name
        self.index = {k: v[0] for k, v in index.items()} if index is not None else {}
        self.header = header
        self.sects = {}
        self.sect_tree = []
        self.sect_list = []
        self.inst_locations = [[] for _ in range(266)]
        self.links = []
        self.footer = []
        self.strings = {}
        self.string_groups = {}
        self.string_locations = {}
        self.string_garbage = {}
        self.unused_sections = []
        self.errors = []
        self.error_sections = {}
        self.variables = []
        self.section_num = 0

    def add_section(self, section: SCTSection):
        name = section.name
        self.sects[name] = section
        self.sect_list.append(name)
        inst_list = self.sects[name].insts_used.keys()
        for inst in inst_list:
            self.inst_locations[inst].append(name)
        if len(section.errors) > 0:
            self.error_sections[name] = section.errors
        self.section_num += 1

    def get_sect_list(self, style):
        if style == 'grouped':
            return self.sect_tree
        return [sect.name for sect in self.sects.values() if sect.type != 'String']


class SCTProject:

    file_name: str
    scts: Dict[str, SCTScript]

    cur_version = 7

    def __init__(self):
        self.scts = {}
        self.file_name = 'Untitled.prj'
        self.filepath = None
        self.global_variables = {'BitVar': {}, 'IntVar': {}, 'ByteVar': {}, 'FloatVar': {}}
        self.version = copy(self.cur_version)
        self.inst_id_colors: Dict[int, str] = {}

    def set_color(self, inst_id, color = ''):
        if isinstance(inst_id, str):
            inst_id = int(inst_id)

        self.inst_id_colors[inst_id] = color

    def get_color(self, inst_id):
        if inst_id is None or inst_id == 'None':
            return ''
        if isinstance(inst_id, str):
            inst_id = int(inst_id)
        return self.inst_id_colors.get(inst_id, '')
