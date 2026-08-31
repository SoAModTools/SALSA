import re
from typing import List, Dict

from SALSA.Analysis.script_performer import ScriptPerformer
from SALSA.Common.constants import FieldTypes as FT, KnownMemAddresses as KA
from SALSA.BaseInstructions.bi_container import BaseInst
from SALSA.Project.project_container import SCTScript


class SCTAnalyzer:
    export_option_fields = {'types': {'req': True, 'type': FT.string_list, 'values': []}}

    known_addr = {
        'Ship_battles': KA.Ship_Battles
    }

    temp_dir = 'E:\\temp'

    def __init__(self, loc=None, verbose=False, temp_dir=None):
        if temp_dir is not None:
            self.temp_dir = temp_dir
        self.verbose_out = verbose
        self.dir = loc
        self.script_list: List[SCTScript] = []
        self.instruction_list: Dict[str, BaseInst] = {}
        self.export_type = ''
        self.export_args = {}

        self.export_functions = {
            'Ship battle turn data': self._ship_battle_turn_parameters,
            'Ship battle turnID decisions': self._ship_battle_turnID_decisions,
            'Instruction Count': self._get_instruction_counts
        }

        self.export_options = {
            'Ship battle turn data': {
                'scripts': '^me5.+sct$',
                'instructions': {
                    165: {
                        'initiative': [
                            7
                        ],
                        'turn bonus': [
                            9, 10, 11, 12
                        ],
                        'lost turns': [
                            13, 14, 15, 16
                        ],
                        'entry offset': [
                            18
                        ]
                    },
                    213: {
                        'turn headings': [
                            0, 1, 2, 3, 4, 5, 6, 7, 8
                        ]
                    }
                },
                'headers': [
                    'turn bonus', 'lost turns', 'turn headings', 'initiative'
                ],
                'function': self._get_script_parameters_by_group
            },
            'Ship battle turnID decisions': {
                'scripts': '^me5.+sct$',
                'split_by_sct': True,
                'subscripts': ['_TURN_CHK'],
                'function': self._get_script_flows,
                'instructions': {174: 'scene'},
                'headers': 'ScriptID,*ID*\n\n*Inst*Starting Value (Traceback),Branches',
                'subscript_change_blacklist': {
                    '_WAIT_LOOP': '_BT_DBG'
                }
            },
            'Instruction Count': {
                'scripts': '^.+sct$',
                'header1': 'ScriptID',
                'header2': 'Inst details'
            }
        }

        self.script_performer = ScriptPerformer()

    def get_export_fields(self):
        types = list(self.export_options.keys())
        self.export_option_fields['types']['values'] = types
        return self.export_option_fields

    def get_export_args(self, export_type):
        return self.export_options[export_type]

    def export(self, sct_list, instruction_list, export_type, export_args=None):
        if export_args is None:
            export_args = {}
        self.script_list = sct_list
        self.instruction_list = instruction_list
        self.export_type = export_type
        self.export_args = export_args
        return self.export_functions[export_type]()

    def _get_script_parameters_by_group(self):
        script_info_list = {'headers': {}}

        for inst, groups in self.export_options[self.export_type]['instructions'].items():
            for group_name, group in groups.items():
                script_info_list['headers'][group_name] = self.instruction_list[str(inst)].get_param_names(group)

        for i, sct in enumerate(self.script_list):
            # print(f'Getting information for {sct.Name}: {i+1}/{len(self.script_list)}')
            script_info_out = sct.get_params_by_group(self.export_options[self.export_type]['instructions'])
            script_info_list[sct.Name] = script_info_out

        return script_info_list

    def _ship_battle_turn_parameters(self):
        info_list = self.export_options[self.export_type]['function']()
        combine = self.export_args.get('combine', False)

        for key, group in info_list.items():
            if key == 'headers':
                continue
            turn_headings = {}
            for entry in group['turn headings'][list(group['turn headings'])[0]].values():
                entry_key = entry.pop(0)
                values = [i for i in entry.values()]
                value_list = []
                for i in range(0, 4):
                    value_list.append([values[i*2], values[i*2+1]])
                turn_headings[entry_key] = value_list
            group['turn headings'] = turn_headings

        for sct_name, sct in info_list.items():
            if sct_name == 'headers':
                continue
            new_turn_headings = {}
            for key, value in sct['entry offset'].items():
                key1 = list(value.keys())[0]
                key2 = list(value[key1].keys())[0]
                offset = int(value[key1][key2])
                if offset in sct['turn headings'].keys():
                    new_turn_headings[key] = sct['turn headings'][offset]
            sct['turn headings'] = new_turn_headings

        turn_heading_icon_dict = {0: '---', 1: 'c!', 2: 'sp', 3: 'blank'}

        groups = [i for i in self.export_options['Ship battle turn data']['headers']]
        header_dict = info_list.pop('headers')
        header_dict['turn headings'] = {0: 'color', 1: 'icon'}
        scripts = info_list
        data_sets = len(groups)
        curr_group = ''
        if combine:
            data_sets = 1
            curr_group = 'all'
        max_turns = -1
        for sct in scripts.values():
            for key, group in sct.items():
                if key == 'turn bonus':
                    max_turns = max(max_turns, len(group))
        # print('check whats here')

        outs = {}
        for data_set in range(0, data_sets):
            if not combine:
                curr_group = groups[data_set]
            header_rows: List[str] = [',', 'Turn,Phase']
            rows: List[str] = []
            for i in range(0, max_turns):
                for j in range(0, 4):
                    rows.append(f'{i},{j}')

            for script_name, sct in scripts.items():
                header_rows[0] += f',{script_name},,,'
                if combine:
                    header_rows[0] += ',,,,'
                    header_rows[0] += ',,'
                for group_name, group in header_dict.items():
                    if not combine:
                        if not group_name == curr_group:
                            continue
                    if group_name == 'entry offset':
                        continue
                    if group_name == 'turn headings':
                        header_rows[0] = header_rows[0][:-2]
                    if group_name == 'initiative':
                        header_rows[0] = header_rows[0][:-3]

                    for param_id, param_name in group.items():
                        cur_row = 0
                        header_rows[1] += f',{param_name}'
                        for turn in sct[group_name].values():
                            for phase in range(0, 4):
                                if group_name == 'turn headings':
                                    # print('check this out')
                                    value = turn[phase][param_id]
                                    if param_name == 'icon':
                                        value = turn_heading_icon_dict[int(value)]
                                    rows[cur_row] += f',{value}'
                                else:
                                    value = turn.get(phase, '---')
                                    if not value == '---':
                                        value = int(value[param_id])
                                    rows[cur_row] += f',{value}'
                                cur_row += 1
                        for row in range(cur_row, len(rows)):
                            rows[row] += ','

            for row in range(0, len(header_rows)):
                header_rows[row] += '\n'

            for row in range(0, len(rows)):
                rows[row] += '\n'
            data = [*header_rows, *rows]
            outs[curr_group] = ''.join(data)

        return outs

    def _get_script_flows(self):
        script_info_list = {'headers': self.export_options[self.export_type]['headers']}

        for i, sct in enumerate(self.script_list):
            print(f'Getting information for {sct.Name}: {i + 1}/{len(self.script_list)}')
            script_info_out = sct.get_export_script_tree(self.export_options[self.export_type]['subscripts'],
                                                         list(self.export_options[self.export_type][
                                                                  'instructions'].keys()))
            script_info_list[sct.Name] = script_info_out

        return script_info_list

    def _ship_battle_turnID_decisions(self):
        info_dict = self.export_options[self.export_type]['function']()
        headers = info_dict.pop('headers')
        script_runs = {}
        blacklist = self.export_options[self.export_type].get('subscript_change_blacklist', None)
        for script_name, script in info_dict.items():
            print(f'\n---Now running {script_name}---')
            script_runs[script_name] = self.script_performer.run(script,
                                                                 self.export_options[self.export_type]['instructions'],
                                                                 temp_dir=self.temp_dir, blacklist=blacklist)

        temp_outs = {}
        for script, result in script_runs.items():
            if '.' in script:
                key = script.split('.')[0]
            else:
                key = script
            script_num = key[2:5]
            csv = headers
            csv = csv.replace('*ID*', script_num)
            csvlines = csv.splitlines()
            csv = '\n'.join(csvlines[:1])
            csv += '\n'
            csv += '\nMemory Addresses,Set Location,Address Value'
            type_known = False
            script_addr_type = None
            for script_type, type_key in KA.Script_types.items():
                if re.match(script_type, script):
                    type_known = True
                    script_addr_type = self.known_addr[type_key]

            used_addr = result['ram_stats']
            used_addr_clean = {}
            for addr, stats in used_addr.items():
                if not isinstance(addr, str):
                    if not isinstance(addr, int):
                        addr = int(addr)
                    addr = str(addr)
                used_addr_clean[addr] = stats
            used_addr = {k: used_addr_clean[k] for k in sorted(list(used_addr_clean))}
            for addr, stats in used_addr.items():
                if 'init_loc' in stats.keys():
                    csv += f'\n{addr},{stats["init_loc"]}'
                    if type_known:
                        for s_addr, value in script_addr_type.items():
                            if addr == s_addr:
                                csv += f',{value}'
                else:
                    print(f'no init location entered for {addr}')

            csv += '\n'
            csv += '\n'.join(csvlines[1:])
            inst_num = len(result['summary'])
            csv = csv.replace('*Inst*', 'Inst,' if inst_num > 1 else '')
            csv_verbose = csv
            csv_condensed = csv
            diff_condensed = ''
            diff_verbose = ''
            level = 0
            for inst, values in result['summary'].items():
                if inst_num > 1:
                    prefix = ',' * level
                    diff_condensed += f'\n{prefix}{inst}'
                    diff_verbose += f'\n{prefix}{inst}'
                    level += 1
                for value, traces in values.items():
                    commas = ',' * level
                    value_prefix = f'\n{commas}{value}'
                    for trace, diff_dict in traces.items():
                        trace_prefix = f'{value_prefix} ({trace.replace(",", ":")})'
                        if diff_dict['has_diff']:
                            body_verbose = self._format_diff_tree(diff_dict['stratified'], level,
                                                                  diff_dict['trace_level'])
                            body_condensed = self._format_diff_tree(diff_dict['condensed'], level,
                                                                    diff_dict['trace_level'])
                            diff_verbose += f'\n{trace_prefix}{body_verbose}'
                            diff_condensed += f'\n{trace_prefix}{body_condensed}'
                        else:
                            body_out = self._format_diff_tree(diff_dict['out'], level, diff_dict['trace_level'])
                            diff_verbose += f'\n{trace_prefix}{body_out}'
                            diff_condensed += f'\n{trace_prefix}{body_out}'

                if inst_num > 1:
                    level -= 1

            csv_condensed += diff_condensed
            csv_verbose += diff_condensed
            csv_condensed = self._fill_out_commas(csv_condensed)
            csv_verbose = self._fill_out_commas(csv_verbose)
            temp_outs[key] = csv_condensed
            if self.verbose_out:
                temp_outs[f'{key}_verbose'] = csv_verbose

        return temp_outs

    def _format_diff_tree(self, diff_dict, diff_level, trace_lvl) -> str:
        diff_level += 1
        output = ''
        if 'inst' not in diff_dict:
            body = f'-> {diff_dict["value"]} ({ScriptPerformer.get_traceback_string(diff_dict["traceback"], trace_lvl).replace(",", ":")}) '
            return f',{body}'
        inst = diff_dict['inst']
        commas = ',' * diff_level
        if inst == 'out':
            out_values = diff_dict['out']
            if 'exit' in out_values:
                level = 0
            else:
                level = trace_lvl
            body = f'-> {out_values["value"]} ({ScriptPerformer.get_traceback_string(out_values["traceback"], level).replace(",", ":")}) '
            output = f',{body}\n'
        elif inst == 'choice':
            question = diff_dict['question']
            responses = {f'{commas}{k}': v for k, v in diff_dict['options'].items()}
            output += f',{question}'
            for key, option in responses.items():
                output += f'\n{key}'
                option_result = self._format_diff_tree(option, diff_level, trace_lvl)
                output += option_result
        elif inst == 'jumpif':
            condition = diff_dict['condition']
            responses = {f'{commas}{k}': v for k, v in diff_dict['options'].items()}
            output += f',{condition}'
            for key, option in responses.items():
                output += f'\n{key}'
                option_result = self._format_diff_tree(option, diff_level, trace_lvl)
                output += option_result
        elif inst == 'switch':
            switch = f'switch-{diff_dict["condition"]}'
            responses = {f'{commas}{k}': v for k, v in diff_dict['options'].items()}
            output += f',{switch}'
            for key, option in responses.items():
                output += f'\n{key}'
                option_result = self._format_diff_tree(option, diff_level, trace_lvl)
                output += option_result
        else:
            print('Add another segment?')

        diff_level -= 1
        return output

    @staticmethod
    def _fill_out_commas(csv):
        csv_lines = csv.splitlines()
        max_comma_num = 0
        for line in csv_lines:
            cur_commas = line.count(',')
            max_comma_num = max(cur_commas, max_comma_num)

        new_lines = []
        for line in csv_lines:
            cur_commas = line.count(',')
            rem_commas = ',' * (max_comma_num - cur_commas)
            new_lines.append(f'{line}{rem_commas}')

        out = f'\n'.join(new_lines)
        return out

    def _get_instruction_counts(self):
        print_rows = []
        print_header1 = ',Script IDs'
        print_header2 = 'Inst'
        first_sct = True
        for script in self.script_list:
            name = script.Name[2:6]
            if script.Name[6] != '.':
                name += script.Name[6]
            insts_used_raw = script.Details['Instructions Used']
            print_header2 += f',{name}'
            for i in range(0, 266):
                if first_sct:
                    print_rows.append(f'{i} - {self.instruction_list[str(i)]._name}')
                print_rows[i] += f',{insts_used_raw.get(i, 0)}'
            first_sct = False
        all_rows = [print_header1, print_header2, *print_rows]
        csv = '\n'.join(all_rows)
        instruction_count_output = {'Instruction Counts': self._fill_out_commas(csv=csv)}

        return instruction_count_output


