import copy
import json
import os.path
from typing import List, Dict

from SALSA.BaseInstructions.bi_container import BaseInstLib, BaseInst, locked_conversions
from SALSA.FileModels.instruction_model import InstructionModel
from SALSA.Common.constants import LOCK


modifiers = (129, 13)


class BaseInstLibFacade:

    def __init__(self):
        self.group_inst_list = [0, 3]
        self.inst_model = InstructionModel()
        self.default_inst_details = self.inst_model.load_instructions('default')
        self.user_identifier = 'default'
        self.default_lib = self._set_inst_all_fields(self.default_inst_details, BaseInstLib())
        if os.path.exists('./UserSettings/user_is_default.txt'):
            self.user_inst_details = None
            self.lib = self.default_lib
        else:
            self.user_inst_details = self.inst_model.load_instructions('user')
            self.user_identifier = 'user'
            self.lib = self._set_inst_all_fields(self.user_inst_details, copy.deepcopy(self.default_lib))

        self.details_getter_fxns = {
            'default': self._get_default_inst_details,
            'user': self._get_user_specific_details
        }

    def _set_inst_all_fields(self, inst_dict, lib):
        for inst_id, inst_settings in inst_dict.items():
            if isinstance(inst_id, str):
                inst_id = int(inst_id)
            lib.insts[inst_id].set_inst_details(inst_settings, self.user_identifier)
        return lib

    def get_inst(self, inst_id) -> BaseInst:
        return self.lib.insts[inst_id]

    def _get_default_inst_details(self):
        return {k: v.get_default_inst_details() for k, v in enumerate(self.lib.insts)}

    def get_all_insts(self) -> List[BaseInst]:
        return self.lib.insts

    # Used to set details from a json file
    def set_inst_details(self, inst_id, details: dict):
        self.lib.insts[inst_id].set_inst_details(details)

    # Used to set details with the instruction editor
    def set_single_inst_detail(self, inst_id, field, value, param_id=None):
        self.lib.insts[int(inst_id)].set_inst_field(field, value, param_id)

    def save_user_insts(self):
        self.inst_model.save_instructions(inst_dict=self.details_getter_fxns[self.user_identifier](), inst_type=self.user_identifier)

    def _get_user_specific_details(self):
        return self.lib.get_differences(self.default_lib)

    def get_tree_entries(self, headers, inst_id=None):
        if inst_id is None:
            elements = self.lib.insts
            id_key = 'inst_ID'
        else:
            elements = self.lib.insts[inst_id].params
            elements = [elements[k] for k in range(len(elements))]
            id_key = 'param_ID'

        tree = []
        for i, element in enumerate(elements):
            entry = {}
            element = element.__dict__
            for item in headers:
                if item == id_key:
                    entry[id_key] = f'{i}'
                    continue
                if item not in element:
                    raise KeyError(f'Item not present in inst dict: {item}')
                entry[item] = element[item]
                if inst_id is not None:
                    if self.check_locked([inst_id, i, item]):
                        entry[item] = f'{LOCK} {entry[item]}'

            # Locks every parameter field except name
            for key in entry:
                if key in ('name', 'default_value'):
                    continue
                if LOCK not in entry[key] and key != id_key:
                    entry[key] = f'{LOCK} {entry[key]}'

            tree.append(entry)
        return tree

    def check_locked(self, item_code: list):
        inst_id = item_code.pop(0)
        element = self.lib.insts[inst_id]
        locked_key = 'instruction'
        if len(item_code) > 1:
            param_id = int(item_code.pop(0))
            element = element.params[param_id]
            locked_key = 'parameter'
        field = locked_conversions[locked_key][item_code[0]]
        result = field in element.locked_fields
        return result

    def get_user_type(self):
        return self.user_identifier

    def get_relevant(self, search, exclude_modifiers=False):
        relevant = []
        if search == '':
            return [f'{inst.instruction_id} - {inst.name}' for inst in self.lib.insts if inst.name != 'code_error']
        for inst in self.lib.insts:
            inst: BaseInst
            if inst.name == 'code_error':
                continue
            if search in str(inst.instruction_id) or search.lower() in inst.name.lower():
                if exclude_modifiers and inst.instruction_id in modifiers:
                    continue
                relevant.append(f'{inst.instruction_id} - {inst.name}')
        return relevant


if __name__ == '__main__':

    base_insts = BaseInstLibFacade()
    insts: List[BaseInst] = base_insts.lib.insts
    for i in range(len(insts)):
        if insts[i].instruction_id != i:
            print(f'inst at position {i} is {insts[i].instruction_id}')

    with open('DefaultInstructions.json', 'r') as fh:
        file = fh.read()
        file_json = json.loads(file)

    base_insts._set_inst_all_fields(file_json)

    inst_deets = base_insts._get_default_inst_details()
    print('loaded')
