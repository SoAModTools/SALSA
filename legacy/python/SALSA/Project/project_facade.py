import copy
from typing import Union, Tuple, Literal

from Project.project_container import SCTScript
from SALSA.Common.string_utils import get_padding_for_number
from SALSA.Project.project_searcher import ProjectSearcher
from SALSA.Project.RepairTools.texbox_disappear_repair import TBStringToParamRepair
from SALSA.Project.Updater.project_updater import ProjectUpdater
from SALSA.BaseInstructions.bi_defaults import loop_count_name
from SALSA.Common.script_string_utils import SAstr_to_head_and_body, head_and_body_to_SAstr, blank_string
from SALSA.Project.description_formatting import format_description
from SALSA.Project.project_container import SCTProject, SCTSection, SCTParameter, SCTInstruction, SCTLink
from SALSA.BaseInstructions.bi_facade import BaseInstLibFacade
from SALSA.Common.setting_class import settings
from SALSA.Common.constants import sep, alt_sep, alt_alt_sep, uuid_sep, label_name_sep, compound_sect_suffix, \
    virtual_sect_suffix, label_sect_suffix, link_sep, footer_str_group_name, override_str, do_not_encode_str, \
    string_group_sect_suffix
from SALSA.Scripts.scpt_param_codes import get_scpt_override
from SALSA.Scripts.script_encoder import SCTEncoder


class SCTProjectFacade:
    project: Union[SCTProject, None]
    log_key = 'PrjFacade'

    def __init__(self, base_insts: BaseInstLibFacade):
        self.project = None
        self.callbacks = {}
        self.base_insts = base_insts
        if self.log_key not in settings.keys():
            settings[self.log_key] = {}

        self.desc_callbacks = {
            'get_str': self.get_string_to_edit,
            'get_inst': self.get_inst_desc_info
        }
        self.cur_script = None
        self.searcher = None

    def load_project(self, prj: SCTProject):
        # version numbers were not given for the first version
        if getattr(prj, 'version', None) is None:
            prj.version = 1

        # Check that the loaded project is the current version and update if possible
        if prj.version != SCTProject.cur_version:
            if prj.version < SCTProject.cur_version:
                prj = ProjectUpdater.update_project(prj)
                self.callbacks['delay_set_change'](100)
            else:
                print(f'{self.log_key}: This project was created in a newer version of SALSA. '
                      f'Please update SALSA to read this project.'
                      f'\n(project version: {prj.version}, SALSA project version: {SCTProject.cur_version})')
                return False

        self.project = prj
        self.project_cleanup_placeholder_insts()
        self.project_cleanup_string_headers()
        self.searcher = ProjectSearcher(self.base_insts, self.project)
        return True

    def project_cleanup_placeholder_insts(self):
        for sct_name in self.project.scts:
            script = self.project.scts[sct_name]
            for sect_name in script.sects:
                section = script.sects[sect_name]
                insts_to_remove = []
                for inst in section.insts:
                    if section.insts[inst].base_id is None:
                        insts_to_remove.append(inst)

                for inst in insts_to_remove:
                    self.remove_inst(sct_name, sect_name, inst, None)

    def project_cleanup_string_headers(self):
        for sct_name in self.project.scts:
            cur_script = self.project.scts[sct_name]
            for string in cur_script.strings:
                if '\\h' not in cur_script.strings[string]:
                    cur_script.strings[string] = '\\h()' + cur_script.strings[string]

    def create_new_project(self):
        self.project = SCTProject()

    def set_filepath(self, filepath):
        if filepath == '' or filepath is None:
            return
        self.project.filepath = filepath

    def get_filepath(self):
        return self.project.filepath

    # ---------------------- #
    # Project Editor Methods #
    # ---------------------- #

    def set_callback(self, key, callback):
        self.callbacks[key] = callback

    # Tree methods

    def get_tree(self, headers: Tuple[str], script=None, section=None, style='grouped', **kwargs):
        if self.project is None:
            return
        if script is None:
            scripts = self.project.scts
            tree_list = self._create_tree(group=scripts, key_list=list(scripts.keys()), headers=headers)
        elif section is None:
            self.cur_script = script
            section_list = self.project.scts[script].get_sect_list(style)
            sections = self.project.scts[script].sects
            tree_list = self._create_tree(group=sections, key_list=section_list, headers=headers)
        elif 'instruction' in kwargs:
            tree_list = self.get_parameter_tree(script, section, kwargs['instruction'], headers)
        else:
            self.cur_script = script
            self._refresh_inst_positions(script=script, section=section)
            self._refresh_all_conditions(script=script, section=section)
            inst_list = self.project.scts[script].sects[section].get_inst_list(style)
            instructions = self.project.scts[script].sects[section].insts
            tree_list = self._create_tree(group=instructions, key_list=inst_list, headers=headers,
                                          base=self.base_insts.get_all_insts(), base_key='base_id')

        return tree_list

    def _create_tree(self, group, key_list, headers, base=None, base_key=None, prev_group_type=None,
                     key_only=False, key_only_key=''):

        tree_list = []
        for key in key_list:
            if isinstance(key, dict):
                for ele_key, ele_value in key.items():

                    # This detects switch cases
                    if sep not in ele_key:
                        kok = 'name'
                        tree_list.extend(self._create_tree(group=group, key_list=[ele_key], headers=headers, base=base,
                                                           base_key=base_key, key_only=True, key_only_key=kok))
                        tree_list[-1]['group_type'] = 'case'
                        if len(ele_value) != 0:
                            tree_list[-1][headers[0]] = group[self.get_inst_uuid_from_group_entry(ele_value)].ungrouped_position
                            tree_list[-1]['absolute_offset'] = group[self.get_inst_uuid_from_group_entry(ele_value)].absolute_offset
                        else:
                            tree_list[-1][headers[0]] = ''
                            tree_list[-1]['absolute_offset'] = ''
                    else:
                        ele_key = ele_key.split(sep)
                        tree_list.extend(
                            self._create_tree(group=group, key_list=[ele_key[0]], headers=headers, base=base,
                                              base_key=base_key, prev_group_type=ele_key[1]))
                        tree_list[-1]['group_type'] = ele_key[1]
                        if ele_key[1] == 'else':
                            if len(ele_value) == 0:
                                tree_list[-1][headers[0]] = '??'
                                tree_list[-1]['absolute_offset'] = None
                            else:
                                tree_list[-1][headers[0]] = group[self.get_inst_uuid_from_group_entry(ele_value)].ungrouped_position
                                tree_list[-1]['absolute_offset'] = group[self.get_inst_uuid_from_group_entry(ele_value)].absolute_offset
                    tree_list.append('group')
                    ele_value = [ele_value] if not isinstance(ele_value, list) else ele_value
                    cur_group_type = ele_key[1] if len(ele_key) > 1 else ''
                    tree_list.extend(self._create_tree(group=group, key_list=ele_value, headers=headers, base=base,
                                                       base_key=base_key, prev_group_type=cur_group_type))
                    tree_list.append('ungroup')
                continue

            if key_only:
                values = {'row_data': None}
                if key_only_key == '':
                    key_only_key = headers[0]
                for header in headers:
                    if key_only_key == header:
                        values[header] = key
                        continue
                    values[header] = ''
                tree_list.append(values)

            else:
                element = group[key]
                if isinstance(element, SCTSection):
                    if element.type == 'String':
                        continue
                element_dict = element.__dict__
                values = {'row_data': key}
                for header_key in headers:
                    value_dict = element_dict
                    if header_key not in element_dict:
                        if base is None or base_key is None:
                            raise KeyError(
                                f'PrjFacade: {header_key} not in {element}, no base_element_group or base_key given')

                        base_inst_key = element_dict[base_key]
                        if base_inst_key is None:
                            base_dict = {header_key: '---'}
                        else:
                            base_dict = base[base_inst_key].__dict__

                        if header_key not in base_dict:
                            raise KeyError(f'PrjFacade: {header_key} not in {element} or {base[base_key]}')
                        value_dict = base_dict

                    if header_key == 'value' and element_dict['override'] is not None:
                        values[header_key] = 'Overridden'
                    else:
                        values[header_key] = value_dict[header_key] if isinstance(value_dict[header_key], str) else str(
                            value_dict[header_key])
                if isinstance(element, SCTSection):
                    if element.is_compound:
                        values['name'] += compound_sect_suffix
                    elif len(element.inst_list) == 0:
                        values['name'] += virtual_sect_suffix
                    elif len(element.inst_list) == 1:
                        values['name'] += label_sect_suffix
                    if element.name in self.project.scts[self.cur_script].string_groups:
                        values['name'] += string_group_sect_suffix
                if isinstance(element, SCTInstruction):
                    if element.base_id == 9:
                        values['name'] += label_name_sep + element.label
                    if not element.encode_inst:
                        values['name'] = do_not_encode_str + values['name']
                tree_list.append(values)
        return tree_list

    def get_parameter_tree(self, script, section, instruction, headings, **kwargs):
        inst = self.project.scts[script].sects[section].insts[instruction]
        base_inst = self.base_insts.get_inst(inst.base_id)

        tree = self._create_tree(inst.params, base_inst.params_before, headings, base=base_inst.params,
                                 base_key='ID')

        for param in base_inst.params_before:
            if 'string' in base_inst.params[param].type:
                tree[param]['type'] = 'String' if 'footer' not in base_inst.params[param].type else 'Footer String'
                tree[param]['value'] = inst.params[param].linked_string
            elif 'footer' in base_inst.params[param].type:
                tree[param]['type'] = 'Footer Entry'
                tree[param]['value'] = inst.params[param].linked_string

            if 'jump' in base_inst.params[param].type or 'subscript' in base_inst.params[param].type:
                tree[param]['type'] = 'Jump'
                if inst.params[param].link is None:
                    continue
                tgt_sect = inst.params[param].link.target_trace[0]
                tgt_inst = inst.params[param].link.target_trace[1]
                target_inst = self.project.scts[script].sects[tgt_sect].insts[tgt_inst]
                tgt_name = self.base_insts.get_inst(target_inst.base_id).name
                tree[param]['value'] = f'{tgt_sect} - {target_inst.ungrouped_position} {tgt_name}'

        if base_inst.loop is None:
            return tree

        for loop, params in enumerate(inst.l_params):
            temp_tree = self._create_tree(params, [f'loop{loop}'], headings, key_only=True)
            temp_tree += ['group']
            loop_items = self._create_tree(params, base_inst.loop, headings, base=base_inst.params, base_key='ID')
            for item in loop_items:
                if 'row_data' not in item:
                    continue
                item['row_data'] = f'{loop}{sep}{item["row_data"]}'
            temp_tree += loop_items
            temp_tree += ['ungroup']
            tree += temp_tree

        tree += self._create_tree(inst.params, base_inst.params_after, headings, base=base_inst.params,
                                  base_key='ID')

        return tree

    def get_instruction_details(self, script, section, instruction, **kwargs):
        try:
            instruction = self.project.scts[script].sects[section].insts[instruction]
        except KeyError as e:
            print(self.log_key, e)
            return None
        if instruction.base_id is None:
            return None
        base_inst = self.base_insts.get_inst(instruction.base_id)
        instruction_details = copy.deepcopy(base_inst.__dict__)
        instruction_details['base_parameters'] = instruction_details['params']
        for key, value in instruction.__dict__.items():
            instruction_details[key] = value
        instruction_details['description'] = format_description(inst=instruction, base_inst=base_inst, callbacks=self.desc_callbacks)
        return instruction_details

    def add_scripts_to_project(self, scripts: dict):
        for name, script in scripts.items():
            self._add_script_to_project(name, script)
        if 'update_scripts' not in self.callbacks:
            return
        self.callbacks['update_scripts']()

    def _add_script_to_project(self, script_name, script):
        self.project.scts[script_name] = script
        script_keys = sorted(list(self.project.scts.keys()), key=str.casefold)
        self.project.scts = {k: self.project.scts[k] for k in script_keys}

    def remove_script(self, rowdata):
        if rowdata not in self.project.scts:
            return
        self.project.scts.pop(rowdata)

    def get_project_script_by_name(self, name):
        if name not in self.project.scts.keys():
            print(f'{self.log_key}: No script loaded with the name: {name}')
            return None
        return self.project.scts[name]

    def create_delay_parameter(self):
        return SCTParameter(_id=-1, _type='scpt-int')

    def set_delay_parameter(self, delay_parameter, script, section, instruction, **kwargs):
        self.project.scts[script].sects[section].insts[instruction].delay_param = delay_parameter

    def remove_delay_parameter(self, script, section, instruction, **kwargs):
        self.project.scts[script].sects[section].insts[instruction].delay_param = None

    def update_inst_field(self, field, value, script, section, instruction, **kwargs):
        inst = self.project.scts[script].sects[section].insts[instruction]
        inst.__setattr__(field, value)

    # ---------------------------- #
    # Instruction position methods #
    # ---------------------------- #

    def get_inst_uuid_by_ind(self, script, section, inst_ind):
        return self.project.scts[script].sects[section].inst_list[int(inst_ind)]

    def get_inst_ind(self, script, section, inst):
        return self.project.scts[script].sects[section].inst_list.index(inst)

    def _refresh_inst_positions(self, script, section):
        section = self.project.scts[script].sects[section]
        for i, inst_id in enumerate(section.inst_list):
            section.insts[inst_id].ungrouped_position = i

    def _refresh_all_conditions(self, script, section):
        for inst in self.project.scts[script].sects[section].insts.values():
            inst.generate_condition(self.get_script_variables_with_aliases(script))

    # ----------------------- #
    # Script variable methods #
    # ----------------------- #

    def get_script_variables_with_aliases(self, script):
        global_vars = self.project.global_variables
        var_dict = None if script is None else self.project.scts[script].variables
        if var_dict is None:
            var_dict = global_vars
        all_vars = {}
        for var_type, var_list in var_dict.items():
            all_vars[var_type] = {}
            var_order = sorted(list(var_list.keys()))
            for key in var_order:
                all_vars[var_type][key] = {'alias': var_list[key]['alias']}
                if var_dict is not global_vars:
                    all_vars[var_type][key]['is_global'] = key in global_vars[var_type]
                else:
                    all_vars[var_type][key]['is_global'] = True
        return all_vars

    def get_var_alias(self, script, var_type, var_id):
        if ':' in var_type:
            var_type = var_type.split(':')[0]
        if var_id not in self.project.scts[script].variables[var_type]:
            return 'No Alias'
        return self.project.scts[script].variables[var_type][var_id]['alias']

    def set_variable_alias(self, script, var_type, var_id, alias):
        var_dict = self.project.global_variables[var_type] if script is None \
            else self.project.scts[script].variables[var_type]

        if var_id not in var_dict:
            var_dict[var_id] = {'alias': alias}
        else:
            var_dict[var_id]['alias'] = alias

    def remove_global(self, var_type, var_id):
        if var_id in self.project.global_variables[var_type]:
            self.project.global_variables[var_type].pop(var_id)

    def get_variable_usages(self, script, var_type, var_id):
        if script is not None:
            return self.project.scts[script].variables[var_type][var_id]['usage']
        usage = []
        for name, cur_script in self.project.scts.items():
            if var_id in cur_script.variables[var_type]:
                usage += [(name, *_) for _ in list(cur_script.variables[var_type][var_id]['usage'])]
        return usage

    def update_var_usage(self, changes, script, section, instruction, parameter):
        trace = (section, instruction, parameter)

        for change in changes['add']:
            var_parts = change.split(': ')
            var_type = var_parts[0]
            var_key = int(var_parts[1])
            if var_key in self.project.scts[script].variables[var_type]:
                self.project.scts[script].variables[var_type][var_key]['usage'].append(trace)
            else:
                self.project.scts[script].variables[var_type][var_key] = {'alias': '', 'usage': [trace]}

        for change in changes['remove']:
            var_parts = change.split(': ')
            var_type = var_parts[0]
            var_key = int(var_parts[1])
            if trace not in self.project.scts[script].variables[var_type][var_key]['usage']:
                print(f'{self.log_key}: No variable usage to remove for {trace} in {var_type}: {var_key}')
                continue
            self.project.scts[script].variables[var_type][var_key]['usage'].remove(trace)

    # ------------------------------ #
    # String Editor Callback Methods #
    # ------------------------------ #

    def get_string_tree(self, script, headers):
        string_groups = self.project.scts[script].string_groups
        strings = self.project.scts[script].strings
        string_tree = []
        for key, group in string_groups.items():
            string_tree.append({headers[0]: key, headers[1]: '', 'row_data': None})
            string_tree.append('group')
            for string_id in sorted(group):
                _, str_head, str_body = SAstr_to_head_and_body(strings[string_id])
                if str_head is None:
                    str_head = ''
                if '\n' in str_body:
                    str_body = str_body.split('\n')[0]
                    str_body += '...'
                string_tree.append({headers[0]: string_id, headers[1]: f'{str_head} {str_body}', 'row_data': string_id})
            string_tree.append('ungroup')

        return string_tree

    def get_string_to_edit(self, string_id, script=None):
        if string_id is None:
            return ''
        script = self.cur_script if script is None else script
        return SAstr_to_head_and_body(self.project.scts[script].strings[string_id])

    def edit_string(self, script, string_id, changes):
        if script == '' or string_id == '':
            return
        no_head, head, body = SAstr_to_head_and_body(self.project.scts[script].strings[string_id])
        if 'no_head' in changes:
            no_head = changes['no_head']
        if 'head' in changes:
            head = changes['head']
        if 'body' in changes:
            body = changes['body']
        self.project.scts[script].strings[string_id] = head_and_body_to_SAstr(no_head, head, body)
        self.callbacks['set_change'](script)

    def add_string_group(self, script, string_group=''):
        if string_group == '':
            string_group = self.get_new_sect_name(script)
        cur_script = self.project.scts[script]
        cur_script.string_groups[string_group] = []

        self.create_section(script, new_name=string_group, inst_list=[9])

        self.callbacks['set_change'](script)

        return string_group

    def remove_string_group(self, script, string_group):
        cur_script = self.project.scts[script]
        cur_script.string_groups.pop(string_group)
        self.remove_section(script, string_group)

    def rename_string_group(self, script, group_name, new_group_name):
        cur_script = self.project.scts[script]
        cur_script.string_groups[new_group_name] = cur_script.string_groups.pop(group_name)
        self.change_section_name(script, group_name, None, new_group_name)

    def add_string(self, script, string_group, string_id='', string=None):
        if string_id == '':
            string_id = self.get_new_sect_name(script)
        if string is None:
            string = blank_string
        self.project.scts[script].string_groups[string_group].append(string_id)
        self.project.scts[script].strings[string_id] = string
        self.project.scts[script].string_locations[string_id] = string_group

        self.callbacks['set_change'](script)

        return string_id

    def delete_string(self, script, string_id):
        self.project.scts[script].strings.pop(string_id)
        group = self.project.scts[script].string_locations[string_id]
        self.project.scts[script].string_locations.pop(string_id)
        self.project.scts[script].string_groups[group].remove(string_id)

        self.callbacks['set_change'](script)

    def change_string_id(self, script, string_id, new_string_id):
        cur_script = self.project.scts[script]
        group = cur_script.string_locations[string_id]
        cur_script.string_groups[group].remove(string_id)
        cur_script.string_groups[group].append(new_string_id)
        cur_script.string_locations[new_string_id] = cur_script.string_locations.pop(string_id)
        cur_script.strings[new_string_id] = cur_script.strings.pop(string_id)
        for sect in cur_script.sects.values():
            for inst in sect.insts.values():
                if inst.base_id in (144, 24):
                    if inst.params[0].linked_string == string_id:
                        inst.params[0].linked_string = new_string_id
                if inst.base_id in (155, 25):
                    if inst.params[1].linked_string == string_id:
                        inst.params[1].linked_string = new_string_id

    # ----------------------------- #
    # Param Editor Callback Methods #
    # ----------------------------- #

    def get_jmp_section_list(self, script, section, first_only=False):
        dict_out = {}
        for sect in self.project.scts[script].sect_list:
            if sect == section and first_only:
                continue
            dict_out[sect] = {}
            cur_sect = self.project.scts[script].sects[sect]
            for i in cur_sect.inst_list:
                key = (f'{cur_sect.insts[i].ungrouped_position}{link_sep}'
                       f'{self.base_insts.get_inst(cur_sect.insts[i].base_id).name}{link_sep}'
                       f'{cur_sect.insts[i].base_id}')
                dict_out[sect][key] = i
                if first_only:
                    break
        return dict_out

    def get_first_inst(self, script, section):
        return self.project.scts[script].sects[section].inst_list[0]

    def refresh_links(self, script, section, instruction):
        inst = self.project.scts[script].sects[section].insts[instruction]
        for l in inst.links_out:
            if l.type in ['String', 'Footer']:
                continue
            target_inst = self.project.scts[script].sects[l.target_trace[0]].insts[l.target_trace[1]]
            if l in target_inst.links_in:
                target_inst.links_in.remove(l)
        inst.links_out.clear()
        for p in inst.params.values():
            if p.link is not None:
                if p.link.type in ['String', 'Footer']:
                    continue
                inst.links_out.append(p.link)
                target_inst = self.project.scts[script].sects[p.link.target_trace[0]].insts[p.link.target_trace[1]]
                target_inst.links_in.append(p.link)

    def get_jmp_inst_dict(self, script, section, goto_inst):
        dict_out = self.get_jmp_section_list(script, section, False)

        cur_sect = self.project.scts[script].sects[section]
        cur_inst = cur_sect.insts[goto_inst]
        inst_list = [_ for _ in cur_sect.inst_list if _ != goto_inst]
        if len(cur_inst.my_master_uuids) != 0:
            goto_master_uuid = cur_inst.my_master_uuids[0]
            inst_list = inst_list[:inst_list.index(goto_master_uuid)]

            parents, index = self.get_grouped_parents_and_index(goto_master_uuid, cur_sect.inst_tree)
            cur_group = cur_sect.inst_tree
            for p in parents:
                cur_group = cur_group[p]

            if index + 1 != len(cur_group):
                if goto_master_uuid == self.extract_parent_uuid_from_group(cur_group[index+1]):
                    index += 1

            if index + 1 != len(cur_group):
                for entry in cur_group[index+1:]:
                    inst_list.append(self.extract_parent_uuid_from_group(entry))

        dict_out[section] = {f'{cur_sect.insts[i].ungrouped_position}{link_sep}'
                f'{self.base_insts.get_inst(cur_sect.insts[i].base_id).name}'
                f'{link_sep}{cur_sect.insts[i].base_id}': f'{i}' for i in inst_list}

        return dict_out

    def get_string_options(self, script, is_footer=False):
        if is_footer:
            return {f'{footer_str_group_name}': self.project.scts[script].string_groups[footer_str_group_name]}
        else:
            return {k: v for k, v in self.project.scts[script].string_groups.items() if k != footer_str_group_name}

    def get_string_group(self, script, string):
        if string is None:
            return None
        if string not in self.project.scts[script].string_locations:
            return None
        return self.project.scts[script].string_locations[string]

    def check_param_editable(self, script, section, instruction, parameter):
        inst = self.project.scts[script].sects[section].insts[instruction]
        base_param = self.get_base_parameter(inst.base_id, parameter)
        if 'footer' in base_param.type and 'string' in base_param.type:
            if footer_str_group_name not in self.project.scts[script].string_groups:
                return f'No footer string group in the current script. _Footer_ group required'
            elif len(self.project.scts[script].string_groups[footer_str_group_name]) == 0:
                return f'No footer strings in _Footer_ group. Cannot select a string'
        return None

    def ensure_delay_param(self, script, section, instruction, new_param):
        inst = self.project.scts[script].sects[section].insts[instruction]
        if inst.delay_param is None:
            inst.delay_param = new_param

    def get_string_group_length(self, script, group_name):
        if group_name not in self.project.scts[script].string_groups:
            return None
        return len(self.project.scts[script].string_groups[group_name])

    def set_is_string_group(self, script, section, change):
        if change == 'add':
            self.project.scts[script].string_groups[section] = []
        elif change == 'remove':
            self.project.scts[script].string_groups.pop(section)

    # ----------------------- #
    # Script analysis methods #
    # ----------------------- #

    def check_script_is_in_project(self, name):
        return name.lower() in [_.lower() for _ in self.project.scts.keys()]

    # ------------------------ #
    # Section analysis methods #
    # ------------------------ #

    def sect_is_group(self, script, section):
        cur_group = self.project.scts[script].sect_tree

        parents, index = self.get_grouped_parents_and_index(section, cur_group)

        for p in parents:
            cur_group = cur_group[p]

        return isinstance(cur_group[index], dict)

    def get_new_sect_name(self, script):
        i = 0
        while self.is_sect_name_used(script, f'untitled{i}'):
            i += 1
        return f'untitled{i}'

    def is_sect_name_used(self, script, new_name, code_only=False):
        cur_script: SCTScript = self.project.scts[script]
        for sect in cur_script.sects.values():
            if sect.is_compound:
                for inst in sect.insts.values():
                    if inst.base_id == 9:
                        if inst.label == new_name:
                            return True
            else:
                if sect.name == new_name:
                    if code_only:
                        return len(sect.insts) > 1
                    return True
        if not code_only:
            if new_name in cur_script.strings.keys():
                return True
        return False

    def create_section(self, script, new_name='', location=None, insert_in_group=False, inst_list=None):
        if new_name == '':
            new_name = self.get_new_sect_name(script)

        cur_script = self.project.scts[script]
        new_sect = SCTSection()
        new_sect.name = new_name
        new_sect.type = 'Group'
        new_sect.absolute_offset = -1
        cur_script.sects[new_name] = new_sect

        inst_list = [] if inst_list is None else inst_list
        for inst in inst_list:
            new_inst_id = self.add_inst(script, new_name)
            self.change_inst(script, new_name, new_inst_id, new_id=inst)

        cur_script.sect_list.append(new_name)
        cur_script.sect_tree.append(new_name)

        if location is not None:
            self.move_items((new_name, new_name), location, insert_in_group, script)

        return new_name

    def assign_section_type(self, s, n):
        inst_list = self.project.scts[s].sects[n].inst_list
        if len(inst_list) == 0:
            self.project.scts[s].sects[n].type = 'Group'
        elif len(inst_list) == 1:
            self.project.scts[s].sects[n].type = 'Label'
        else:
            self.project.scts[s].sects[n].type = 'Script'

    def change_section_name(self, script, section, instruction, new_name):
        cur_script = self.project.scts[script]
        cur_section = cur_script.sects[section]

        old_sect_name = cur_section.name
        cur_section.name = new_name

        cur_script.sects[new_name] = cur_script.sects.pop(old_sect_name)

        # Change the name in the list and tree
        cur_script.sect_list[cur_script.sect_list.index(old_sect_name)] = new_name
        self._recursive_sect_name_replacer(cur_script.sect_tree, old_sect_name, new_name)

        # Change the name in other items
        if old_sect_name in cur_script.folded_sects:
            cur_script.folded_sects[new_name] = cur_script.folded_sects.pop(old_sect_name)

        if old_sect_name in cur_script.folded_sects.values():
            folded_keys = list(cur_script.folded_sects.keys())
            folded_ind = [i for i, x in enumerate(list(cur_script.folded_sects.values())) if x == old_sect_name]
            keys_to_change = [folded_keys[i] for i in folded_ind]
            for key in keys_to_change:
                cur_script.folded_sects[key] = new_name

        if old_sect_name in cur_script.unused_sections:
            cur_script.unused_sections[cur_script.unused_sections.index(old_sect_name)] = new_name

        for sect_list in cur_script.inst_locations:
            if old_sect_name in sect_list:
                sect_list[sect_list.index(old_sect_name)] = new_name

        for inst in cur_section.insts.values():
            for link in inst.links_in:
                link.target_trace[0] = new_name
            for link in inst.links_out:
                link.origin_trace[0] = new_name

        if len(cur_section.inst_list) == 0:
            return
        if instruction is None:
            instruction = cur_section.inst_list[0]
        cur_label = cur_section.insts[instruction]

        cur_label.label = new_name

    def _recursive_sect_name_replacer(self, tree, old, new):
        if isinstance(tree, list):
            if old in tree:
                tree[tree.index(old)] = new
                return True

            for entry in tree:
                if isinstance(entry, dict):
                    replaced = self._recursive_sect_name_replacer(entry, old, new)
                    if replaced:
                        return True

        if isinstance(tree, dict):
            if old in list(tree.keys())[0]:
                old_key = list(tree.keys())[0]
                new_key = f'{new}{old_key.replace(old, "").replace("(0)", "")}'
                tree[new_key] = tree.pop(old_key)
                return True

            for value in tree.values():
                replaced = self._recursive_sect_name_replacer(value, old, new)
                if replaced:
                    return True

    def check_for_compound_sect(self, script, section):
        sect = self.project.scts[script].sects[section]
        sect.is_compound = False
        inst_ind = 0
        while not sect.is_compound:
            inst_ind += 1
            if len(sect.inst_list) <= inst_ind:
                break
            if sect.insts[sect.inst_list[inst_ind]].base_id == 9:
                sect.is_compound = True

    # ------------------------------------------ #
    # Instruction and parameter analysis methods #
    # ------------------------------------------ #

    def get_parameter(self, script, section, instruction, parameter):
        instruction = self.project.scts[script].sects[section].insts[instruction]
        if parameter == 'delay':
            return instruction.delay_param
        if sep in parameter:
            param_parts = parameter.split(sep)
            return instruction.l_params[int(param_parts[0])][int(param_parts[1])]
        return instruction.params[int(parameter)]

    def get_inst_id(self, script, section, instruction, **kwargs):
        return self.project.scts[script].sects[section].insts[instruction].base_id

    def get_inst_id_name(self, inst_id):
        return self.base_insts.get_inst(inst_id).name

    def get_base_parameter(self, inst_id, param_str):
        if param_str == 'delay':
            return self.base_insts.get_inst(129).params[0]
        param_parts = param_str.split(sep)
        b_param_id = param_parts[-1]
        return self.base_insts.get_inst(inst_id).params[int(b_param_id)]

    def get_inst_rcm_restrictions(self, script, section, instruction, parent_is_case=None, **kwargs):
        cur_sect = self.project.scts[script].sects[section]
        inst_ind = cur_sect.inst_list.index(instruction)

        # Checks for initial label and return of a function
        if inst_ind == 0:
            if len(cur_sect.inst_list) > 1:
                return 'first'
            else:
                return ''

        if inst_ind + 1 == len(cur_sect.inst_list):
            if len(cur_sect.inst_list) > 2:
                return 'last'
            else:
                return ''

        # Checks if the instruction is a goto with a master and there is more than one inst in the group and its a case
        parents, index = self.get_grouped_parents_and_index(instruction, cur_sect.inst_tree)
        cur_group = cur_sect.inst_tree
        for p in parents:
            cur_group = cur_group[p]
        if len(cur_group) == 1 and parent_is_case:
            return ''
        elif len(cur_sect.insts[instruction].my_master_uuids) > 0:
            return 'goto'
        return ''

    def get_inst_desc_info(self, link: SCTLink):
        if link.target_trace is None:
            return f'Link has no target'
        if link.script not in self.project.scts:
            return f'Link has no section'
        if link.target_trace[0] not in self.project.scts[link.script].sects:
            return f'Link target section ({link.target_trace[0]}) not in script ({link.script})'
        if link.target_trace[1] is None:
            return 'Link target instruction was none'

        section = self.project.scts[link.script].sects[link.target_trace[0]]
        if link.target_trace[1] not in section.insts:
            return f'Link target instruction ({link.target_trace[1]}) not in section {section.name}'

        inst = section.insts[link.target_trace[1]]

        return f'{inst.ungrouped_position}{link_sep}' \
               f'{self.base_insts.get_inst(inst.base_id).name}{link_sep}' \
               f'{inst.base_id}'

    def has_loops(self, script, section, instruction, **kwargs):
        cur_inst_id = self.project.scts[script].sects[section].insts[instruction].base_id
        return self.base_insts.get_inst(cur_inst_id).loop is not None

    def inst_is_switch(self, script, section, instruction, **kwargs):
        return self.project.scts[script].sects[section].insts[instruction].base_id == 3

    def inst_is_group(self, script, section, instruction, **kwargs):
        return self.project.scts[script].sects[section].insts[instruction].base_id in (0, 3)

    def group_is_empty(self, script, section, instruction, **kwargs):
        cur_sect = self.project.scts[script].sects[section]
        parents, index = self.get_grouped_parents_and_index(instruction, cur_sect.inst_tree)
        cur_group = cur_sect.inst_tree
        for p in parents:
            cur_group = cur_group[p]
        if not isinstance(cur_group[index], dict):
            return True
        k, v = list(cur_group[index].items())[0]
        if 'switch' in k:
            for k1, v1 in v.items():
                if len(v) == 1:
                    if v[0] not in cur_sect.insts[instruction].my_goto_uuids:
                        return False
                elif len(v1) > 0:
                    return False
        elif len(v) == 1:
            if v[0] not in cur_sect.insts[instruction].my_goto_uuids:
                return False
        elif len(v) > 0:
            return False
        return True

    def inst_is_label(self, script, section, instruction):
        return self.project.scts[script].sects[section].insts[instruction].base_id == 9

    def get_inst_offset(self, script, section, instruction, **kwargs):
        return self.project.scts[script].sects[section].insts[instruction].absolute_offset

    # ------------------------ #
    # Group based manipulation #
    # ------------------------ #

    def move_items(self, sel_bounds, insert_after, insert_in_group, script, section=None):
        if section is None:
            base_group = self.project.scts[script].sect_tree
            cur_list = self.project.scts[script].sect_list
        else:
            base_group = self.project.scts[script].sects[section].inst_tree
            cur_list = self.project.scts[script].sects[section].inst_list

        first_uuid, last_uuid = sel_bounds
        if sep in first_uuid:
            first_uuid, first_case = first_uuid.split(sep)
            return self.move_switch_case(script, section, first_uuid, first_case, insert_after)

        f_parents, f_index = self.get_grouped_parents_and_index(first_uuid, base_group)
        l_parents, l_index = self.get_grouped_parents_and_index(last_uuid, base_group)

        if len(f_parents) > len(l_parents):
            print(f'{self.log_key}: Unable to move group, first parent list is larger than last')
            return

        for i in range(len(f_parents)):
            if f_parents[i] != l_parents[i]:
                print(f'{self.log_key}: Unable to move group, parent lists are different')
                return

        if len(l_parents) > len(f_parents):
            test_uuid = None
            while len(l_parents) > len(f_parents) + 1:
                test_uuid = l_parents.pop(-1)
            _, l_index = self.get_grouped_parents_and_index(test_uuid, base_group)

        # Handle tree first

        cur_group = base_group
        for p in f_parents:
            cur_group = cur_group[p]
        moved_group = cur_group[f_index: l_index+1]
        for i in reversed(range(f_index, l_index+1)):
            cur_group.pop(i)

        insert_case = None
        if sep in insert_after:
            insert_after, insert_case = insert_after.split(sep)

        i_parents, i_index = self.get_grouped_parents_and_index(insert_after, base_group)

        for p in i_parents:
            base_group = base_group[p]

        # Modify the correct insertion point depending on whether to insert in group or not
        if section is None:
            is_group = self.sect_is_group(script, insert_after)
        else:
            is_group = self.inst_is_group(script, section, insert_after)

        if is_group:
            if insert_in_group:
                base_group = base_group[i_index]
                base_group = base_group[list(base_group.keys())[0]]
                if insert_case is not None:
                    base_group = base_group[insert_case]
                i_index = -1
            else:
                insert_after = self.get_inst_uuid_from_group_entry(base_group[i_index], last=True)

        for g in reversed(moved_group):
            base_group.insert(i_index+1, g)

        # Handle list version
        f_inst_list_ind = cur_list.index(first_uuid)
        l_inst_list_ind = cur_list.index(last_uuid)
        sel_elements = cur_list[f_inst_list_ind: l_inst_list_ind + 1]
        temp_after = cur_list[l_inst_list_ind+1:] if l_inst_list_ind + 1 != len(cur_list) else []

        old_tgt_uuid = temp_after[0] if len(temp_after) > 0 else None
        temp_list = cur_list[:f_inst_list_ind] + temp_after
        insert_ind = temp_list.index(insert_after) + 1
        insert_after = temp_list[insert_ind:]

        new_tgt_uuid = insert_after[0] if len(insert_after) > 0 else None
        temp_list = temp_list[:insert_ind] + sel_elements + insert_after

        if section is None:
            self.project.scts[script].sect_list = temp_list
        else:
            self.project.scts[script].sects[section].inst_list = temp_list

        self.callbacks['set_change']()

        if section is None:
            return
        # if the group moved is an instruction group...
        self._refresh_inst_positions(script=script, section=section)

        cur_sect = self.project.scts[script].sects[section]

        # break prev links:
        for link in cur_sect.insts[first_uuid].links_in:
            self.change_link_tgt(cur_sect, link, old_tgt_uuid, remove_from_old_tgt=False)
        cur_sect.insts[first_uuid].links_in = []

        # Jump links to inst after group
        for link in cur_sect.insts[new_tgt_uuid].links_in:
            self.change_link_tgt(cur_sect, link, first_uuid, remove_from_old_tgt=False)
        cur_sect.insts[new_tgt_uuid].links_in = []

        changed_links = []
        for link in cur_sect.insts[old_tgt_uuid].links_in:
            if link.origin_trace[1] in sel_elements:
                changed_links.append(link)
                self.change_link_tgt(cur_sect, link, new_tgt_uuid, remove_from_old_tgt=False)
        for link in changed_links:
            cur_sect.insts[old_tgt_uuid].links_in.remove(link)

    def move_switch_case(self, script, section, switch_uuid, case, insert_after):
        cur_sect = self.project.scts[script].sects[section]
        cur_group = cur_sect.inst_tree
        cur_inst = cur_sect.insts[switch_uuid]
        inst_gotos = [goto for goto in cur_inst.my_goto_uuids]
        goto_poses = [cur_sect.inst_list.index(goto) for goto in inst_gotos]
        goto_poses, inst_gotos = zip(*sorted(zip(goto_poses, inst_gotos)))

        parents, index = self.get_grouped_parents_and_index(switch_uuid, cur_group)

        for parent in parents:
            cur_group = cur_group[parent]

        switch_group = cur_group[index][f'{switch_uuid}{sep}switch']
        case_group = switch_group[case]
        case_goto = cur_sect.insts[self.get_inst_uuid_from_group_entry(case_group, last=True)]
        case_ind = inst_gotos.index(case_goto.ID)

        if insert_after == switch_uuid:
            insert_ind = 0
        else:
            insert_after_case = insert_after.split(sep)[1]
            insert_after_goto = self.get_inst_uuid_from_group_entry(switch_group[insert_after_case], last=True)
            if insert_after_goto not in inst_gotos:
                insert_ind = len(inst_gotos) - 1
                insert_after = inst_gotos[-1]
            else:
                insert_ind = inst_gotos.index(insert_after_goto) + 1
                insert_after = insert_after_goto

        if insert_ind == case_ind:
            return

        # handle inst list move
        case_first_uuid = self.get_inst_uuid_from_group_entry(case_group[0])
        first_ind = cur_sect.inst_list.index(case_first_uuid)
        last_ind = cur_sect.inst_list.index(case_goto.ID)
        case_list = cur_sect.inst_list[first_ind:last_ind + 1]
        temp_list = cur_sect.inst_list[:first_ind] + cur_sect.inst_list[last_ind + 1:]
        list_insert_ind = temp_list.index(insert_after) + 1
        temp_list_after = temp_list[list_insert_ind + 1:] if list_insert_ind + 1 != len(temp_list) else []
        temp_list = temp_list[:list_insert_ind + 1] + case_list + temp_list_after
        cur_sect.inst_list = temp_list

        # handle inst tree move and switch param move
        switch_inst = cur_sect.insts[switch_uuid]
        case_entry = {case: switch_group[case]}
        new_cases = {}

        switch_loops = switch_inst.l_params
        case_loop = switch_loops[case_ind]
        new_l_params = []
        for i, l in enumerate(switch_loops):
            if i == insert_ind:
                new_cases |= case_entry
                new_l_params.append(case_loop)
            k = str(l[2].value)
            if k == case:
                continue
            new_cases |= {k: switch_group[k]}
            new_l_params.append(l)
        cur_group[index] = {f'{switch_uuid}{sep}switch': new_cases}
        switch_inst.l_params = new_l_params

        self.callbacks['set_change']()

    # ---------------------------- #
    # Section manipulation methods #
    # ---------------------------- #

    def add_section(self, script, relevant_sect, is_above):
        cur_script = self.project.scts[script]
        new_sect = SCTSection()
        new_sect.type = 'Label'
        i = 0
        new_name = f'Untitled({i})'
        while self.is_sect_name_used(script, new_name):
            i += 1
            new_name = f'Untitled({i})'
        new_sect.name = new_name
        cur_script.sects[new_name] = new_sect

        index = cur_script.sect_list.index(relevant_sect)
        if not is_above:
            index += 1

        cur_script.sect_list.insert(index, new_name)
        parents, index = self.get_grouped_parents_and_index(relevant_sect, cur_script.sect_tree)

        if not is_above:
            index += 1

        cur_group = cur_script.sect_tree
        for p in parents:
            cur_group = cur_group[p]
        cur_group.insert(index, new_name)

        cur_script.unused_sections.append(new_name)

        return new_name

    def remove_section(self, script, section):
        # This method is not used for string group sections
        cur_script = self.project.scts[script]
        cur_script.sect_list.remove(section)

        parents, index = self.get_grouped_parents_and_index(section, cur_script.sect_tree)
        cur_group = cur_script.sect_tree
        for p in parents:
            cur_group = cur_group[p]
        cur_group.remove(section)

        if section in cur_script.unused_sections:
            cur_script.unused_sections.remove(section)

        if cur_script.sects[section].is_compound:
            cur_script.folded_sects = {k: v for k, v in cur_script.folded_sects.items() if v != section}

        if section in cur_script.error_sections:
            cur_script.error_sections.pop(section)

        cur_sect = cur_script.sects[section]
        links_in = cur_sect.insts[cur_sect.inst_list[0]].links_in
        for link in links_in:
            o_t = link.origin_trace
            o_inst = cur_script.sects[o_t[0]].insts[o_t[1]]
            o_inst.links_out.remove(link)
            o_inst.params[0].link = None

        links_out = []
        for inst in cur_sect.insts.values():
            for link in inst.links_out:
                if link.target_trace is None:
                    continue
                if link.target_trace[0] != section:
                    links_out.append(link)
        for link in links_out:
            t_t = link.target_trace
            cur_script.sects[t_t[0]].insts[t_t[1]].links_in.remove(link)

        cur_script.sects.pop(section)
        self.callbacks['set_change'](script)

    def group_sections(self, script, section_bounds):
        cur_script = self.project.scts[script]
        parents_s, index_s = self.get_grouped_parents_and_index(section_bounds[0], cur_script.sect_tree)
        parents_e, index_e = self.get_grouped_parents_and_index(section_bounds[1], cur_script.sect_tree)

        different_sizes = False
        end_is_longer = False
        if len(parents_s) != len(parents_e):
            different_sizes = True
            end_is_longer = parents_s > parents_e

        parents = parents_s
        other_parents = parents_e
        if different_sizes and end_is_longer:
            parents = parents_e
            other_parents = parents_s

        latest_same = -1
        for i, p in enumerate(parents):
            if p != other_parents[i]:
                break
            else:
                latest_same = i
        same_prefix = latest_same + 1 == len(parents)

        if same_prefix:
            if different_sizes:
                if end_is_longer:
                    index_e = index_s + 1
                else:
                    parents_s = parents_e
                    index_s = index_e - 1
        else:
            if latest_same == -1:
                index_s = parents_s[0]
                index_e = parents_e[0]
                parents_s = []
            else:
                index_s = parents_s[latest_same+1]
                index_e = parents_e[latest_same+1]
                parents_s = parents_s[:latest_same+1]

        cur_group = cur_script.sect_tree
        for p in parents_s:
            cur_group = cur_group[p]

        if index_e >= len(cur_group) or index_s < 0 or index_s == index_e:
            return

        if isinstance(cur_group[index_s], dict):
            return

        new_group = {f'{cur_group[index_s]}{sep}group': [_ for _ in cur_group[index_s+1:index_e+1]]}
        for i in reversed(range(index_s, index_e+1)):
            cur_group.pop(i)
        cur_group.insert(index_s, new_group)

        self.callbacks['set_change']()

    def ungroup_section(self, script, section):
        cur_script = self.project.scts[script]
        parents, index = self.get_grouped_parents_and_index(section, cur_script.sect_tree)
        cur_group = cur_script.sect_tree
        for p in parents:
            cur_group = cur_group[p]
        group = cur_group.pop(index)
        group = [list(group.keys())[0].split(sep)[0]] + list(group.values())[0]
        for entry in reversed(group):
            cur_group.insert(index, entry)

        self.callbacks['set_change']()

    def change_section_type(self, script, section, s_type: Literal['virtual', 'label', 'code']):
        cur_sect = self.project.scts[script].sects[section]

        if len(cur_sect.insts) == 0:
            cur_type = 'virtual'
        elif len(cur_sect.insts) == 1:
            cur_type = 'label'
        else:
            cur_type = 'code'

        if s_type == cur_type:
            return

        if s_type == 'virtual':
            cur_sect.inst_list = []
            cur_sect.inst_tree = []
            cur_sect.insts = {}
            cur_sect.set_type('Label')

        elif cur_type == 'code':
            cur_sect.inst_errors = []
            cur_sect.inst_list = [cur_sect.inst_list[0]]
            cur_sect.inst_tree = [cur_sect.inst_tree[0]]
            cur_sect.insts = {cur_sect.inst_list[0]: cur_sect.insts[cur_sect.inst_list[0]]}
            cur_sect.is_compound = False
            cur_sect.set_type('Label')

        elif cur_type == 'virtual':
            new_inst_UUID = self.add_inst(script, section)
            self.change_inst(script, section, new_inst_UUID, new_id=9)
            cur_sect.insts[new_inst_UUID].label = cur_sect.name

        if s_type == 'code':
            new_inst_UUID = self.add_inst(script, section, cur_sect.inst_tree[0])
            self.change_inst(script, section, new_inst_UUID, new_id=12)
            cur_sect.set_type('SCT')

        self.callbacks['set_change']()

    # ---------------------------------------------- #
    # Instruction and parameter manipulation methods #
    # ---------------------------------------------- #

    def add_switch_case(self, script, section, instruction, **kwargs):
        cur_sect = self.project.scts[script].sects[section]
        parent_list, index = self.get_grouped_parents_and_index(grouped_region=cur_sect.inst_tree, element=instruction)
        cases = [int(loop[2].value) for loop in cur_sect.insts[instruction].l_params]
        next_case = -1
        while next_case in cases:
            next_case += 1
        self.add_inst_sub_group(script, section, instruction, parent_list, index, str(next_case))
        self.update_loop_param_num(cur_sect.insts[instruction])

        self.callbacks['set_change']()

    def remove_switch_case(self, script, section, instruction, case, result, **kwargs):
        return self.change_inst(script, section, instruction, case=case, change_type=result)

    def add_loop_param(self, script, section, instruction, position=None, **kwargs):
        inst = self.project.scts[script].sects[section].insts[instruction]
        loop = {}
        for param_id in self.base_insts.get_inst(inst.base_id).loop:
            base_param = self.base_insts.get_inst(inst.base_id).params[param_id]
            loop_param = SCTParameter(param_id, base_param.type)
            loop_param.set_value(base_param.default_value)
            loop[int(param_id)] = loop_param
        pos = 0 if position is None else position
        if position == -1:
            pos = len(inst.l_params)
        inst.l_params.insert(pos, loop)
        self.update_loop_param_num(inst)
        return True

    def remove_loop_param(self, script, section, instruction, loop_num, **kwargs):
        inst = self.project.scts[script].sects[section].insts[instruction]
        if len(inst.l_params) <= loop_num:
            return False
        inst.l_params.pop(loop_num)
        self.update_loop_param_num(inst)
        return True

    def update_loop_param_num(self, inst):
        for key, param in self.base_insts.get_inst(inst.base_id).params.items():
            if loop_count_name in param.type:
                inst.params[int(key)].set_value(len(inst.l_params))

    def remove_inst(self, script, section, inst, result, custom_link_tgt=None):
        self.remove_inst_links(script, section, inst, custom_tgt=custom_link_tgt)
        # This will handle inst group children, remove any inst links in the group
        # and remove the inst from the grouped representation of insts
        self.change_inst(script, section, inst, change_type=result)
        cur_sect = self.project.scts[script].sects[section]
        inst_is_label = cur_sect.insts[inst].base_id == 9
        cur_sect.insts.pop(inst)
        if inst in cur_sect.inst_list:
            cur_sect.inst_list.remove(inst)
            self._refresh_inst_positions(script, section)

        if inst_is_label and cur_sect.is_compound:
            self.check_for_compound_sect(script, section)

        self.assign_section_type(script, section)

    def add_inst(self, script, section, ref_inst_uuid=None, case=None, direction='below'):
        new_inst = SCTInstruction()
        inst_sect = self.project.scts[script].sects[section]
        inst_sect.insts[new_inst.ID] = new_inst

        if ref_inst_uuid is not None:
            inst_parents, index = self.get_grouped_parents_and_index(ref_inst_uuid, inst_sect.inst_tree)
        else:
            inst_parents, index = [], -1

        cur_group = inst_sect.inst_tree
        for key in inst_parents:
            cur_group = cur_group[key]

        grouped_insert_loc = index

        if direction == 'below':
            grouped_insert_loc += 1

        if direction == 'inside':
            grouped_insert_loc = 0
            cur_group = cur_group[index][list(cur_group[index].keys())[0]]
            index = 0
            if case is not None:
                cur_group = cur_group[case]

        steal_links_from_uuid = None
        if len(cur_group) > 0:
            steal_links_from_uuid = self.get_inst_uuid_from_group_entry(cur_group[index])

        cur_group.insert(grouped_insert_loc, new_inst.ID)

        if ref_inst_uuid is not None:
            ungroup_ref_inst_uuid = ref_inst_uuid

            self.inst_specific_setup(script, section, new_inst)

            if direction == 'below' and isinstance(cur_group[index], dict):
                # The ref inst for below is the last instruction of the group this should almost always be a goto
                ungroup_ref_inst_uuid = self.get_inst_uuid_from_group_entry(cur_group[index], last=True)

            insert_pos = inst_sect.inst_list.index(ungroup_ref_inst_uuid)
            if direction in ('inside', 'below'):
                insert_pos += 1
        else:
            insert_pos = -1
        inst_sect.inst_list.insert(insert_pos, new_inst.ID)

        # if new inst is inside, and clicked is a switch case, change target of link of case to new inst
        if case is not None:
            switch = inst_sect.insts[ref_inst_uuid]
            for loop in switch.l_params:
                if loop[2].value == int(case):
                    loop[3].link.target_trace[1] = new_inst.ID
                    break

        if steal_links_from_uuid is not None:
            remove_direction: Literal['in', 'out'] = 'in'
            if direction == 'below':
                remove_direction = 'out'
            self.remove_inst_links(script=script, section=section, inst=steal_links_from_uuid, custom_tgt=new_inst.ID, direction=remove_direction)

        self.assign_section_type(script, section)

        return new_inst.ID

    def change_inst(self, script, section, inst, new_id=None, case=None, change_type=None):
        # not entering a new_id will remove the instruction
        cur_section = self.project.scts[script].sects[section]
        cur_inst = cur_section.insts[inst]
        if new_id is not None:
            if cur_inst.base_id == new_id:
                return True

        saved_children = {}
        changes = []
        if cur_inst.base_id in self.base_insts.group_inst_list:
            if change_type is None:
                raise ValueError('Current inst is a group type, but no group resolution was given')

            new_id = None if new_id is None else int(new_id)

            custom_link_tgt = self.get_next_grouped_uuid(script, section, inst)
            for goto in cur_inst.my_goto_uuids:
                if case is not None:
                    parents, index = self.get_grouped_parents_and_index(goto, cur_section.inst_tree)
                    if parents[-1] != case:
                        continue

                if goto in cur_section.insts:
                    self.remove_inst(script, section, goto, custom_link_tgt=custom_link_tgt, result=None)

            inst_group = self.get_inst_group(script, section, inst)
            if cur_inst.base_id == 3:
                inst_group = inst_group if case is None else {case: inst_group[f'{inst}{sep}switch'][case]}

            if case is None:
                self.remove_inst_links(script, section, inst, custom_tgt=custom_link_tgt)
            else:
                loop_param_to_remove = None
                for i, loop in enumerate(cur_inst.l_params):
                    if int(case) == loop[2].value:
                        loop_param_to_remove = i
                        break
                if loop_param_to_remove is None:
                    print(f'{self.log_key}: Could not find loop parameter for {case}')
                cur_inst.links_out.remove(cur_inst.l_params[loop_param_to_remove][3].link)
                cur_inst.l_params.pop(loop_param_to_remove)

            changes: list = change_type.split(alt_alt_sep)
            changes.reverse()

            finished_change_indexes = []
            for i, change in enumerate(changes):
                change_parts = change.split(alt_sep)
                cur_group = None
                for key, group in inst_group.items():
                    if key == change_parts[0]:
                        cur_group = group
                        break

                if cur_group is None:
                    print(f'{self.log_key}: Unable to find group for processing: {change_parts[0]}')

                if len(cur_group) == 0:
                    continue

                if 'Move' in change or 'Delete' in change:
                    self.perform_group_change(script, section, inst, cur_group, change)
                    finished_change_indexes.append(i)
                elif 'Insert' in change:
                    self.perform_group_change(script, section, inst, cur_group, change)
                    saved_children[change_parts[0]] = copy.deepcopy(cur_group)
                else:
                    print(f'{self.log_key}: Unknown group change instruction: {change}')

            for i in reversed(finished_change_indexes):
                changes.pop(i)

        parent_list, index = self.get_grouped_parents_and_index(inst, cur_section.inst_tree)

        if index is None:
            print(f'{self.log_key}: instruction not found in grouped instructions...')
            return False

        cur_group = cur_section.inst_tree
        for parent in parent_list:
            cur_group = cur_group[parent]

        if case is not None:
            cur_group[index][f'{inst}{sep}switch'].pop(case)
            return True

        if new_id is None:
            cur_group.pop(index)
            if not index >= len(cur_group):
                if isinstance(cur_group[index], dict):
                    if f'{inst}{sep}else' in cur_group[index]:
                        cur_group.pop(index)
            return True

        if cur_inst.base_id in self.base_insts.group_inst_list:
            cur_group[index] = inst

        # Change inst id
        old_id = cur_inst.base_id
        cur_inst.set_inst_id(int(new_id))
        if old_id == 9 or new_id == '9':
            self.check_for_compound_sect(script, section)

        # Remove any current parameters and loop parameters
        self.remove_inst_parameters(script, section, inst)

        # Add in default parameter values with no loops
        base_inst = self.base_insts.get_inst(int(new_id))

        for i in [*base_inst.params_before, *base_inst.params_after]:
            base_param = base_inst.params[i]
            new_param = SCTParameter(base_param.param_ID, base_param.type)
            if base_param.default_value == override_str:
                new_param.set_value(override_str, get_scpt_override(base_param.type))
            else:
                new_param.set_value(base_param.default_value)
            cur_inst.params[i] = new_param

        self.inst_specific_setup(script, section, cur_inst)

        if int(new_id) in self.base_insts.group_inst_list:
            self.setup_group_type_inst(script, section, inst, cur_inst, parent_list, index)

            # add necessary groups to insert insts into
            for change in changes:
                if 'If' in change:
                    continue
                sub_group = change.split(' ')[-1].lower()
                self.add_inst_sub_group(script, section, inst, parent_list, index, sub_group)

            # Insert insts into groups
            for change in changes:
                # insert inst list into appropriate place in ungrouped
                temp_cur_group = cur_group
                if 'If' in change:
                    ungrouped_index = cur_section.inst_list.index(inst) + 1
                    temp_cur_group = temp_cur_group[index][f'{inst}{sep}if']
                    cur_first_uuid = self.get_inst_uuid_from_group_entry(temp_cur_group[0])
                elif 'Else' in change:
                    goto_inst = cur_section.insts[cur_inst.my_goto_uuids[0]]
                    ungrouped_index = cur_section.inst_list.index(goto_inst.ID) + 1
                    temp_cur_group = temp_cur_group[index + 1][f'{inst}{sep}else']
                else:
                    sub_group = change.split(' ')[-1]
                    temp_cur_group = temp_cur_group[index][f'{inst}{sep}switch'][sub_group]
                    first_uuid = self.get_inst_uuid_from_group_entry(temp_cur_group)
                    ungrouped_index = cur_section.inst_list.index(first_uuid)
                    cur_first_uuid = self.get_inst_uuid_from_group_entry(saved_children[change.split(alt_sep)[0]])

                # Insert saved group into appropriate place in ungrouped
                inst_list = self.extract_inst_uuids_from_group(saved_children[change.split(alt_sep)[0]])
                cur_section.inst_list = cur_section.inst_list[:ungrouped_index] + inst_list + cur_section.inst_list[ungrouped_index:]

                # insert saved_group into appropriate place in grouped
                for item in reversed(saved_children[change.split(alt_sep)[0]]):
                    temp_cur_group.insert(0, item)

                # Adjust if or switch for new linked inst values
                if 'If' in change:
                    next_inst_ind = cur_section.inst_list.index(cur_first_uuid)
                    prev_inst_uuid = cur_section.inst_list[next_inst_ind - 1]
                    prev_inst = cur_section.insts[prev_inst_uuid]
                    # Skip if it is not a goto
                    if not prev_inst.base_id == 10:
                        continue
                    # Skip if the target is already correct
                    prev_tgt = prev_inst.links_out[0].target_trace[1]
                    if prev_tgt == cur_first_uuid:
                        continue
                    if len(prev_inst.my_master_uuids) != 0:
                        prev_master = cur_section.insts[prev_inst.my_master_uuids[0]]
                        # Skip if If is a While
                        if cur_section.inst_list.index(prev_tgt) > cur_section.inst_list.index(prev_master.ID):
                            self.change_link_tgt(tgt_sect=cur_section, link=prev_inst.links_out[0],
                                                 new_tgt_uuid=cur_first_uuid)
                    else:
                        self.change_link_tgt(tgt_sect=cur_section, link=prev_inst.links_out[0],
                                             new_tgt_uuid=cur_first_uuid)
                        continue
                    # Skip if prev has no master, master can only be If or Switch
                    # Skip prev master is switch. prev_inst will not be a goto if an If with Else
                    if prev_master.base_id == 3:
                        continue
                    self.change_link_tgt(tgt_sect=cur_section, link=prev_master.links_out[0], new_tgt_uuid=cur_first_uuid)

                elif 'Else' in change:
                    self.change_link_tgt(tgt_sect=cur_section, link=cur_inst.links_out[0], new_tgt_uuid=inst_list[0])

                else:
                    case_param = None
                    for loop in cur_inst.l_params:
                        if loop[2].value == int(sub_group):
                            case_param = loop[3]
                            break
                    if case_param is None:
                        print(f'{self.log_key}: Unable to find switch case for link change')
                    self.change_link_tgt(tgt_sect=cur_section, link=case_param.link, new_tgt_uuid=cur_first_uuid)

                    next_key = self.get_inst_uuid_from_group_entry(cur_group[index + 1])
                    if inst in next_key:
                        cur_group.pop(index + 1)

        cur_inst.generate_condition(self.get_script_variables_with_aliases(script))
        self._refresh_inst_positions(script, section)

        self.callbacks['set_change']()

        return True

    def remove_inst_parameters(self, script, section, inst, loop=True):
        cur_inst = self.project.scts[script].sects[section].insts[inst]
        cur_inst.params = {}
        if not loop:
            return
        cur_inst.l_params = []

    def set_encode_flag(self, script, section, instruction, setting):
        self.project.scts[script].sects[section].insts[instruction].encode_inst = setting

    # ------------------------------- #
    # Inst group manipulation methods #
    # ------------------------------- #

    def setup_group_type_inst(self, script, section, inst_id, inst, parent_list, index):

        inst_sect = self.project.scts[script].sects[section]

        cur_group = inst_sect.inst_tree
        for key in parent_list:
            cur_group = cur_group[key]

        if inst.base_id == 0:
            target_inst_UUID_index = inst_sect.inst_list.index(inst_id) + 1
            target_inst_UUID = inst_sect.inst_list[target_inst_UUID_index]

            inst_link = SCTLink(origin=-1, origin_trace=[section, inst.ID, 1], type='Jump',
                                target=-1, target_trace=[section, target_inst_UUID], script=script)
            inst.params[1].link = inst_link
            inst.links_out.append(inst_link)
            inst_sect.insts[target_inst_UUID].links_in.append(inst_link)

            goto_inst = SCTInstruction()
            goto_inst.set_inst_id(10)
            goto_param = SCTParameter(0, 'int|jump')
            goto_inst.add_parameter(0, goto_param)
            goto_link = SCTLink(origin=-1, origin_trace=[section, goto_inst.ID, 0], type='Jump',
                                target=-1, target_trace=[section, target_inst_UUID], script=script)
            goto_param.link = goto_link
            inst_sect.insts[target_inst_UUID].links_in.append(goto_link)
            goto_inst.links_out.append(goto_link)

            inst.my_goto_uuids = [goto_inst.ID]
            goto_inst.my_master_uuids = [inst.ID]

            inst_sect.insts[goto_inst.ID] = goto_inst

            inst_sect.inst_list.insert(target_inst_UUID_index, goto_inst.ID)

            grouped_insertion = {f'{inst_id}{sep}if': [goto_inst.ID]}

        elif inst.base_id == 3:
            grouped_insertion = {f'{inst_id}{sep}switch': {}}
            inst.my_goto_uuids = []
        else:
            print(
                f'{self.log_key}: Setup Group Type Inst: Unknown group type inst ({inst.base_id}). Group not made.')
            grouped_insertion = inst_id

        cur_group.pop(index)
        cur_group.insert(index, grouped_insertion)

    def perform_group_change(self, script, section, inst, cur_group, change):
        # Handles Move Above, Move Below, Delete
        cur_sect = self.project.scts[script].sects[section]

        start_inst_key, end_inst_key = self.get_inst_group_bounds(cur_group)
        if start_inst_key is None:
            return

        # Remove Instructions from current place in ungrouped in new temp ungrouped
        start_inst_uuid = start_inst_key if sep not in start_inst_key else start_inst_key.split(sep)[0]
        end_inst_uuid = end_inst_key if sep not in end_inst_key else end_inst_key.split(sep)[0]
        start_ind = cur_sect.inst_list.index(start_inst_uuid)
        end_ind = cur_sect.inst_list.index(end_inst_uuid)
        group_insts = cur_sect.inst_list[start_ind: end_ind + 1]
        temp_ungrouped = []
        temp_ungrouped += cur_sect.inst_list[:start_ind] if start_ind > 0 else []
        temp_ungrouped += cur_sect.inst_list[end_ind + 1:] if end_ind < len(
            cur_sect.inst_list) - 1 else []

        # Remove instructions from current place in grouped in new temp grouped
        parents, grouped_ind = self.get_grouped_parents_and_index(inst, cur_sect.inst_tree)

        cur_temp_group = cur_sect.inst_tree
        for parent in parents:
            cur_temp_group = cur_temp_group[parent]

        if "Delete" in change:
            for del_inst in reversed(group_insts):
                links_removed = False
                if len(cur_sect.insts[del_inst].links_in) > 0:
                    for link in cur_sect.insts[del_inst].links_in:
                        if link.origin_trace is None:
                            continue
                        if link.origin_trace[1] in temp_ungrouped:
                            links_removed = True
                            self.remove_inst_links(script, section, del_inst)
                if not links_removed and len(cur_sect.insts[del_inst].links_out) > 0:
                    for link in cur_sect.insts[del_inst].links_out:
                        if link.target_trace is None:
                            continue
                        if link.target_trace[1] in temp_ungrouped:
                            self.remove_inst_links(script, section, del_inst)
            new_insts = {k: cur_sect.insts[k] for k in cur_sect.insts if k not in group_insts}
            cur_sect.insts = new_insts
            new_ungrouped = temp_ungrouped

        elif 'Move' in change:
            grouped_insert_loc = grouped_ind

            if 'Below' in change:
                grouped_insert_loc += 1

            ungroup_ref_inst_uuid = self.get_inst_uuid_from_group_entry(cur_temp_group[grouped_insert_loc])
            ungrouped_insert_ind = temp_ungrouped.index(ungroup_ref_inst_uuid)

            ungrouped_before = temp_ungrouped[:ungrouped_insert_ind]
            ungrouped_after = temp_ungrouped[ungrouped_insert_ind:]

            new_ungrouped = ungrouped_before + group_insts + ungrouped_after
            for entry in reversed(cur_group):
                cur_temp_group.insert(grouped_insert_loc, entry)

            new_tgt_uuid = inst
            if 'Below' in change:
                new_tgt_uuid = ungrouped_after[0]
            prev_inst_uuid = new_ungrouped[new_ungrouped.index(new_tgt_uuid) - 1]
            prev_inst = cur_sect.insts[prev_inst_uuid]
            # Skip if it is not a goto
            if prev_inst.base_id == 10:
                # Skip if the target is already correct
                prev_tgt = prev_inst.links_out[0].target_trace[1]
                if prev_tgt == new_tgt_uuid:
                    pass
                elif len(prev_inst.my_master_uuids) != 0:
                    prev_master = cur_sect.insts[prev_inst.my_master_uuids[0]]
                    # Skip if If is a While
                    if cur_sect.inst_list.index(prev_tgt) > cur_sect.inst_list.index(
                            prev_master.ID):
                        self.change_link_tgt(tgt_sect=cur_sect, link=prev_inst.links_out[0],
                                             new_tgt_uuid=new_tgt_uuid)
                    # Skip if prev has no master, master can only be If or Switch
                    # Skip prev master is switch. prev_inst will not be a goto if an If with Else
                    if prev_master.base_id != 3:
                        self.change_link_tgt(tgt_sect=cur_sect, link=prev_master.links_out[0], new_tgt_uuid=new_tgt_uuid)
                else:
                    self.change_link_tgt(tgt_sect=cur_sect, link=prev_inst.links_out[0],
                                         new_tgt_uuid=new_tgt_uuid)

        elif 'Insert' in change:
            new_ungrouped = temp_ungrouped

        else:
            print(f'{self.log_key}: Unknown change request: {change}')
            return

        cur_sect.inst_list = new_ungrouped

    def add_inst_sub_group(self, script, section, inst, parent_list, index, sub_group):
        cur_sect = self.project.scts[script].sects[section]
        cur_group = cur_sect.inst_tree
        cur_inst = cur_sect.insts[inst]
        for parent in parent_list:
            cur_group = cur_group[parent]

        test_group = cur_group
        if cur_inst.base_id == 3:
            test_group = test_group[index]
            test_group = test_group[list(test_group.keys())[0]]

        if cur_inst.base_id == 0:
            sub_group = f'{inst}{sep}{sub_group}'

        if self._inst_sub_group_present(test_group, sub_group):
            return

        if 'else' in sub_group:
            cur_group.insert(index + 1, {sub_group: []})
            return

        # The remainder is for adding a case to the switch
        goto_target_uuid = self.get_next_grouped_uuid(script=script, section=section, inst=inst)

        for link in cur_inst.links_out:
            loop_id = int(link.origin_trace[2].split(sep)[0])
            link.origin_trace[2] = f'{loop_id+1}{sep}3'

        case_goto = SCTInstruction()
        case_goto.my_master_uuids.append(inst)
        cur_inst.my_goto_uuids.append(case_goto.ID)

        cur_sect.insts[case_goto.ID] = case_goto
        case_goto.set_inst_id(10)
        case_goto.params[0] = SCTParameter(0, 'int|jump')
        case_goto.params[0].link = SCTLink('Jump', origin=-1, origin_trace=[section, case_goto.ID, 0],
                                           target=-1, target_trace=[section, goto_target_uuid], script=script)

        switch_loop_params = {2: SCTParameter(2, 'int'), 3: SCTParameter(3, 'int|jump')}
        switch_loop_params[2].value = int(sub_group)
        switch_loop_params[3].link = SCTLink('Jump', origin=-1, origin_trace=[section, inst, f'0{sep}3'],
                                             target=-1, target_trace=[section, case_goto.ID], script=script)

        cur_inst.l_params.insert(0, switch_loop_params)
        cur_inst.links_out.insert(0, switch_loop_params[3].link)
        cur_inst.params[1].set_value(cur_inst.params[1].value + 1)

        case_goto.links_in.append(switch_loop_params[3].link)
        case_goto.links_out.append(case_goto.params[0].link)
        cur_sect.insts[goto_target_uuid].links_in.append(case_goto.params[0].link)

        inst_pos = cur_sect.inst_list.index(inst)
        cur_sect.inst_list.insert(inst_pos + 1, case_goto.ID)
        cur_group[index][list(cur_group[index].keys())[0]] = {sub_group: [case_goto.ID], **test_group}

    def inst_specific_setup(self, script, sect, new_inst: SCTInstruction):
        if new_inst.base_id == 9:
            i = 0
            new_label = f'Untitiled({i})'
            while self.is_sect_name_used(script, new_label):
                i += 1
                new_label = f'Untitiled({i})'
            new_inst.label = new_label
        if new_inst.base_id == 10:
            section = self.project.scts[script].sects[sect]
            new_idx = section.inst_list.index(new_inst.ID)
            if new_idx == len(section.inst_list) - 1:
                tgt_inst = section.inst_list[new_idx - 1]
            else:
                tgt_inst = section.inst_list[new_idx + 1]
            new_link = SCTLink('Jump', origin=-1, origin_trace=[sect, new_inst.ID, 0],
                                           target=-1, target_trace=[sect, tgt_inst], script=script)
            new_inst.links_out.append(new_link)
            new_inst.params[0].link = new_link


    def adjust_IF_grouping_type(self, script, section, inst, new_tgt_inst):
        cur_sect = self.project.scts[script].sects[section]
        if len(cur_sect.insts[inst].my_master_uuids) == 0:
            return

        goto_inst = cur_sect.insts[inst]



        goto_tgt = goto_inst.links_out[0].target_trace[1]
        master_inst = cur_sect.insts[goto_inst.my_master_uuids[0]]

        if master_inst.base_id == 3:
            return

        master_tgt = master_inst.links_out[0].target_trace[1]

        if section != goto_inst.links_out[0].target_trace[0] or goto_tgt == master_tgt:
            cur_group_type = 'if'
        elif cur_sect.inst_list.index(goto_tgt) > cur_sect.inst_list.index(master_tgt):
            cur_group_type = 'else'
        else:
            cur_group_type = 'while'

        if new_tgt_inst == master_tgt or new_tgt_inst not in cur_sect.inst_list:
            new_group_type = 'if'
        elif cur_sect.inst_list.index(new_tgt_inst) > cur_sect.inst_list.index(master_tgt):
            new_group_type = 'else'
        else:
            new_group_type = 'while'

        group_key = f'{master_inst.ID}{sep}{cur_group_type}'
        parents, index = self.get_grouped_parents_and_index(group_key, cur_sect.inst_tree)

        cur_group = cur_sect.inst_tree
        for p in parents:
            cur_group = cur_group[p]

        if cur_group_type == new_group_type:
            if cur_group_type != 'else':
                return

            first_out_inst = self.get_next_grouped_uuid(script, section, master_tgt)

            # else becomes smaller
            if cur_sect.inst_list.index(new_tgt_inst) < cur_sect.inst_list.index(first_out_inst):
                insts_to_move = cur_sect.inst_list[cur_sect.inst_list.index(new_tgt_inst):
                                                   cur_sect.inst_list.index(first_out_inst)]
                for inst in reversed(insts_to_move):
                    cur_group.insert(index + 1, inst)

                cur_group = cur_group[index][group_key]

                ind_to_remove = cur_group.index(new_tgt_inst) + 1
                while len(cur_group) > ind_to_remove:
                    cur_group.pop(ind_to_remove)
            else:
                insts_to_move = cur_sect.inst_list[cur_sect.inst_list.index(first_out_inst):
                                                   cur_sect.inst_list.index(new_tgt_inst)]

                for inst in reversed(insts_to_move):
                    cur_group.remove(inst)

                cur_group = cur_group[index][group_key]

                for inst in reversed(insts_to_move):
                    cur_group.append(inst)
            return

        elif cur_group_type == 'else':
            # remove else group, replace insts, and change main group title to new title

            removed_else_entries = cur_group.pop(index)[group_key]

            for entry in reversed(removed_else_entries):
                cur_group.insert(index, entry)

            if new_group_type != 'while':
                return

            index = index - 1
            group_key = f'{master_inst.ID}{sep}if'

        elif new_group_type == 'else':
            # add else group, put required insts into else group

            if new_tgt_inst not in cur_group:
                raise IndexError('New target should be at same level as if Group')

            insts_to_move = cur_group[index+1:cur_group.index(new_tgt_inst)]
            for inst in insts_to_move:
                cur_group.remove(inst)

            cur_group.insert(index+1, {f'{master_inst.ID}{sep}else': insts_to_move})

            if cur_group_type == 'if':
                return

        # change group key
        old_group_entries = cur_group.pop(index)[group_key]
        cur_group.insert(index, {f'{master_inst.ID}{sep}{new_group_type}': old_group_entries})

    # --------------------------- #
    # Inst group analysis methods #
    # --------------------------- #

    def get_inst_group(self, script, section, inst_uuid) -> Union[None, dict]:
        cur_sect = self.project.scts[script].sects[section]
        parents, index = self.get_grouped_parents_and_index(inst_uuid, cur_sect.inst_tree)

        cur_level = cur_sect.inst_tree
        for parent in parents:
            cur_level = cur_level[parent]

        group = copy.deepcopy(cur_level[index])

        if not isinstance(group, dict):
            return None

        if index + 1 < len(cur_level):
            next_element = cur_level[index + 1]
            if isinstance(next_element, dict):
                if inst_uuid in list(next_element.keys())[0]:
                    group |= copy.deepcopy(next_element)

        return group

    def get_grouped_parents_and_index(self, element, grouped_region, parents=None) -> (list, int):
        if parents is None:
            parents = []

        if element in grouped_region:
            return parents, grouped_region.index(element)

        if isinstance(grouped_region, dict):
            for key, value in grouped_region.items():
                out_parents, index = self.get_grouped_parents_and_index(element, value, parents=parents + [key])
                if out_parents is not None:
                    return out_parents, index
        else:
            for i, entry in enumerate(grouped_region):
                if not isinstance(entry, dict):
                    continue

                for key, value in entry.items():
                    if element in key:
                        return parents, i

                    out_parents, index = self.get_grouped_parents_and_index(element, value, parents=parents + [i] + [key])
                    if out_parents is not None:
                        return out_parents, index

        return None, None

    def get_inst_group_bounds(self, cur_group) -> (str, str):
        if isinstance(cur_group, dict):
            cur_group = list(cur_group.values())
        first = None
        last = None
        i = 0
        while first is None:
            first = self.get_inst_uuid_from_group_entry(cur_group[i])
            i += 1
            if i == len(cur_group):
                return first, last
        j = len(cur_group) - 1
        while last is None:
            last = self.get_inst_uuid_from_group_entry(cur_group[j], last=True)
            j -= 1
        return first, last

    @staticmethod
    def _inst_sub_group_present(cur_group: Union[list, dict], sub_group: str):
        if 'else' in sub_group:
            for item in cur_group:
                if not isinstance(item, dict):
                    continue
                if list(item.keys())[0] == sub_group:
                    return True
        else:
            if sub_group in cur_group:
                return True
        return False

    def extract_inst_uuids_from_group(self, cur_group, uuids: Union[list, None] = None):
        if uuids is None:
            uuids = []

        for item in cur_group:
            item_uuid = self.get_inst_uuid_from_group_entry(item)
            if isinstance(item_uuid, str):
                if item_uuid not in uuids:
                    uuids.append(item_uuid)

            if not isinstance(item, dict):
                continue

            if item_uuid not in uuids:
                uuids.append(item_uuid)

            item_key = list(item.keys())[0]
            next_group = item[item_key]
            if isinstance(next_group, dict):
                for value in next_group.values():
                    uuids = self.extract_inst_uuids_from_group(value, uuids)
            else:
                uuids = self.extract_inst_uuids_from_group(next_group, uuids)

        return uuids

    def extract_parent_uuid_from_group(self, entry):
        if isinstance(entry, str):
            if sep not in entry:
                return entry
            return entry.split(sep)[0]

        elif isinstance(entry, dict):
            key = list(entry.keys())[0]
            return key.split(sep)[0]

        else:
            raise ValueError(f'{self.log_key}: Incorrect entry type, cannot extract uuid')

    def get_inst_uuid_from_group_entry(self, entry, last=False):
        if isinstance(entry, dict):
            if len(entry) == 0:
                item_uuid = None
            elif sep in list(entry.keys())[0] and len(entry[list(entry.keys())[0]]) == 0:
                item_uuid = list(entry.keys())[0].split(sep)[0]
            elif not last:
                item_key = list(entry.keys())[0]
                item_uuid = item_key.split(sep)[0]
            else:
                item_key = list(entry.keys())[-1]
                item_uuid = self.get_inst_uuid_from_group_entry(entry[item_key], last)
        elif isinstance(entry, list):
            if len(entry) == 0:
                item_uuid = None
            elif last:
                item_uuid = self.get_inst_uuid_from_group_entry(entry[-1], last)
            else:
                item_uuid = self.get_inst_uuid_from_group_entry(entry[0], last)
        else:
            return entry
        return item_uuid

    def get_next_grouped_uuid(self, script, section, inst):
        cur_sect = self.project.scts[script].sects[section]
        cur_inst = cur_sect.insts[inst]
        parents, index = self.get_grouped_parents_and_index(inst, cur_sect.inst_tree)
        cur_group = cur_sect.inst_tree
        for parent in parents:
            cur_group = cur_group[parent]

        if isinstance(cur_group[index], dict) and index + 1 < len(cur_group[index]):
            if 'if' in list(cur_group[index].keys())[0]:
                if inst == self.get_inst_uuid_from_group_entry(cur_group[index + 1]):
                    index += 1

        if index + 1 < len(cur_group):
            # if cur_inst is an if/else/while
            if cur_inst.base_id == 0 and len(cur_inst.my_goto_uuids) > 0:
                my_goto = cur_sect.insts[cur_inst.my_goto_uuids[0]]
                if len(my_goto.links_out) == 0:
                    new_tgt_inst_uuid = self.get_inst_uuid_from_group_entry(cur_group[index + 1])
                else:
                    new_tgt_inst_uuid = my_goto.links_out[0].target_trace[1]
            else:
                new_tgt_inst_uuid = self.get_inst_uuid_from_group_entry(cur_group[index + 1])
        else:
            # if no parents, and targets last inst, then it is probably the return inst which is valid
            if len(parents) == 0:
                return cur_group[-1]
            next_inst = parents[-1]
            if uuid_sep not in next_inst:
                if len(parents) == 1:
                    return None
                if isinstance(parents[-2], str):
                    if 'switch' in parents[-2]:
                        case_pos = -1
                        switch = self.project.scts[script].sects[section].insts[parents[-2].split(sep)[0]]
                        for i, lp in enumerate(switch.l_params):
                            if lp[2].value == int(next_inst):
                                case_pos = i
                        if case_pos + 1 != switch.params[1].value:
                            return switch.l_params[case_pos+1][3].link.target_trace[1]
                next_inst = parents[-2]
            next_inst = next_inst.split(sep)[0]
            new_tgt_inst_uuid = self.get_next_grouped_uuid(script, section, next_inst)

        return new_tgt_inst_uuid

    # ----------------- #
    # Inst link methods #
    # ----------------- #

    def remove_inst_links(self, script, section, inst, custom_tgt=None, remove_link=False,
                          direction: Literal['in', 'out', 'in out'] = 'in out'):
        cur_sect = self.project.scts[script].sects[section]
        cur_inst = cur_sect.insts[inst]
        if len(cur_inst.links_in) == 0 and len(cur_inst.links_out) == 0:
            return

        parents, index = self.get_grouped_parents_and_index(inst, cur_sect.inst_tree)
        cur_group = cur_sect.inst_tree
        for parent in parents:
            cur_group = cur_group[parent]

        new_tgt_inst_uuid = custom_tgt

        if 'in' in direction:
            for link in cur_inst.links_in:
                if new_tgt_inst_uuid is None:
                    new_tgt_inst_uuid = self.get_next_grouped_uuid(script, section, inst)
                ori_sect = link.origin_trace[0]
                ori_inst_uuid = link.origin_trace[1]
                ori_inst = self.project.scts[script].sects[ori_sect].insts[ori_inst_uuid]
                if new_tgt_inst_uuid is None or remove_link:
                    ori_inst.links_out.pop(ori_inst.links_out.index(link))
                else:
                    self.change_link_tgt(tgt_sect=cur_sect, link=link, new_tgt_uuid=new_tgt_inst_uuid, remove_from_old_tgt=False)

            cur_inst.links_in = []

        if 'out' in direction:
            for link in cur_inst.links_out:
                if link.target_trace is None:
                    continue
                tgt_sect = link.target_trace[0]
                tgt_inst_uuid = link.target_trace[1]
                self.project.scts[script].sects[tgt_sect].insts[tgt_inst_uuid].links_in.remove(link)

            cur_inst.links_out = []

        if len(cur_inst.my_master_uuids) > 0:
            for master_uuid in cur_inst.my_master_uuids:
                master_inst = cur_sect.insts[master_uuid]
                master_inst.my_goto_uuids.remove(cur_inst.ID)

        if len(cur_inst.my_goto_uuids) > 0:
            for goto_uuid in cur_inst.my_master_uuids:
                master_inst = cur_sect.insts[goto_uuid]
                master_inst.my_master_uuids.remove(cur_inst.ID)


    @staticmethod
    def change_link_tgt(tgt_sect: SCTSection, link: SCTLink, new_tgt_uuid: str, remove_from_old_tgt=True):
        prev_tgt_uuid = link.target_trace[1]
        if remove_from_old_tgt:
            tgt_sect.insts[prev_tgt_uuid].links_in.remove(link)
        tgt_sect.insts[new_tgt_uuid].links_in.append(link)
        link.target_trace[1] = new_tgt_uuid

    def repair_text_box_fade(self, sct_model, queue):
        project = TBStringToParamRepair.repair_project(project=self.project, inst_lib=self.base_insts, sct_model=sct_model, status_queue=queue)
        if project is None:
            return
        self.project = project

    # #  Refresh Methods  # #

    def refresh_abs_poses(self, scripts, queue):
        error_scts = []
        for script in scripts:
            queue.put({'sub_msg': f'{script}'})
            SCTEncoder.encode_sct_file_from_project_script(project_script=self.project.scts[script], use_garbage=True,
                                                           combine_footer_links=False, add_spurious_refresh=True,
                                                           base_insts=self.base_insts, update_inst_pos=True,
                                                           endian='big')
            for error in self.project.scts[script].errors:
                if 'Encoding' in error:
                    error_scts.append(script)
                    break
        return error_scts

    def refresh_condition(self, script, section, instruction, **kwargs):
        inst = self.project.scts[script].sects[section].insts[instruction]
        inst.generate_condition(self.get_script_variables_with_aliases(script))

    def get_project_encode_errors(self):
        errors = {}
        for name, sct in self.project.scts.items():
            sct_errors = []
            for error in sct.errors:
                if 'Encoding' in error:
                    sct_errors.append(error)
            if len(sct_errors) > 0:
                errors[name] = sct_errors
        return errors

    # # Update SCT Methods # #
    def get_script(self, script, queue):
        queue.put({'sub_msg': f'Encoding {script}'})
        ind, sct = SCTEncoder.encode_sct_file_from_project_script(project_script=self.project.scts[script],
                                                                  use_garbage=False, combine_footer_links=True,
                                                                  add_spurious_refresh=False, endian='big',
                                                                  base_insts=self.base_insts, update_inst_pos=True,
                                                                  separate_index=True)
        for error in self.project.scts[script].errors:
            if 'Encoding' in error:
                return script
        return ind, sct

    def find_similar_inst(self, script: str, other_section: SCTSection, inst_offset: int):
        other_inst = None
        for inst in other_section.insts.values():
            if inst_offset == inst.absolute_offset:
                other_inst = inst
                break
        if other_inst is None:
            return None

        my_script = self.project.scts[script]
        desired_section = other_section.name
        if desired_section not in my_script.sects:
            if desired_section in my_script.folded_sects:
                desired_section = my_script.folded_sects[desired_section]
            else:
                return None

        my_section = my_script.sects[desired_section]
        my_inst_list = my_section.inst_list
        if desired_section != other_section.name or my_section.is_compound:
            i0 = None
            i1 = -1
            found_it = False
            for i, inst in enumerate(my_inst_list):
                if my_section.insts[inst].base_id != 9:
                    continue
                if found_it:
                    i1 = i
                if my_section.insts[inst].label == other_section.name:
                    i0 = i
                    found_it = True
            if i0 is None:
                return None
            my_inst_list = my_inst_list[i0: i1]

        # find inst (or insts) in my inst list with same base_id and parameter values
        same_base_inst = []
        similar_insts = []
        for inst in my_inst_list:
            if my_section.insts[inst].base_id != other_inst.base_id:
                continue
            same_base_inst.append(inst)
            if self._insts_are_similar(my_section.insts[inst], other_inst):
                similar_insts.append(inst)

        if len(similar_insts) == 0:
            return None

        if len(similar_insts) == 1:
            return my_section.insts[similar_insts[0]].absolute_offset

        preceeding_insts = 0
        for inst in other_section.inst_list:
            if other_section.insts[inst].base_id != other_inst.base_id:
                continue
            if other_inst.ID == inst:
                break
            if self._insts_are_similar(other_section.insts[inst], other_inst):
                preceeding_insts += 1

        target_inst = min(len(similar_insts) - 1, preceeding_insts)
        return my_section.insts[similar_insts[target_inst]].absolute_offset

    def _insts_are_similar(self, inst: SCTInstruction, other_inst: SCTInstruction):
        if len(inst.params) != len(other_inst.params):
            return False
        if len(inst.l_params) != len(other_inst.l_params):
            return False

        for param_key, param in inst.params.items():
            if not self._params_are_similar(param, other_inst.params[param_key]):
                return False

        for i, loop in enumerate(inst.l_params):
            for param_key, param in loop.items():
                if not self._params_are_similar(param, other_inst.l_params[i][param_key]):
                    return False
        return True

    @staticmethod
    def _params_are_similar(param, other_param):
        if param.type != other_param.type:
            return False
        if 'scpt' in param.type and param.value != other_param.value:
            return False
        return True

    def get_sect_preditor_offset(self, script, inst_sect_name, inst_offset, tgt_sect_name):
        tgt_sect = self.project.scts[script].sects[tgt_sect_name]
        tgt_links = tgt_sect.insts[tgt_sect.inst_list[0]].links_in

        if len(tgt_links) == 0:
            return None

        backup_links = []
        links = []
        for link in tgt_links:
            if link.target_trace[0] == tgt_sect_name:
                backup_links.append(link)
                if link.origin_trace[0] == inst_sect_name:
                    links.append(link)

        if len(links) == 0:
            links = backup_links

        if len(links) == 0:
            return None

        if len(links) == 1:
            link = links[0]
        else:
            link_dists = [abs(self.project.scts[script].sects[link.origin_trace[0]]
                              .insts[link.origin_trace[1]].absolute_offset - inst_offset) for link in links]
            link = links[link_dists.index(min(link_dists))]

        return self.project.scts[script].sects[link.origin_trace[0]].insts[link.origin_trace[1]].absolute_offset

    # # Project Search Methods # #

    def search(self, search_entry, keep_case, headers=None):
        if self.searcher is None:
            self.searcher = ProjectSearcher(self.base_insts, self.project)
        links = self.searcher.search(search_entry, keep_case)

        return links

    def get_used_insts(self):
        inst_ids = []
        for key, script in self.project.scts.items():
            for sect_name, sect in script.sects.items():
                inst_ids += [v.base_id for v in sect.insts.values()]
        return [(f'{i}:{get_padding_for_number(i, 4, " ")},{self.base_insts.get_inst(i).name}', i) for i in sorted(list(set(inst_ids)))]

    def get_search_filter_trees(self):
        scripts = []
        sections = []

        for key, script in self.project.scts.items():
            scripts.append((key, key))
            sections += [(k, k) for k in script.sects]

        inst_ids = self.get_used_insts()

        return {
            'sct:': sorted(list(set(scripts))),
            'sect:': sorted(list(set(sections))),
            'inst:': inst_ids
        }
