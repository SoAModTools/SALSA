from typing import List
from SALSA.BaseInstructions.bi_defaults import inst_defaults


locked_conversions = {
    'instruction': {
        'loop': 'Loop',
        'loop_iter': 'Loop Iterations',
        'loop_cond': 'Loop Break Condition',
        'name': 'Name',
        'description': 'Description',
        'location': 'Location',
        'no_new_frame': 'Skip Frame Refresh',
        'forced_new_frame': 'Force Frame Refresh',
        'link': 'Link',
        'link_type': 'Link Type',
        'param2': 'Hard parameter two',
        'notes': 'Notes',
        'user_notes': 'Notes',
        'default_notes': 'Notes'
    },
    'parameter': {
        'name': 'Name',
        'type': 'Type',
        'default_value': 'Default',
        'mask': 'Mask',
        'is_signed': 'Signed'
    }
}


class BaseParam:

    def __init__(self, param_id, param_dict, default_name, link_type=None):

        self.locked_fields = [k for k in param_dict.keys()]
        self.param_ID = param_id
        self.name: str = param_dict.get('Name', default_name)
        self.type: str = param_dict['Type']
        self.default_value = param_dict.get('Default', None)
        self.mask = param_dict.get('Mask', None)
        self.is_signed = param_dict.get('Signed', None)
        self.link_type = link_type

        # # Use for finding unused keys in instruct defaults
        # for key in param_dict.keys():
        #     if key not in ['Name', 'Type', 'Default', 'Mask', 'Signed']:
        #         print('Extra parameter field present...')

    def set_parameter_details(self, param_details):
        if 'Name' not in self.locked_fields:
            self.name = param_details.get('Name', self.name)
        if 'Mask' not in self.locked_fields:
            if 'Mask' in param_details.keys():
                if isinstance(param_details['Mask'], bool):
                    if 'Mask Value' in param_details.keys() and param_details['Mask']:
                        temp_mask = param_details.get('Mask Value')
                        self.mask = int(temp_mask, 16) if isinstance(temp_mask, str) else temp_mask
                else:
                    self.mask = param_details.get('Mask', self.mask)
        if 'Signed' not in self.locked_fields:
            self.is_signed = param_details.get('Signed', self.is_signed)
        if 'Default' not in self.locked_fields and self.type != 'int':
            self.default_value = param_details.get('Default', self.default_value)

    def get_fields(self):
        fields = {}
        if 'Name' not in self.locked_fields:
            fields['Name'] = self.name
        if 'Signed' not in self.locked_fields:
            fields['Signed'] = self.is_signed
        if 'Mask' not in self.locked_fields and self.mask is not None:
            fields['Mask'] = self.mask
        if 'Default' not in self.locked_fields and self.default_value is not None:
            fields['Default'] = self.default_value
        return fields

    def set_parameter_detail(self, field, value):
        if locked_conversions['parameter'][field] not in self.locked_fields:
            self.__setattr__(field, value)

    def get_differences(self, other_param):
        return {'Name': self.name} if self.name != other_param.name else {}


class BaseInst:

    def __init__(self, inst_id, inst_values):

        self.locked_fields = [k for k in inst_values.keys() if k != 'Parameters']
        self.instruction_id = inst_id
        self.name = inst_values.get('Name', f'Inst{inst_id}')
        self.description = inst_values.get('Description', '\n')
        self.location = inst_values['Location']
        self.no_new_frame = bool(inst_values['Skip Frame Refresh'])
        self.forced_new_frame = inst_values['Force Frame Refresh'] if self.no_new_frame == 0 else False

        self.link = inst_values.get('Link', None)
        self.link_type = inst_values.get('Link Type', None)

        self.param2 = inst_values['Hard parameter two']
        self.default_notes = ''
        self.user_notes = None
        self.params = {}
        for key, param in inst_values['Parameters'].items():
            link_type = None
            if self.link is not None:
                if self.link == key:
                    link_type = self.link_type
            self.params[key] = BaseParam(param_id=key, param_dict=param, default_name=f'Unknown{key}', link_type=link_type)

        self.loop = inst_values.get('Loop', None)
        self.loop_iter = inst_values.get('Loop Iterations', None)
        self.loop_cond = inst_values.get('Loop Break Condition', None)

        self.params_before = list(self.params.keys()) if self.loop is None else []
        self.params_after = []
        hit_loop = False
        if self.loop is not None:
            for p in list(self.params.keys()):
                if p in self.loop:
                    hit_loop = True
                    continue
                if hit_loop:
                    self.params_after.append(p)
                else:
                    self.params_before.append(p)

        self.warning = inst_values.get('Warning', None)

    def set_inst_details(self, updated_details, user_id):
        if 'Name' not in self.locked_fields:
            self.name = updated_details.get('Name', self.name)
        if 'Description' not in self.locked_fields:
            self.description = updated_details.get('Description', self.description)
        if 'Parameters' in updated_details.keys():
            for key, param_details in updated_details['Parameters'].items():
                if isinstance(key, str):
                    key = int(key)
                self.params[key].set_parameter_details(param_details=param_details)

        if user_id == 'default':
            self.default_notes = updated_details.get('Notes', '')
        elif user_id == 'user':
            self.user_notes = updated_details.get('Notes', '')
        else:
            raise KeyError(f'BaseInst: Unknown user type: {user_id}')

    def set_inst_field(self, field, value, param_id=None):
        if param_id is not None:
            return self.params[int(param_id)].set_parameter_detail(field, value)
        if locked_conversions['instruction'][field] not in self.locked_fields:
            self.__setattr__(field, value)

    def get_default_inst_details(self):
        fields = {}
        if 'Name' not in self.locked_fields:
            fields['Name'] = self.name
        if 'Description' not in self.locked_fields:
            fields['Description'] = self.description
        fields['Notes'] = self.default_notes
        params = {}
        for key, param in self.params.items():
            p_dict = param.get_fields()
            if len(p_dict) > 0:
                params[key] = p_dict
        if len(params) > 0:
            fields['Parameters'] = params
        return fields

    def get_param_names(self, param_list):
        names = {}
        for param in self.params.values():
            if int(param.param_ID) in param_list:
                names[int(param.param_ID)] = param.name
        return names

    def __repr__(self):
        return f'INST({self.instruction_id}: pos:{self.location}'

    def get_differences(self, other_inst):
        return_diffs = {}
        if self.name != other_inst.name:
            return_diffs['Name'] = self.name
        if self.description != other_inst.description:
            return_diffs['Description'] = self.description
        return_diffs['Notes'] = self.user_notes
        parameter_diffs = {}
        for key, param in self.params.items():
            diffs = param.get_differences(other_inst.params[key])
            if len(diffs) > 0:
                parameter_diffs[key] = diffs
        if len(parameter_diffs) > 0:
            return_diffs['Parameters'] = parameter_diffs
        return return_diffs


class BaseInstLib:
    """Takes in a dictionary containing instruction information and produces an object containing
        the necessary information to decode *.sct files"""

    def __init__(self):
        self.insts = [BaseInst(k, v) for k, v in inst_defaults.items()]
        insts_with_a_parameter = [_ for _ in self.insts if len(_.params) > 0]
        self.p1_scpt = [_.instruction_id for _ in insts_with_a_parameter if 'scpt' in _.params[0].type]
        self.p1_int = [_.instruction_id for _ in insts_with_a_parameter if 'int' in _.params[0].type]

    def get_differences(self, other_lib):
        other_lib: BaseInstLib
        diffs_out = {}
        for i, inst in enumerate(self.insts):
            diffs = inst.get_differences(other_lib.insts[i])
            if len(diffs) != 0:
                diffs_out[i] = diffs

        return diffs_out


if __name__ == '__main__':

    base_insts = BaseInstLib()
    insts: List[BaseInst] = base_insts.insts
    for i in range(len(insts)):
        if insts[i].instruction_id != i:
            print(f'inst at position {i} is {insts[i].instruction_id}')
