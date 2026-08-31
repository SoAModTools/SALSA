import re
import copy
import json
import math
import multiprocessing as mp
import os
import shutil
import sys
from datetime import datetime
from math import floor
from typing import List, Dict

from SALSA.Common.console_progress_bar import printProgressBar


class ScriptPerformer:
    current_subscript = ''
    current_index = 0
    index_stack = []
    addrs = {}
    out = {}
    switches = {}
    new_opens = []
    all_init_ram_conditions = []
    new_closed = []
    all_starts = []
    requested_hit = False
    false_branches = []
    false_switch_entries = []
    debug_verbose = False
    with_compare_assumption = False
    reset_cond_states_between_runs = False
    use_multiprocessing = True
    chunk_compare_num = 5000
    max_chunks = 50
    results = []
    temp_ext = '.tmp'
    use_actions = True

    def __init__(self):

        self.scpt_codes = {
            '(1)<(2)': self._is_lt,
            '(1)<=(2)': self._is_leq,
            '(1)>(2)': self._is_gt,
            '(1)>=(2)': self._is_geq,
            '(1)==(2)': self._is_eq,
            '(1)==(2)[5]': self._is_eq,
            '(1)&(2)': self._anded,
            '(1)|(2)': self._ored,
            '(1)!=0 and (2)!=0': self._both_true,
            '(1)!=0 or (2)!=0': self._either_true,
            'overwrites (1) with (2)': self._return_two,
            '(1)*(2)': self._multiply,
            '(1)/(2)': self._divide,
            '(1)%(2)': self._mod,
            '(1)+(2)': self._add,
            '(1)-(2)': self._subtract,
        }

        self.time = datetime.now()

    def run(self, input_dict, inst_details, temp_dir, blacklist=None):
        self.addrs = input_dict.get('addresses', {})
        temp_addrs = {}
        for key, value in self.addrs.items():
            if isinstance(key, float):
                key = int(key)
            if isinstance(key, int):
                key = str(key)
            temp_addrs[key] = value
        self.addrs = temp_addrs
        self.all_starts = []
        self.new_opens = []
        self.new_closed = []
        init_script = input_dict['subscript_groups'].pop('init')
        init_ram = self._get_defined_ram()
        print(f'\nSubscript: init: Iteration 0')
        self._run_subscript_branch(name='init', subscripts=init_script, ram=init_ram, blacklist=blacklist)

        all_closed = self.new_closed
        self.new_closed = []

        all_open = self.new_opens
        self.new_opens = []

        if len(all_open) > 0:
            self.new_opens.append(all_open.pop(0))

        hit_requested = False
        for action in self.new_opens[0]['actions']:
            if 'requested' in action.keys():
                hit_requested = True

        for name, sub_tree in input_dict['subscript_groups'].items():
            self.time = datetime.now()
            done = False
            if len(self.new_opens) == 0:
                initial_ram = self._get_defined_ram()
                branch_index = None
            else:
                branch_index = 0
                initial_ram = copy.deepcopy(self.new_opens[branch_index]['cur_ram'])

            iteration = 0
            with_mid = True
            while not done:
                iteration += 1

                self.false_branches = []

                if branch_index is not None:
                    current_branch_init: dict = self.new_opens[0]['init_value']
                    if current_branch_init is not None:
                        if 'parameter values' in current_branch_init:
                            value_out = current_branch_init["parameter values"]["scene"]
                            if isinstance(value_out, str):
                                value_out = current_branch_init["address_values"][value_out]
                        else:
                            value_out = 'None'
                    else:
                        value_out = 'None'
                else:
                    value_out = 'None'

                print(f'\nSubscript: {name}: Iteration {iteration}: Init Value:{value_out}')

                if self.debug_verbose:
                    print('Starting branch run:')
                self._run_subscript_branch(name=name, subscripts=sub_tree, hit_requested=hit_requested,
                                           ram=initial_ram, branch_index=branch_index, blacklist=blacklist)

                new_open_num = len(self.new_opens)

                false_branch_num = len(self.false_branches)
                print(
                    f'{new_open_num} branches opened - {false_branch_num} false branches detected and flagged')

                branches_to_remove = [*self.false_branches]
                for branch in reversed(sorted(list(set(branches_to_remove)))):
                    self.new_opens.pop(branch)

                # transfer completed branches to new closed, add closed branches to all_open
                branches_to_transfer = []
                ends = []
                for i, branch in enumerate(self.new_opens):
                    printProgressBar(prefix='Transferring Completed Branches', total=new_open_num,
                                     iteration=i, length=124)
                    b_actions = branch['actions']
                    has_req = False
                    has_end = False
                    for action in reversed(b_actions):
                        if list(action.keys())[0] == 'requested':
                            has_req = True
                            branch['out_value'] = copy.deepcopy(action['requested'])
                            break
                        elif list(action.keys())[0] == 'end':
                            has_end = True
                            branch['out_value'] = copy.deepcopy(action['end'])
                            branch['exit'] = True
                            ends.append(i)
                            break
                    if has_req or has_end:
                        branches_to_transfer.append(i)
                printProgressBar(prefix='Transferring Completed Branches', total=new_open_num,
                                 iteration=new_open_num, length=124)

                for b in reversed(sorted(list(set(branches_to_transfer)))):
                    old_branch = self.new_opens.pop(b)
                    if 'init_ram' not in old_branch:
                        old_branch['init_ram'] = {}
                    old_branch['end_ram'] = copy.deepcopy(old_branch['cur_ram'])
                    self.new_closed.append(copy.deepcopy(old_branch))
                    if b not in ends:
                        new_branch = {'init_value': old_branch['out_value'],
                                      'init_ram': copy.deepcopy(old_branch['cur_ram']),
                                      'cur_ram': copy.deepcopy(old_branch['cur_ram']),
                                      'actions': [], 'switch_states': [], 'jump_states': []}
                        all_open.append(new_branch)

                all_open = [*all_open, *self.new_opens]
                self.new_opens = []
                all_closed = [*all_closed, *self.new_closed]
                self.new_closed = []

                open_num = len(all_open)

                branches_to_remove = []
                for i, branch in enumerate(all_open):
                    if 'new_run' in branch.keys():
                        branches_to_remove.append(i)

                # flag for removal identical open branches

                should_parallel_open = ((open_num * open_num) / 2) > self.chunk_compare_num
                if self.use_multiprocessing and should_parallel_open:
                    print('Preparing Workers to flag identical open branches...')
                    segments = []
                    branch_num_for_chunk_calc = len(all_open)
                    branch_num_for_index_calc = branch_num_for_chunk_calc - 1
                    branch_num_sq = branch_num_for_chunk_calc * branch_num_for_chunk_calc
                    compare_num = branch_num_sq / 2
                    chunks = floor(compare_num / self.chunk_compare_num) + 1
                    cpus = min(chunks, mp.cpu_count())
                    compares_per_chunk = floor(compare_num / cpus)
                    last_index = -1
                    is_last_segment = False
                    for i in range(cpus):
                        first_index = last_index + 1
                        quadratic_sqrt = math.sqrt((branch_num_for_index_calc * branch_num_for_index_calc) - (
                                4 * (-1 / 2) * compares_per_chunk))
                        numerator = abs((branch_num_for_index_calc * -1) + quadratic_sqrt)
                        if numerator == 0:
                            numerator = abs((branch_num_for_index_calc * -1) - quadratic_sqrt)
                        index_num_for_chunk = math.ceil(abs(numerator / (2 * (-1 / 2))))
                        last_index = first_index + index_num_for_chunk
                        if last_index >= len(all_open):
                            is_last_segment = True
                            last_index = open_num - 1
                        segments.append({'start_index': first_index, 'last_index': last_index, 'branches': all_open})
                        if is_last_segment:
                            break
                        branch_num_for_index_calc -= (last_index - first_index) + 1
                    if len(segments) == 0:
                        segments.append({'start_index': 0, 'last_index': open_num - 1, 'branches': all_open})
                    else:
                        segments[-1]['last_index'] = open_num - 1
                    pool = mp.Pool(cpus)
                    results = pool.map(self._open_branch_duplicate_flagging, segments)
                    pool.close()
                    pool.join()
                    for result in results:
                        branches_to_remove = [*branches_to_remove, *result]
                    print()
                else:
                    results = self._open_branch_duplicate_flagging(
                        args_in={'start_index': 0, 'last_index': (len(all_open) - 1), 'branches': all_open})

                    branches_to_remove = [*branches_to_remove, *results]

                # flag for removal open branches with the same conditions as a closed branch
                total_closed = len(all_closed)
                should_parallel_closed = (total_closed * open_num) > self.chunk_compare_num
                if self.use_multiprocessing and should_parallel_closed:
                    print('Preparing Workers to flag mirrored open branches...')
                    segments = []
                    branch_num = len(all_closed)
                    cpus = mp.cpu_count()
                    last_index = -1
                    for i in range(cpus):
                        first_index = last_index + 1
                        last_index = floor(branch_num * ((i + 1) / cpus))
                        if last_index >= branch_num:
                            last_index = branch_num - 1
                        segments.append({'start_index': first_index, 'last_index': last_index, 'with_mid': with_mid,
                                         'branches_to_remove': branches_to_remove, 'branches': all_open,
                                         'closed_branches': all_closed[first_index:last_index]})
                    if not len(segments) == 0:
                        segments[-1]['last_index'] = (len(all_closed) - 1)
                    pool = mp.Pool(cpus)
                    results = pool.map(self._closed_branch_duplicate_flagging, segments)
                    pool.close()
                    pool.join()
                    for result in results:
                        branches_to_remove = [*branches_to_remove, *result]
                    print()
                else:
                    print('Flagging mirrored open branches...')
                    results = self._closed_branch_duplicate_flagging(
                        args_in={'start_index': 0, 'last_index': (len(all_closed) - 1), 'with_mid': with_mid,
                                 'branches_to_remove': branches_to_remove,
                                 'closed_branches': all_closed, 'branches': all_open})
                    branches_to_remove = [*branches_to_remove, *results]

                # Remove flagged branches
                branches_to_remove = sorted(list(set(branches_to_remove)))
                for i in reversed(branches_to_remove):
                    if i >= len(all_open):
                        print(f'branch_remove_error: index {i} out of range')
                        continue
                    all_open.pop(i)

                print(
                    f'{len(branches_to_remove)} branches pruned - {len(all_open)} open branches '
                    f'remaining - {len(all_closed)} closed branches')

                # Remove any switch or jump states for a fresh run
                if self.reset_cond_states_between_runs:
                    for branch in all_open:
                        branch['switch_states'] = []
                        branch['jump_states'] = []

                hit_requested = False

                if len(all_open) == 0:
                    done = True
                else:
                    branch_index = 0
                    updated_branch = all_open.pop(0)
                    mid_ram = copy.deepcopy(updated_branch['cur_ram'])
                    initial_ram = copy.deepcopy(mid_ram)
                    if 'new_run' in updated_branch.keys():
                        new_run_dict = updated_branch['new_run']
                        new_run_dict['value'] = new_run_dict['value'] + 1
                        new_run_dict['mid_ram'].append(mid_ram)
                    else:
                        new_run_dict = {'value': 1, 'mid_ram': [mid_ram]}
                    updated_branch['new_run'] = new_run_dict
                    self.new_opens.append(updated_branch)

            print('\n')
            # Remove duplicates and any branch which goes past the out value, and any branch which contains a choice without modification

        tree_difference_summary = self._make_all_out_summary(inst_details=inst_details, temp_dir=temp_dir,
                                                             all_branches=all_closed)
        print('Done with tree summary')

        # Extract start from values
        has_starts = {}
        for inst, values in tree_difference_summary.items():
            has_start = False
            for value in values:
                if isinstance(value, str):
                    if value == 'Start':
                        has_start = True
            has_starts[inst] = has_start
        starts = {}
        for inst, has_start in has_starts.items():
            if has_start:
                starts[inst] = {'Start': tree_difference_summary[inst].pop('Start')}

        # Sort summaries by value and add start to the beginning

        sorted_summary = {}
        for inst, values in tree_difference_summary.items():
            if has_starts[inst]:
                sorted_summary[inst] = starts[inst]
            else:
                sorted_summary[inst] = {}
            sorted_summary[inst] = {**sorted_summary[inst], **{k: values[k] for k in sorted(list(values))}}

        d_time = datetime.now() - self.time
        d = {"days": d_time.days}
        d["hours"], rem = divmod(d_time.seconds, 3600)
        d["minutes"], d["seconds"] = divmod(rem, 60)
        time_difference = "{days} days {hours}:{minutes}:{seconds}".format(**d)
        print('Time to complete this subscript: ', time_difference)

        return {'trees_detail': self.new_closed, 'ram_stats': self.addrs, 'summary': sorted_summary}

    def _open_branch_duplicate_flagging(self, args_in):
        start_index = args_in['start_index']
        last_index = args_in['last_index']
        branches = args_in['branches']
        branches_to_remove = []
        current_open_branch_num = last_index - start_index + 1
        if last_index == -1:
            current_open_branch_num = len(branches) - start_index
        for i, open1 in enumerate(branches[start_index:last_index]):
            printProgressBar(prefix='Flagging identical open branches',
                             suffix=f'total_branches: {current_open_branch_num}',
                             total=current_open_branch_num, iteration=i, length=119)
            sys.stdout.flush()
            for j, open2 in enumerate(branches[start_index + i + 1:]):
                ind2 = start_index + i + 1 + j
                if self._variables_are_equal_recursive(open1, open2):
                    branches_to_remove.append(ind2)
        printProgressBar(prefix='Flagging identical open branches', suffix=f'total_branches: {current_open_branch_num}',
                         total=current_open_branch_num, iteration=current_open_branch_num, length=119)

        return branches_to_remove

    def _closed_branch_duplicate_flagging(self, args_in):
        repeats = []
        branches_to_remove = args_in['branches_to_remove']
        start_index = args_in['start_index']
        last_index = args_in['last_index']
        with_mid = args_in['with_mid']
        closed_branches = args_in['closed_branches']
        branches = args_in['branches']
        current_outs = last_index - start_index + 1
        for i, out_branch in enumerate(closed_branches):
            printProgressBar(prefix='Removing open branches which mirror closed branches', length=100,
                             total=current_outs, iteration=i)
            sys.stdout.flush()
            for j, open_branch in enumerate(branches):
                if j in branches_to_remove:
                    continue
                elif self._branch_has_same_conditions(out_branch=out_branch, open_branch=open_branch,
                                                      with_req=True, with_mid=with_mid):
                    branches_to_remove.append(j)
                    repeats.append(j)

        printProgressBar(prefix='Removing open branches which mirror closed branches',
                         total=current_outs, length=100,
                         iteration=current_outs)
        sys.stdout.flush()

        return branches_to_remove

    def _run_subscript_branch(self, name, subscripts, ram=None, branch_index=None, hit_requested=False,
                              back_log=None, ptr=None, traceback=None, depth=0, blacklist=None) -> bool:

        if depth > 30:
            print('Lots of recursion...')

        if traceback is None:
            traceback: List[Dict] = []

        if back_log is None:
            back_log: List[Dict] = []
        else:
            traceback = copy.deepcopy(back_log)

        current_pointer = ptr
        if current_pointer is None:
            current_pointer = 0
        elif current_pointer >= len(subscripts[name]['pos_list']):
            return hit_requested

        traceback.append({'name': name, 'ptr': current_pointer})

        if len(traceback) > 10:
            print('recursion...')

        tabs = '\t' * len(traceback)
        spaces = ' ' * depth
        current_sub = subscripts[name]
        if len(current_sub['pos_list']) == 0:
            if self.debug_verbose:
                print(f'{tabs}{name}: no actions')
            return hit_requested
        else:
            if self.debug_verbose:
                print(f'{tabs}{name}')

        cur_ram = ram
        if current_pointer >= len(current_sub['pos_list']):
            print('error: current pointer outside position list length')
        inst_pos = current_sub['pos_list'][current_pointer]
        inst = current_sub[inst_pos]
        done = False
        force_branch = False
        force_jump = False
        increment_pointer = True
        modify = False
        while not done:
            cur_trace_id = len(traceback) - 1
            traceback[-1]['ptr'] = current_pointer
            if self.debug_verbose:
                print(f'{spaces}depth: {depth} current position: {name}:{current_pointer}-{inst_pos}')
            if 'set' in inst:
                new_ram = self._set_memory_pos(inst['set'], cur_ram)
                if branch_index is None:
                    new_branch = {'init_ram': {}, 'cur_ram': copy.deepcopy(new_ram), 'switch_states': [], 'actions': [],
                                  'jump_states': [], 'init_value': {'instruction': 'all', 'traceback': [{'name': 'Start', 'pos': 0}]}}
                    self.new_opens.append(new_branch)
                    branch_index = 0
                self.new_opens[branch_index]['cur_ram'] = new_ram
                cur_ram = new_ram

                switch_states_to_remove = []
                for i, state in enumerate(self.new_opens[branch_index]['switch_states']):
                    if state['address'] == inst['set']['addr']:
                        switch_states_to_remove.append(i)

                switch_states_to_remove = reversed(sorted(list(set(switch_states_to_remove))))
                for state in switch_states_to_remove:
                    self.new_opens[branch_index]['switch_states'].pop(state)

                jump_states_to_remove = []
                for i, state in enumerate(self.new_opens[branch_index]['jump_states']):
                    addr = inst['set']['addr']
                    addr = addr if isinstance(addr, str) else str(addr)
                    if addr in state['condition']:
                        jump_states_to_remove.append(i)

                jump_states_to_remove = reversed(sorted(list(set(jump_states_to_remove))))
                for state in jump_states_to_remove:
                    self.new_opens[branch_index]['jump_states'].pop(state)

            elif 'loop' in inst:
                return False

            elif 'goto' in inst or 'goto_subscript' in inst:
                inst_name = list(inst.keys())[0]
                if 'subscript' in inst_name:
                    name = inst['goto_subscript']['next']
                    current_sub = subscripts[name]
                    inst_pos = inst['goto_subscript']['location']

                else:
                    inst_pos = inst['goto']
                current_pointer = self._get_ptr(pos=inst_pos, pos_list=current_sub['pos_list'])
                if 'subscript' in inst_name:
                    traceback[-1] = {'name': name, 'ptr': current_pointer}
                increment_pointer = False

            elif 'jumpif' in inst or 'subscript_jumpif' in inst:
                inst_name = list(inst.keys())[0]
                if branch_index is None:
                    self.new_opens.append({'init_value': {'instruction': 'all', 'traceback': [{'name': 'Start', 'pos': 0}]}, 'init_ram': copy.deepcopy(cur_ram), 'switch_states': [],
                                           'actions': [], 'jump_states': [], 'cur_ram': copy.deepcopy(cur_ram)})
                    branch_index = 0

                if 'subscript' in inst_name:
                    jump_condition = inst['subscript_jumpif']['condition']
                    next_name = inst['subscript_jumpif']['next']
                    inst_pos = inst['subscript_jumpif']['location']
                    action_jump_dict = {'subscript': next_name, 'pos': inst_pos}
                    inst_ptr = self._get_ptr(inst_pos, subscripts[next_name]['pos_list'])
                else:
                    jump_condition = inst['jumpif']['condition']
                    next_name = name
                    inst_pos = inst['jumpif']['location']
                    action_jump_dict = {'subscript': next_name, 'pos': inst_pos}
                    inst_ptr = self._get_ptr(inst_pos, subscripts[next_name]['pos_list'])

                action_jump_dict['condition'] = jump_condition
                action_jump_dict['traceback'] = copy.deepcopy(traceback)

                has_condition = False
                condition_index = None
                condition_string = self._generate_condition_string(jump_condition[list(jump_condition.keys())[0]])

                addresses = re.findall('0x[0-9,a-d]{8}', condition_string)
                compare_address_dict = {}
                for address in addresses:
                    value = self._get_memory_pos(address, cur_ram)
                    if value is not None:
                        compare_address_dict[address] = value
                        new_string = f'{address}({value})'
                        condition_string = condition_string.replace(address, new_string)

                if len(compare_address_dict) > 0:
                    action_jump_dict['address_dict'] = compare_address_dict

                for i, state in enumerate(self.new_opens[branch_index]['jump_states']):
                    if state['condition'] == condition_string:
                        has_condition = True
                        condition_index = i

                jump = False
                make_branch = False
                if has_condition and self.with_compare_assumption:
                    jump = self.new_opens[branch_index]['jump_states'][condition_index]['jumped']
                else:
                    can_jump = self._can_jump(jump_condition, copy.deepcopy(cur_ram))

                    if not force_branch:
                        if can_jump:
                            should_jump = not self._should_not_jump(compare=jump_condition, ram=cur_ram, isBase=True)
                            if should_jump:
                                jump = True
                                make_branch = False
                        else:
                            make_branch = True
                    else:
                        make_branch = True
                        force_branch = False

                    if force_jump:
                        make_branch = False
                        jump = True

                if make_branch:
                    back_log = copy.deepcopy(traceback)
                    if len(back_log) > 0:
                        back_log.pop()
                    new_branch_index = len(self.new_opens)
                    action_jump_dict['jumped'] = True
                    new_branch = copy.deepcopy(self.new_opens[branch_index])

                    if modify:
                        mod_key = list(new_branch['actions'][-1])[0]
                        new_branch['actions'][-1][mod_key]['modification'] = {
                            'jumpif': copy.deepcopy(action_jump_dict)}
                    else:
                        new_branch['actions'].append(
                            {'jumpif': copy.deepcopy(action_jump_dict)})

                    if self.with_compare_assumption:
                        new_branch['jump_states'].append({'condition': condition_string, 'jumped': True})

                    self.new_opens.append(new_branch)

                    new_ram = copy.deepcopy(self.new_opens[new_branch_index]['cur_ram'])

                    sub_hit_requested = self._run_subscript_branch(name=next_name, subscripts=subscripts, ptr=inst_ptr,
                                                                   ram=new_ram, back_log=copy.deepcopy(back_log),
                                                                   hit_requested=hit_requested,
                                                                   branch_index=new_branch_index, depth=depth + 1,
                                                                   blacklist=blacklist)

                    if not sub_hit_requested:
                        self.false_branches.append(new_branch_index)

                if jump:
                    current_pointer = inst_ptr
                    increment_pointer = False
                    if 'subscript' in inst_name:
                        next_name = inst['subscript_jumpif']['next']
                        next_inst_pos = inst['subscript_jumpif']['location']
                        next_inst_ptr = self._get_ptr(pos=next_inst_pos, pos_list=subscripts[next_name]['pos_list'])
                        traceback[-1] = {'name': next_name, 'ptr': next_inst_ptr}
                        name = next_name
                        current_sub = subscripts[next_name]
                        current_pointer = next_inst_ptr

                action_jump_dict['jumped'] = jump
                updated_branch = self.new_opens[branch_index]

                if modify:
                    mod_key = list(updated_branch['actions'][-1])[0]
                    updated_branch['actions'][-1][mod_key]['modification'] = {
                        'jumpif': action_jump_dict}
                else:
                    updated_branch['actions'].append({'jumpif': action_jump_dict})

                if self.with_compare_assumption:
                    if not has_condition:
                        updated_branch['jump_states'].append({'condition': condition_string, 'jumped': jump})
                self.new_opens[branch_index] = updated_branch

                force_branch = False
                force_jump = False
                modify = False

            elif 'end' in inst:
                end_dict = copy.deepcopy(inst['end'])
                pos_traceback = self._convert_traceback_to_pos(copy.deepcopy(traceback), subscripts)
                pos_traceback.append({'name': end_dict['script'], 'pos': 'Start'})
                end_dict = {**end_dict, 'traceback': pos_traceback}

                if branch_index is not None:
                    self.new_opens[branch_index]['actions'].append({'end': copy.deepcopy(end_dict)})
                    prev_req_dict: dict = copy.deepcopy(self.new_opens[branch_index]['init_value'])
                    if 'parameter values' in prev_req_dict.keys():
                        closing_branch = f'{branch_index}:{prev_req_dict["parameter values"]}-' \
                                         f'{prev_req_dict["address_values"]}'
                    else:
                        closing_branch = f'{branch_index}: Start'
                else:
                    closing_branch = f'{branch_index}: Start'

                if self.debug_verbose:
                    print(
                        f'\tDepth: {depth}, Closing Branch: {closing_branch} (would switch to {inst["end"]["script"]})')

                return True

            elif 'requested' in inst:
                if inst_pos == 185:
                    # print('pause here')
                    pass
                hit_requested = True
                req_dict = copy.deepcopy(inst['requested'])
                current_inst_code = req_dict['instruction']
                params = req_dict['parameter values']
                param_address_values = {}
                for value in params.values():
                    if not isinstance(value, str):
                        value = str(value)
                    elif 'Bit: ' in value:
                        addr = value.split(' ')[1]
                        param_address_values[value] = cur_ram[addr]
                    if ': 0x8' in value:
                        if ' ' in value:
                            addr = value.split(' ')[1]
                        else:
                            addr = value
                        param_address_values[value] = self._get_memory_pos(addr, cur_ram, 'important')

                pos_traceback = self._convert_traceback_to_pos(copy.deepcopy(traceback), subscripts)

                req_dict = {**req_dict, 'address_values': param_address_values, 'subscript': name,
                            'traceback': pos_traceback, 'pos': inst_pos}

                if branch_index is not None:
                    self.new_opens[branch_index]['actions'].append({'requested': req_dict})
                    prev_req_dict = self.new_opens[branch_index]['init_value']
                    if 'parameter values' in prev_req_dict.keys():
                        closing_branch = f'{branch_index}:{prev_req_dict["parameter values"]}-' \
                                         f'{prev_req_dict["address_values"]}'
                    else:
                        closing_branch = f'{branch_index}: Start'
                else:
                    out_branch = {'end_ram': copy.deepcopy(cur_ram), 'actions': [{'requested': req_dict}],
                                  'init_value': {'instruction': current_inst_code,
                                                 'traceback': [{'name': 'Start', 'pos': 0}]},
                                  'switch_states': [], 'jump_states': []}
                    self.new_opens.append(out_branch)
                    closing_branch = 'None'
                    branch_index = 0

                if self.debug_verbose:
                    print(f'\tDepth: {depth}, Adding Req for {inst["requested"]["inst"]} to Branch {closing_branch}')

            elif 'switch' in inst:
                if branch_index is None:
                    self.new_opens.append({'init_value': {'instruction': 'all', 'traceback': [{'name': 'Start', 'pos': 0}]}, 'init_ram': copy.deepcopy(cur_ram),
                                           'cur_ram': copy.deepcopy(cur_ram), 'actions': [], 'switch_states': [],
                                           'jump_states': []})
                    branch_index = 0

                switch_all = False
                if force_branch:
                    switch_all = True
                    force_branch = False

                # if memory addr has numerical value follow switch, add option: 'internal'
                switch_addr = inst['switch']['condition']
                switch_entries = inst['switch']['entries']

                can_switch = self._can_switch(addr=switch_addr, ram=cur_ram)
                if not can_switch:
                    switch_all = True

                # check for switch in list of switches, add switch to list if not present
                switch_ID = f'{name}-{inst_pos}'
                if switch_ID not in self.switches.keys():
                    self.switches[switch_ID] = inst['switch']

                # Test for previous switch decisions:
                prev_switch_entry = None
                prev_switch_state = False
                if switch_all:
                    for state in self.new_opens[branch_index]['switch_states']:
                        if state['address'] == switch_addr:
                            switch_all = False
                            prev_switch_state = True
                            prev_switch_entry = state['entry']

                # If entry cannot be selected, produce a branch for each entry
                if switch_all:
                    prev_branch = copy.deepcopy(self.new_opens[branch_index])
                    back_log = copy.deepcopy(traceback)
                    if len(back_log) > 0:
                        back_log.pop()
                    # if name == 'select_tactics':
                    #     print('pause here')
                    first = True
                    for key, offset in switch_entries.items():
                        if first:
                            first = False
                            continue
                        inst_pos = offset
                        inst_ptr = self._get_ptr(pos=offset, pos_list=current_sub['pos_list'])
                        action_switch_dict = {'branched_all': switch_all, 'entries': switch_entries,
                                              'condition': switch_addr,
                                              'selected_entry': key, 'next_inst_ptr': inst_ptr,
                                              'next_inst_pos': current_sub['pos_list'][inst_ptr]}

                        new_branch = copy.deepcopy(self.new_opens[branch_index])

                        if modify:
                            mod_key = list(new_branch['actions'][-1])[0]
                            new_branch['actions'][-1][mod_key]['modification'] = {
                                'switch': copy.deepcopy(action_switch_dict)}
                        else:
                            new_branch['actions'].append(
                                {'switch': copy.deepcopy(action_switch_dict)})

                        if self.with_compare_assumption:
                            new_branch['switch_states'].append({'address': switch_addr, 'entry': key})

                        new_branch_index = len(self.new_opens)
                        self.new_opens.append(new_branch)
                        new_ram = copy.deepcopy(self.new_opens[new_branch_index]['cur_ram'])

                        sub_hit_requested = self._run_subscript_branch(name=name, subscripts=subscripts, ptr=inst_ptr,
                                                                       ram=new_ram, depth=depth + 1,
                                                                       branch_index=new_branch_index,
                                                                       hit_requested=hit_requested,
                                                                       back_log=copy.deepcopy(back_log),
                                                                       blacklist=blacklist)

                        if not sub_hit_requested:
                            self.false_branches.append(new_branch_index)
                        # if self.debug_verbose:
                        #     suffix = f'req:{hit_requested}'
                        #     print(f'{tabs}\t<-- {suffix}')
                    switch_addr_value = list(switch_entries.keys())[0]
                elif prev_switch_state:
                    switch_addr_value = prev_switch_entry
                else:
                    switch_addr_value = self._get_memory_pos(switch_addr, ram, 'control')

                increment_pointer = False
                if switch_addr_value in switch_entries:
                    entry = switch_entries[switch_addr_value]
                else:
                    entry = switch_entries[-1]
                inst_ptr = self._get_ptr(pos=entry, pos_list=current_sub['pos_list'])
                action_switch_dict = {'branched_all': switch_all, 'entries': switch_entries, 'condition': switch_addr,
                                      'selected_entry': switch_addr_value, 'next_inst_ptr': inst_ptr,
                                      'next_inst_pos': current_sub['pos_list'][inst_ptr]}

                if modify:
                    mod_key = list(self.new_opens[branch_index]['actions'][-1])[0]
                    self.new_opens[branch_index]['actions'][-1][mod_key]['modification'] = {
                        'switch': action_switch_dict}
                else:
                    self.new_opens[branch_index]['actions'].append({'switch': action_switch_dict})

                if self.with_compare_assumption:
                    if force_branch:
                        self.new_opens[branch_index]['switch_states'].append(
                            {'address': switch_addr, 'entry': switch_addr_value})

                current_pointer = inst_ptr
                modify = False

                if self.debug_verbose:
                    suffix = f'req:{hit_requested}'
                    print(f'{tabs}\t<-- {suffix}')

            elif 'choice' in inst:
                if branch_index is None:
                    self.new_opens.append({'init_value': {'instruction': 'all', 'traceback': [{'name': 'Start', 'pos': 0}]}, 'init_ram': copy.deepcopy(cur_ram),
                                           'cur_ram': copy.deepcopy(cur_ram), 'switch_states': [], 'actions': [],
                                           'jump_states': []})
                    branch_index = 0
                force_branch = True
                modify = True
                choice_dict = {'details': inst['choice'], 'children': {}}
                self.new_opens[branch_index]['actions'].append({'choice': choice_dict})

            elif 'subscript_load' in inst:
                next_name = inst['subscript_load']['next']
                load_next = True
                if blacklist is not None:
                    if name in blacklist.keys():
                        if next_name == blacklist[name]:
                            load_next = False

                if load_next:
                    back_log.append({'name': name, 'ptr': current_pointer})
                    next_inst_pos = inst['subscript_load']['location']
                    next_inst_ptr = self._get_ptr(pos=next_inst_pos, pos_list=subscripts[next_name]['pos_list'])
                    traceback[-1]['ptr'] = current_pointer
                    traceback.append({'name': next_name, 'ptr': next_inst_ptr})
                    name = next_name
                    current_sub = subscripts[next_name]
                    current_pointer = next_inst_ptr
                    increment_pointer = False
                    # if branch_index is not None:
                    #     new_branch = self._branch_append_node(branch=self.open_branch_segments[branch_index],
                    #                                           node_key='subscript', node_value=copy.deepcopy(sub_dict))
                    #     self.open_branch_segments[branch_index] = new_branch
                    # if self.debug_verbose:
                    #     suffix = f'req:{hit_requested}'
                    #     print(f'{tabs}\t<-- {suffix}')

            if increment_pointer:
                current_pointer += 1
                traceback[cur_trace_id]['ptr'] = current_pointer
            else:
                increment_pointer = True

            if current_pointer < len(current_sub['pos_list']):
                inst_pos = current_sub['pos_list'][current_pointer]
                inst = current_sub[inst_pos]
            elif len(back_log) > 0:
                backtrack_successful = False
                while not backtrack_successful:
                    traceback.pop()
                    checkpoint = back_log.pop()
                    new_name = checkpoint['name']
                    new_ptr = checkpoint['ptr']
                    name = new_name
                    current_pointer = new_ptr + 1
                    current_sub = subscripts[new_name]
                    if current_pointer < len(current_sub['pos_list']):
                        inst_pos = current_sub['pos_list'][current_pointer]
                        inst = current_sub[inst_pos]
                        backtrack_successful = True
                    elif len(back_log) == 0:
                        done = True
                        backtrack_successful = True
            elif 'fallthrough' in current_sub.keys():
                name = current_sub['fallthrough']
                current_sub = subscripts[name]
                current_pointer = 0
                inst_pos = current_sub['pos_list'][current_pointer]
                inst = current_sub[inst_pos]
                traceback[-1] = {'name': name, 'ptr': current_pointer}
                # if self.debug_verbose:
                #     suffix = f'req:{hit_requested}'
                #     print(f'{tabs}\t<-- {suffix}')
            else:
                done = True

        if branch_index is not None:
            self.new_opens[branch_index]['cur_ram'] = copy.deepcopy(cur_ram)
        return hit_requested

    @staticmethod
    def _get_ptr(pos, pos_list):
        ptr = 0
        if pos == 0:
            return ptr
        if pos in pos_list:
            return int(pos_list._index(pos))
        else:
            if pos > pos_list[-1]:
                return len(pos_list)
            else:
                for i, p in enumerate(pos_list):
                    if p > pos:
                        return i

    @staticmethod
    def _convert_traceback_to_pos(traceback, subscripts) -> List[Dict]:
        new_trace = []
        for trace in traceback:
            trace_pos_list = subscripts[trace['name']]['pos_list']
            ptr = trace.pop('ptr')
            if ptr < len(trace_pos_list):
                trace_pos = trace_pos_list[ptr]
            else:
                trace_pos = trace_pos_list[-1]
            new_trace.append({**trace, 'pos': trace_pos})

        return new_trace

    def _make_all_out_summary(self, inst_details, temp_dir, all_branches) -> dict:
        num_branches = len(all_branches)

        parallel_processes = False
        if ((num_branches * num_branches) / 2) > self.chunk_compare_num:
            parallel_processes = self.use_multiprocessing

        # Group branches by: Inst, Inst_value, Subscript, Position
        print('Grouping Branches...')
        groups = {}
        all_insts = []
        for i, branch in enumerate(all_branches):
            inst = branch['init_value']['instruction']
            if isinstance(inst, str):
                if inst == 'all':
                    all_insts.append(branch)
                    continue

            if inst not in groups.keys():
                groups[inst] = {}

            start_check = branch['init_value']['traceback'][-1]['name']
            if start_check == 'Start':
                value = 'Start'
            else:
                value = branch['init_value']['parameter values'][inst_details[inst]]
                if isinstance(value, str):
                    addr = value
                    value = branch['init_value']['address_values'][addr]

            if value not in groups[inst].keys():
                groups[inst][value] = []
            groups[inst][value].append(branch)

        for inst, values in groups.items():
            for i, branch in enumerate(all_insts):

                start_check = branch['init_value']['traceback'][-1]['name']
                if start_check == 'Start':
                    value = 'Start'
                else:
                    value = branch['init_value']['parameter values'][inst_details[inst]]
                    if isinstance(value, str):
                        addr = value
                        value = branch['init_value']['address_values'][addr]

                if value not in values.keys():
                    groups[inst][value] = []
                groups[inst][value].append(branch)

        grouped_branches = {}
        all_trace_levels = {}
        all_args = []
        for inst, values in groups.items():
            grouped_branches[inst] = {}
            all_trace_levels[inst] = {}
            if not parallel_processes:
                for value, branches in values.items():
                    args_in = {'inst': inst, 'value': value, 'branches': branches}
                    result = self._get_traceback_groups(args_in=args_in)
                    grouped_branches[inst][value] = result['groups']
                    all_trace_levels[inst][value] = result['trace_level']
            else:
                # place holder for debugging multiprocessing, not formatted for multiprocessing
                for value, branches in values.items():
                    grouped_branches[inst][value] = {}
                    all_args.append({'inst': inst, 'value': value, 'branches': branches})

        if parallel_processes:
            pool = mp.Pool(mp.cpu_count())
            results = pool.map(self._get_traceback_groups, all_args)
            pool.close()
            pool.join()

            for out in results:
                grouped_branches[out['inst']][out['value']] = out['groups']
                all_trace_levels[out['inst']][out['value']] = out['trace_level']

        # calculate the number of calcs per worker
        total_calc_num = 0
        group_calc_num = {}
        for inst in grouped_branches:
            group_calc_num[inst] = {}
            for value in grouped_branches[inst]:
                group_calc_num[inst][value] = {}
                for trace, branches in grouped_branches[inst][value].items():
                    branch_num = len(branches)
                    cur_calc_num = (branch_num * branch_num) / 2
                    group_calc_num[inst][value][trace] = cur_calc_num
                    total_calc_num += cur_calc_num

        calcs_per_worker = math.ceil(total_calc_num / self.max_chunks)

        # Remove any duplicate closed branches
        duplicates = {}
        all_args = []
        for inst, values in grouped_branches.items():
            duplicates[inst] = {}
            for value, traces in values.items():
                duplicates[inst][value] = {}
                if not parallel_processes:
                    for trace, branches in traces.items():
                        first_index = 0
                        last_index = len(branches) - 1
                        args_in = {'inst': inst, 'value': value, 'branches': branches, 'start_index': first_index,
                                   'trace': trace, 'last_index': last_index}
                        out = self._remove_traceback_duplicate_branches(args_in=args_in)
                        duplicates[inst][value][trace] = out['duplicates']
                else:
                    for trace, branches in traces.items():
                        branch_num_for_index_calc = len(branches) - 1
                        compare_num = group_calc_num[inst][value][trace]
                        chunks = floor(compare_num / calcs_per_worker) + 1
                        compares_per_chunk = round(compare_num / chunks)
                        last_index = -1
                        for i in range(chunks):
                            first_index = last_index + 1
                            quadratic_sqrt = math.sqrt((branch_num_for_index_calc * branch_num_for_index_calc) - (
                                    4 * (-1 / 2) * compares_per_chunk))
                            numerator = abs((branch_num_for_index_calc * -1) + quadratic_sqrt)
                            if numerator == 0:
                                numerator = abs((branch_num_for_index_calc * -1) - quadratic_sqrt)
                            index_num_for_chunk = math.ceil(abs(numerator / (2 * (-1 / 2))))
                            last_index = first_index + index_num_for_chunk
                            all_args.append({'inst': inst, 'value': value, 'branches': branches, 'trace': trace,
                                             'start_index': first_index, 'last_index': last_index})
                            branch_num_for_index_calc -= (last_index - first_index) + 1
                        all_args[-1]['last_index'] = len(branches) - 1
                        duplicates[inst][value][trace] = []

        if parallel_processes:
            pool = mp.Pool(mp.cpu_count())
            results = pool.map(self._remove_traceback_duplicate_branches, all_args)
            pool.close()
            pool.join()

            for i, out in enumerate(results):
                duplicates[out['inst']][out['value']][out['trace']] = [
                    *duplicates[out['inst']][out['value']][out['trace']],
                    *out['duplicates']]

        for inst in duplicates.keys():
            for value in duplicates[inst].keys():
                for trace, dups in duplicates[inst][value].items():
                    dups = sorted(list(set(dups)))
                    for dup in reversed(dups):
                        grouped_branches[inst][value][trace].pop(dup)

        # recalculate the number of calcs per worker with duplicates removed
        total_calc_num = 0
        group_calc_num = {}
        for inst in grouped_branches:
            group_calc_num[inst] = {}
            for value in grouped_branches[inst]:
                group_calc_num[inst][value] = {}
                for trace, branches in grouped_branches[inst][value].items():
                    branch_num = len(branches)
                    cur_calc_num = (branch_num * branch_num) / 2
                    group_calc_num[inst][value][trace] = cur_calc_num
                    total_calc_num += cur_calc_num

        # Remove anything that is in the temporary directory in case anything was left over from a previous run
        for d in os.listdir(temp_dir):
            path = os.path.join(temp_dir, d)
            if os.path.isdir(path):
                shutil.rmtree(path)
            else:
                os.remove(path)

        print('\nLooking for branch differences')
        all_args = []
        difference_dicts = {}
        all_outs = {}
        all_diff_levels = {}
        all_internals = {}
        for inst, values in grouped_branches.items():
            difference_dicts[inst] = {}
            all_outs[inst] = {}
            all_diff_levels[inst] = {}
            all_internals[inst] = {}
            for value, traces in values.items():
                difference_dicts[inst][value] = {}
                all_outs[inst][value] = {}
                all_diff_levels[inst][value] = {}
                all_internals[inst][value] = []
                if not parallel_processes:
                    for trace, branches in traces.items():
                        i = 0
                        first_index = 0
                        last_index = len(branches) - 1
                        args_in = {'inst': inst, 'value': value, 'branches': branches, 'first_index': first_index,
                                   'trace': trace, 'inst_details': inst_details[inst], 'last_index': last_index,
                                   'temp_dir': temp_dir, 'file_index': i}
                        out = self._get_traceback_group_details(args_in=args_in)
                        difference_dicts[out['inst']][out['value']][out['trace']] = out
                        if out['internal']:
                            all_internals[inst][value].append(trace)
                else:
                    for trace, branches in traces.items():
                        branch_num_for_index_calc = len(branches) - 1
                        compare_num = group_calc_num[inst][value][trace]
                        chunks = floor(compare_num / calcs_per_worker) + 1
                        compares_per_chunk = round(compare_num / chunks)
                        last_index = -1
                        for i in range(chunks):
                            first_index = last_index + 1
                            quadratic_sqrt = math.sqrt((branch_num_for_index_calc * branch_num_for_index_calc) - (
                                    4 * (-1 / 2) * compares_per_chunk))
                            numerator = abs((branch_num_for_index_calc * -1) + quadratic_sqrt)
                            if numerator == 0:
                                numerator = abs((branch_num_for_index_calc * -1) - quadratic_sqrt)
                            index_num_for_chunk = math.ceil(abs(numerator / (2 * (-1 / 2))))
                            last_index = first_index + index_num_for_chunk
                            all_args.append({'inst': inst, 'value': value, 'branches': branches, 'trace': trace,
                                             'inst_details': inst_details[inst], 'temp_dir': temp_dir, 'file_index': i,
                                             'first_index': first_index, 'last_index': last_index})
                            branch_num_for_index_calc -= (last_index - first_index) + 1
                        all_args[-1]['last_index'] = len(branches) - 1

        if parallel_processes:
            pool = mp.Pool(mp.cpu_count())
            results = pool.map(self._get_traceback_group_details, all_args)
            pool.close()
            pool.join()

            for i, out in enumerate(results):
                if out['trace'] not in difference_dicts[out['inst']][out['value']].keys():
                    difference_dicts[out['inst']][out['value']][out['trace']] = copy.deepcopy(out)
                else:
                    cur_out = difference_dicts[out['inst']][out['value']][out['trace']]
                    cur_out['diff_levels'] = sorted(list({*cur_out['diff_levels'], *out['diff_levels']}))
                    if not cur_out['has_differences']:
                        cur_out['has_differences'] = out['has_differences']
                    if cur_out['internal']:
                        cur_out['internal'] = out['internal']
                    for level in out['file_sizes']:
                        if level not in cur_out['file_sizes'].keys():
                            cur_out['file_sizes'][level] = out['file_sizes'][level]
                        else:
                            cur_out['file_sizes'][level] += out['file_sizes'][level]
                    difference_dicts[out['inst']][out['value']][out['trace']] = cur_out

            for inst in difference_dicts.keys():
                for value in difference_dicts[inst].keys():
                    for trace in difference_dicts[inst][value].keys():
                        if difference_dicts[inst][value][trace]['internal']:
                            all_internals[inst][value].append(trace)

        # Combine Temp Files - d includes the inst, value, trace, and level
        for d in os.listdir(temp_dir):
            dir_path = os.path.join(temp_dir, d)
            files = []
            filenames = {}
            for f in os.listdir(dir_path):
                new_filename = f'{d}{self.temp_ext}'
                path = os.path.join(temp_dir, new_filename)
                if not os.path.exists(path):
                    new_index = len(files)
                    filenames[new_filename] = new_index
                    files.append(open(path, 'w', encoding='shiftjis'))
                old_file = open(os.path.join(dir_path, f))
                lines = old_file.readlines()
                files[filenames[new_filename]].writelines(lines)
                old_file.close()
            for file in files:
                file.close()

            shutil.rmtree(dir_path)

        print('Making Summaries...')
        summary = {}
        all_args = []
        for inst, values in grouped_branches.items():
            summary[inst] = {}
            for value, traces in values.items():
                summary[inst][value] = {}
                if not parallel_processes:
                    for trace, branches in traces.items():
                        args_in = {**difference_dicts[inst][value][trace], 'branches': branches, 'temp_dir': temp_dir,
                                   'trace_level': all_trace_levels[inst][value]}

                        result = [self._generate_difference_summaries(args_in=args_in)]
                        for out in result:
                            summary[out['inst']][out['value']][out['trace']] = out['summary']
                else:
                    # place holder for debugging multiprocessing, not formatted for multiprocessing
                    for trace, branches in traces.items():
                        all_args.append({**difference_dicts[inst][value][trace], 'branches': branches,
                                         'temp_dir': temp_dir, 'trace_level': all_trace_levels[inst][value]})
        if parallel_processes:
            pool = mp.Pool(mp.cpu_count())
            results = pool.map(self._generate_difference_summaries, all_args)
            pool.close()
            pool.join()
            for out in results:
                summary[out['inst']][out['value']][out['trace']] = out['summary']

        # Clean up any temp files
        for f in os.listdir(temp_dir):
            os.remove(os.path.join(temp_dir, f))

        # Remove internal summaries and append them to external summaries
        print('Appending internal summaries to their external counterparts...')
        appended_summary = {}
        first = True
        for int_inst, int_values in all_internals.items():
            for int_value, int_traces in int_values.items():
                for int_trace in int_traces:
                    int_summary = summary[int_inst][int_value][int_trace]
                    if first:
                        temp_summary = summary
                        first = False
                    else:
                        temp_summary = appended_summary
                    appended_summary = {}
                    for sum_inst, sum_values in temp_summary.items():
                        for sum_value, sum_traces in sum_values.items():
                            for sum_trace, cur_summary in sum_traces.items():
                                same_val = int_value == sum_value
                                same_trace = int_trace == sum_trace
                                if same_trace and same_val:
                                    continue
                                int_traceback = grouped_branches[int_inst][int_value][int_trace][0]['init_value'][
                                    'traceback']
                                keys = ('stratified', 'condensed')
                                new_sum = {k: v for k, v in cur_summary.items() if k not in keys}
                                for key in keys:
                                    if 'out' in int_summary.keys():
                                        int_sum = {'inst': 'out', 'out': copy.deepcopy(int_summary['out'])}
                                    else:
                                        int_sum = copy.deepcopy(int_summary[key])

                                    if 'out' in cur_summary.keys():
                                        cur_sum = {'inst': 'out', 'out': copy.deepcopy(cur_summary['out'])}
                                    else:
                                        cur_sum = copy.deepcopy(cur_summary[key])

                                    temp_sum = self._append_int_summary_to_externals(int_sum, cur_sum, int_traceback)
                                    new_sum[key] = temp_sum

                                if sum_inst not in appended_summary.keys():
                                    appended_summary[sum_inst] = {}
                                if sum_value not in appended_summary[sum_inst].keys():
                                    appended_summary[sum_inst][sum_value] = {}
                                if sum_trace not in appended_summary[sum_inst][sum_value].keys():
                                    appended_summary[sum_inst][sum_value][sum_trace] = {}

                                appended_summary[sum_inst][sum_value][sum_trace] = new_sum
        if len(appended_summary) == 0:
            appended_summary = summary

        return appended_summary

    def _get_traceback_groups(self, args_in):
        inst = args_in['inst']
        value = args_in['value']
        branches = args_in['branches']
        grouped_values = {}
        traceback_level = len(branches[0]['init_value']['traceback']) - 1
        branch_num = len(branches)
        progress_prefix = f'Creating Groups for {inst}:{value}'
        progress_bar_length = 150 - len(progress_prefix)
        unique_tracebacks = []

        for i, branch in enumerate(branches):
            print_suffix = f'{i}/{branch_num}'
            printProgressBar(prefix=progress_prefix, suffix=print_suffix, length=progress_bar_length,
                             total=branch_num, iteration=i)
            sys.stdout.flush()
            test_traceback = branch['init_value']['traceback']
            add_traceback = True
            for trace in unique_tracebacks:
                if self._variables_are_equal_recursive(test_traceback, trace):
                    add_traceback = False
            if add_traceback:
                unique_tracebacks.append(test_traceback)
        compared = []

        for i, trace1 in enumerate(unique_tracebacks):
            compared.append(i)
            for j, trace2 in enumerate(unique_tracebacks):
                if j in compared:
                    continue
                new_trace_level = self._get_traceback_diff_level(trace1, trace2)
                traceback_level = min(traceback_level, new_trace_level)

        for branch in branches:
            diff_traceback = branch['init_value']['traceback']
            trace_key = self._get_traceback_string(diff_traceback, traceback_level)
            if trace_key not in grouped_values.keys():
                print(f'Created group: Value: {value}, Trace: {trace_key}{" " * 150}')
                grouped_values[trace_key] = []
            grouped_values[trace_key].append(branch)

        return {'groups': grouped_values, 'trace_level': traceback_level, 'inst': inst, 'value': value}

    def _get_traceback_diff_level(self, traceback1, traceback2):
        trace1_range = len(traceback1) - 1
        trace2_range = len(traceback2) - 1
        if not trace1_range == trace2_range:
            level = min(trace1_range, trace2_range)
        else:
            level = trace1_range
            level_found = False
            if self._variables_are_equal_recursive(traceback1, traceback2):
                return trace1_range
            while not level_found:
                if not self._variables_are_equal_recursive(traceback1[level], traceback2[level]):
                    break
                level -= 1
                if level == 0:
                    level_found = True
        return level

    @classmethod
    def get_traceback_string(cls, traceback, level):
        return cls._get_traceback_string(traceback, level)

    @staticmethod
    def _get_traceback_string(traceback, level):
        if level >= len(traceback):
            level = len(traceback) - 1

        trace_key = ''
        first_tr = True
        for trace in traceback[level:]:
            if first_tr:
                first_tr = False
            else:
                trace_key += ','
            first_val = True
            for trace_value in trace.values():
                if first_val:
                    first_val = False
                else:
                    trace_key += '-'
                trace_key = f'{trace_key}{trace_value}'

        return trace_key

    def _remove_traceback_duplicate_branches(self, args_in) -> dict:
        inst = args_in['inst']
        value = args_in['value']
        trace = args_in['trace']
        start_index = args_in['start_index']
        last_index = args_in['last_index']
        branches = args_in['branches']
        branches_to_remove = []
        current_open_branch_num = last_index - start_index + 1
        print_prefix = 'Flagging identical open branches'
        print_suffix = f'total_branches: {current_open_branch_num}'
        print_length = 200 - len(print_prefix)
        if last_index == -1:
            current_open_branch_num = len(branches) - start_index
        for i, open1 in enumerate(branches[start_index:last_index]):
            printProgressBar(prefix=print_prefix,
                             suffix=print_suffix,
                             total=current_open_branch_num, iteration=i, length=print_length)
            sys.stdout.flush()
            actions1 = open1['actions']
            for j, open2 in enumerate(branches[start_index + i + 1:]):
                ind2 = start_index + i + 1 + j
                actions2 = open2['actions']
                if self._variables_are_equal_recursive(actions1, actions2):
                    branches_to_remove.append(ind2)
        printProgressBar(prefix=print_prefix, suffix=print_suffix,
                         total=current_open_branch_num, iteration=current_open_branch_num, length=print_length)

        return {'duplicates': branches_to_remove, 'inst': inst, 'value': value, 'trace': trace}

    def _get_traceback_group_details(self, args_in) -> dict:
        inst = args_in['inst']
        value = args_in['value']
        trace = args_in['trace']
        param_name = args_in['inst_details']
        branches = args_in['branches']
        temp_dir = args_in['temp_dir']
        index = args_in['file_index']
        first_index = args_in['first_index']
        last_index = args_in['last_index']

        branch_num = last_index - first_index + 1
        if last_index == -1:
            branch_num = len(branches) - first_index
        progress_prefix = f'\rGetting first differences from {inst}:{value}:{trace}'
        progress_bar_length = 200 - len(progress_prefix)

        # Determine whether this position is internal and get outs for each branch
        is_internal = True
        branch_outs = []

        for branch in branches:
            if isinstance(value, str):
                if value == 'Start':
                    is_internal = False
            elif 'init' in trace or 'new_run' in branch.keys():
                is_internal = False

            if 'exit' in branch.keys():
                out_value = 'END'
            else:
                out_value = branch['out_value']['parameter values'][param_name]
                if isinstance(out_value, str):
                    addr = out_value
                    out_value = branch['out_value']['address_values'][addr]
            out_trace = branch['out_value']['traceback']
            branch_outs.append({'value': out_value, 'traceback': out_trace, 'exit': True})

        diffs_file_header = f'{inst}-{value}-{trace}'
        diff_levels = []
        has_differences = False
        dir_index = {}
        files = []
        level_sizes = {}
        if branch_num > 1:
            for i, branch1 in enumerate(branches[first_index: last_index]):
                ind1 = i + first_index
                progress_suffix = f' \t{i}/{branch_num}'
                printProgressBar(prefix=f'{progress_prefix}', suffix=f'{progress_suffix}',
                                 length=progress_bar_length, total=branch_num, iteration=i, printEnd='\r')
                sys.stdout.flush()
                j_first_index = first_index + i + 1
                for j, branch2 in enumerate(branches[j_first_index:]):
                    ind2 = j_first_index + j
                    if self.debug_verbose:
                        print(f'\tGetting first difference between {i} and {j}')
                    temp_difference = self._get_first_difference(branch1=branch1,
                                                                 branch2=branch2,
                                                                 top_level=True)
                    if len(temp_difference) == 0:
                        continue
                    diff_level = temp_difference.pop('level')
                    diff_levels.append(diff_level)
                    dir_name = f'{diffs_file_header}-{diff_level}'
                    dir_path = os.path.join(temp_dir, dir_name)

                    if diff_level not in level_sizes.keys():
                        if not os.path.exists(dir_path):
                            try:
                                os.mkdir(dir_path)
                            except FileExistsError as e:
                                print(f'Tried to make dir but already exists {e}')
                        diffs_file_name = f'{dir_name}-{index}{self.temp_ext}'
                        new_file_index = len(files)
                        dir_index[diff_level] = new_file_index
                        files.append(open(os.path.join(dir_path, diffs_file_name), 'w'))
                        level_sizes[diff_level] = 0

                    deets = temp_difference.pop('diff_deets')
                    difference = {'branches': [ind1, ind2], 'level': diff_level, 'diff': temp_difference,
                                  'diff_details': deets}
                    files[dir_index[diff_level]].write(f'{json.dumps(difference)}\n')
                    level_sizes[diff_level] += 1
                    has_differences = True

            progress_suffix = ' \tDONE\t\t\t\t '
            printProgressBar(prefix=f'{progress_prefix}', suffix=f'{progress_suffix}',
                             total=branch_num, iteration=branch_num, length=progress_bar_length, printEnd='\r')
            sys.stdout.flush()

            diff_levels = sorted(list(set(diff_levels)))

        for file in files:
            file.close()

        output = {'outs': branch_outs, 'diff_levels': diff_levels, 'internal': is_internal, 'inst': inst,
                  'value': value, 'trace': trace, 'diffs_header': diffs_file_header, 'has_differences': has_differences,
                  'file_sizes': level_sizes}

        return output

    def _get_first_difference(self, branch1, branch2, top_level=False, level=0) -> dict:
        if self.use_actions:
            difference = {}
            for i, action1 in enumerate(branch1['actions']):
                if i >= len(branch2['actions']):
                    return {}
                action2 = branch2['actions'][i]
                action1_key = list(action1.keys())[0]
                action2_key = list(action2.keys())[0]

                action_is_same = self._variables_are_equal_recursive(action1, action2)

                if not action_is_same:
                    sub_difference = self._get_difference_details(action1, action2)
                    deets1 = action1[action1_key]
                    deets2 = action2[action2_key]
                    difference = {'level': i, 'diff_deets': [deets1, deets2], **sub_difference}
                    break

        else:
            if 'children' not in branch1.keys() or 'children' not in branch2.keys():
                if self.debug_verbose:
                    print('\t\tNo children or difference present...')
                return {}

            ch1_keys = list(branch1['children'].keys())
            ch2_keys = list(branch2['children'].keys())
            rest1 = {k: branch1[k] for k in branch1.keys() if not k == 'children'}
            rest2 = {k: branch2[k] for k in branch2.keys() if not k == 'children'}
            if top_level:
                action_is_same = True
            else:
                action_is_same = self._variables_are_equal_recursive(rest1, rest2)
                if action_is_same:
                    action_is_same = self._variables_are_equal_recursive(ch1_keys, ch2_keys)

            if action_is_same:
                next_branch1 = branch1['children'][ch1_keys[0]]
                next_branch2 = branch2['children'][ch2_keys[0]]
                temp_diff = self._get_first_difference(next_branch1, next_branch2, level=(level + 1))

                if 'children' in temp_diff.keys():
                    temp_diff.pop('children')

                if 'insert_key' in temp_diff.keys():
                    temp_diff.pop('insert_key')
                    level = temp_diff.pop('level')
                    deets = temp_diff.pop('diff_deets')
                    difference = {ch2_keys[0]: temp_diff, 'level': level, 'diff_deets': deets}
                else:
                    difference = temp_diff

            else:
                difference = self._get_difference_details(branch1, branch2)
                difference['level'] = level
                deets1 = {k: branch1[k] for k in branch1.keys() if not k == 'children'}
                deets2 = {k: branch2[k] for k in branch2.keys() if not k == 'children'}
                difference['diff_deets'] = [deets1, deets2]
                difference['insert_key'] = True

        return difference

    def _get_difference_details(self, var1, var2):
        diff = {}
        if not type(var1) == type(var2):
            type1 = type(var1).__name__
            type2 = type(var2).__name__
            diff = {'var1': copy.deepcopy(var1), 'var2': copy.deepcopy(var2), 'var1_type': copy.deepcopy(type1),
                    'var2_type': copy.deepcopy(type2)}

        elif isinstance(var1, dict):
            for key in var1:
                if key not in var2.keys():
                    return {'var1': copy.deepcopy(var1), 'var2': copy.deepcopy(var2), 'key': copy.deepcopy(key)}
            for key, var1_value in var1.items():
                var2_value = var2[key]
                sub_diff = self._get_difference_details(var1_value, var2_value)
                if not len(sub_diff) == 0:
                    diff[key] = sub_diff

        elif isinstance(var1, list):
            if not len(var1) == len(var2):
                diff = [var1, var2]
            else:
                for i in range(len(var1)):
                    var1_value = var1[i]
                    var2_value = var2[i]
                    sub_diff = self._get_difference_details(var1_value, var2_value)
                    if not len(sub_diff) == 0:
                        diff[i] = sub_diff

        else:
            if not var1 == var2:
                return {'var1': copy.deepcopy(var1), 'var2': copy.deepcopy(var2)}
            else:
                return {}

        return diff

    def _generate_difference_summaries(self, args_in) -> dict:
        diff_levels = args_in['diff_levels']
        dif_header = args_in['diffs_header']
        file_sizes = args_in['file_sizes']
        branch_outs = args_in['outs']
        trace_level = args_in['trace_level']
        branches = args_in['branches']
        inst = args_in['inst']
        value = args_in['value']
        trace = args_in['trace']
        has_differences = args_in['has_differences']

        # print(f'Summarizing {value}-{trace}...')
        if len(branches) > 1:
            if has_differences:

                stratified_diff_summary = self._get_stratified_differences(
                    dif_header=dif_header, outs=copy.deepcopy(branch_outs), value=value, trace=trace,
                    diff_levels=copy.deepcopy(diff_levels), temp_dir=args_in['temp_dir'], file_sizes=file_sizes)

                condensed_diff_summary = self._condense_stratified_differences(
                    copy.deepcopy(stratified_diff_summary))

                summary = {'has_diff': True, 'stratified': stratified_diff_summary,
                           'condensed': condensed_diff_summary, 'trace_level': trace_level}

            else:
                out = branch_outs[0]
                summary = {'has_diff': False, 'out': out, 'unused_trees': True,
                           'trace_level': trace_level}
        else:
            out = branch_outs[0]
            summary = {'has_diff': False, 'out': out, 'trace_level': trace_level}

        return {'summary': summary, 'inst': inst, 'value': value, 'trace': trace}

    def _get_stratified_differences(self, outs, diff_levels, temp_dir, dif_header, file_sizes, value, trace,
                                    valid_ids=None) -> dict:
        strat_diff = {}
        rem_levels = diff_levels[1:]
        level = diff_levels[0]
        child_ids = {}
        progress_prefix = f'Summarizing {dif_header}:{level}'
        progressbar_length = 200 - len(progress_prefix)
        cur_line = 0
        dif_file_name = f'{dif_header}-{level}{self.temp_ext}'
        diff_path = os.path.join(temp_dir, dif_file_name)
        file = open(diff_path, 'r')
        while True:
            line = file.readline()
            if not line:
                break
            diff: dict = json.loads(line)
            if not diff['level'] == level:
                continue
            b_keys = diff['branches']
            invalid_id_detected = False
            if valid_ids is not None:
                for key in b_keys:
                    if key not in valid_ids:
                        invalid_id_detected = True

            if len(b_keys) < 2:
                print('Stratification Error: Invalid number of branches to form a difference')
                continue
            if invalid_id_detected:
                continue

            diff_inst = list(diff['diff'].keys())[0]
            diff_dict = diff['diff'][diff_inst]

            if diff_inst == 'choice':
                if 'jumpif' in diff_dict['modification']:
                    b_diff_jmp_value_dict = diff_dict['modification']['jumpif']['jumped']
                elif 'switch' in diff_dict['modification']:
                    b_diff_jmp_value_dict = diff_dict['modification']['switch']['selected_entry']

                b_diff_values = [b_diff_jmp_value_dict['var1'], b_diff_jmp_value_dict['var2']]

                choice_details = diff['diff_details'][0]['details']
                options = choice_details['choices']
                strat_diff['inst'] = 'choice'
                strat_diff['question'] = choice_details['question']
                for i, value in enumerate(b_diff_values):
                    if isinstance(value, bool):
                        if value:
                            entry = 1
                        else:
                            entry = 0
                    else:
                        entry = int(value)
                    if not options[entry] in child_ids.keys():
                        child_ids[options[entry]] = []
                    b_id = b_keys[i]
                    if b_id not in child_ids[options[entry]]:
                        child_ids[options[entry]].append(b_id)

            elif diff_inst == 'jumpif':
                details1 = diff['diff_details'][0]
                details2 = diff['diff_details'][1]

                jumped1 = details1['jumped']
                option1 = f'{not jumped1}'
                if 'address_dict' in details1:
                    for addr, value in details1['address_dict'].items():
                        option1 += f'-{addr}:{value}'

                jumped2 = details2['jumped']
                option2 = f'{not jumped2}'
                if 'address_dict' in details2:
                    for addr, value in details2['address_dict'].items():
                        option2 += f'-{addr}:{value}'

                strat_diff['inst'] = 'jumpif'

                if 'results' not in strat_diff:
                    strat_diff['results'] = []
                if option1 not in strat_diff['results']:
                    strat_diff['results'].append(option1)
                if option2 not in strat_diff['results']:
                    strat_diff['results'].append(option2)
                condition_key = list(diff['diff_details'][0]['condition'].keys())[0]
                if re.match('0x', condition_key):
                    condition = self._generate_condition_string(cond_in=details1['condition'][condition_key])
                else:
                    condition = self._generate_condition_string(cond_in=details1['condition'])
                strat_diff['condition'] = condition

                if option1 not in child_ids.keys():
                    child_ids[option1] = []
                b_id = b_keys[0]
                if b_id not in child_ids[option1]:
                    child_ids[option1].append(b_id)

                if option2 not in child_ids.keys():
                    child_ids[option2] = []
                b_id = b_keys[1]
                if b_id not in child_ids[option2]:
                    child_ids[option2].append(b_id)

                child_ids = {k: child_ids[k] for k in sorted(list(child_ids.keys()))}

            elif diff_inst == 'switch':
                details = diff['diff_details'][0]
                b_diff_jmp_value_dict = diff_dict['selected_entry']
                found_values = True
                for ind in ('var1', 'var2'):
                    if ind not in b_diff_jmp_value_dict.keys():
                        found_values = False

                b_diff_values = []
                if not found_values:
                    continue
                else:
                    b_diff_values.append(b_diff_jmp_value_dict['var1'])
                    b_diff_values.append(b_diff_jmp_value_dict['var2'])

                strat_diff['inst'] = 'switch'
                condition = details['condition']
                strat_diff['condition'] = condition
                for i, value in enumerate(b_diff_values):
                    if isinstance(value, bool):
                        if value:
                            entry = 0
                        else:
                            entry = 1
                        print('\t\tswitch w/ a boolean???')
                    else:
                        entry = int(value)
                    if entry not in child_ids.keys():
                        child_ids[entry] = []
                    b_id = b_keys[i]
                    if b_id not in child_ids[entry]:
                        child_ids[entry].append(b_id)
                for child_list in child_ids.values():
                    if len(child_list) == 0:
                        print('error')
            else:
                print(f'make new category: {diff_inst}')

            printProgressBar(prefix=progress_prefix, suffix=f'{cur_line}/{file_sizes[level]}',
                             length=progressbar_length, total=file_sizes[level], iteration=cur_line, printEnd='\r')
            sys.stdout.flush()
            cur_line += 1
        file.close()

        printProgressBar(prefix=progress_prefix, suffix=f'\tDONE{" " * 25}', length=progressbar_length,
                         total=file_sizes[level], iteration=file_sizes[level], printEnd='')
        sys.stdout.flush()

        temp_children = child_ids
        if len(temp_children) == 0:
            if len(rem_levels) > 0:
                return self._get_stratified_differences(outs=outs, diff_levels=rem_levels, valid_ids=valid_ids,
                                                        temp_dir=temp_dir, dif_header=dif_header, file_sizes=file_sizes,
                                                        value=value, trace=trace)
            else:
                cur_outs = []
                for id in valid_ids:
                    cur_outs.append(outs[id])
                output = cur_outs[0]
                out_dict = {'inst': 'out', 'out': output, 'multiple_outs': True}
                # print('no children here?')
                return out_dict

        for key, option_list in child_ids.items():
            if not len(option_list) == 1:
                if len(rem_levels) > 0:
                    out_dict = self._get_stratified_differences(outs=outs, diff_levels=rem_levels,
                                                                valid_ids=option_list, temp_dir=temp_dir,
                                                                dif_header=dif_header, file_sizes=file_sizes,
                                                                value=value, trace=trace)
                else:
                    cur_outs = []
                    for option in option_list:
                        cur_outs.append(outs[option])
                    output = cur_outs[0]
                    out_dict: dict = {'inst': 'out', 'out': output, 'multiple_outs': True}
            else:
                if len(option_list) < 1 or len(outs) < option_list[0]:
                    print('stop here')
                output = outs[option_list[0]]
                out_dict: dict = {'inst': 'out', 'out': output}

            if out_dict is None:
                print('out dict wasnt made??')
            temp_children[key] = out_dict
            if len(out_dict) == 0:
                print('out_dict is empty?')

        strat_diff['options'] = temp_children

        return strat_diff

    def _generate_condition_string(self, cond_in, top_level=True):
        string = ''
        if isinstance(cond_in, str) and top_level:
            return cond_in
        for cond, cond_dict in cond_in.items():
            temp_cond = cond
            values = {}
            for cond_id, value in cond_dict.items():
                if isinstance(value, dict):
                    substring = self._generate_condition_string(cond_in=value, top_level=False)
                    values[cond_id] = substring
                else:
                    values[cond_id] = value

            for key, value in values.items():
                if isinstance(value, str):
                    if ': ' in value:
                        cond_value = value.split(': ')[1].rstrip()
                    else:
                        cond_value = value
                else:
                    cond_value = str(value)
                replace_key = f'({key})'
                temp_cond = temp_cond.replace(replace_key, cond_value)

            string += f'{temp_cond}'
            if not top_level:
                string = f'({string})'

        return string

    def _condense_stratified_differences(self, diffs) -> dict:
        current_diffs = copy.deepcopy(diffs)
        if 'options' in diffs.keys():
            options = {}
            for selection, result in diffs['options'].items():
                if 'inst' not in result.keys():
                    print('condense_error - no inst in result')
                if not result['inst'] == 'out':
                    next_level = self._condense_stratified_differences(result)
                    options[selection] = next_level
                else:
                    if len(result) == 0:
                        print('condense_error - empty result')
                    options[selection] = result

            if len(options) == 0:
                print('condense_error - options is empty')

            last_option = None
            all_options_same = True
            for key, out in options.items():
                cur_option = out
                if all_options_same:
                    if last_option is not None:
                        if not self._variables_are_equal_recursive(cur_option, last_option):
                            all_options_same = False
                last_option = cur_option

            if all_options_same:
                option_key = list(options.keys())[0]
                condensed_dict = options[option_key]
            else:
                current_diffs.pop('options')
                condensed_dict = {**current_diffs, 'options': options}

        else:
            condensed_dict = current_diffs

        return condensed_dict

    def _append_int_summary_to_externals(self, int_sum, cur_sum, int_traceback):

        if not cur_sum['inst'] == 'out':
            if 'options' not in cur_sum.keys():
                print('append_error - no options available, cannot continue')
                return cur_sum
            new_sum = {k: v for k, v in cur_sum.items() if not k == 'options'}
            options = {}
            for option, tree in cur_sum['options'].items():
                new_option = self._append_int_summary_to_externals(int_sum, tree, int_traceback)
                options[option] = new_option
            new_sum['options'] = options

        else:
            if len(cur_sum) == 0:
                print('append_error - end is empty')

            if self._variables_are_equal_recursive(int_traceback, cur_sum['out']['traceback']):
                new_sum = copy.deepcopy(int_sum)
            else:
                new_sum = cur_sum

        return new_sum

    # ----------------------------- #
    # Branch manipulation functions #
    # ----------------------------- #

    def _branch_has_same_conditions(self, out_branch, open_branch, with_req=False, with_mid=False):
        is_same = True

        # Compare initial ram state
        same_init_ram = self._variables_are_equal_recursive(open_branch['init_ram'], out_branch['init_ram'])
        if not same_init_ram:
            is_same = False

        # Compare requested instruction at start of branch
        if with_req and is_same:
            same_req = self._variables_are_equal_recursive(open_branch['init_value'], out_branch['init_value'])
            if not same_req:
                is_same = False

        if with_mid and is_same:
            open_mid = open_branch['cur_ram']
            if 'new_run' in out_branch.keys():
                out_mid = out_branch['new_run']['mid_ram'][0]
                same_mid_ram = self._variables_are_equal_recursive(open_mid, out_mid)
                if not same_mid_ram:
                    is_same = False
            else:
                out_mid = out_branch['cur_ram']
                same_mid_ram = self._variables_are_equal_recursive(open_mid, out_mid)
                if not same_mid_ram:
                    is_same = False

        return is_same

    def _variables_are_equal_recursive(self, var1, var2):
        if not type(var1) == type(var2):
            return False
        if isinstance(var1, dict):
            for key in var1:
                if key not in var2.keys():
                    return False
            for key, var1_value in var1.items():
                var2_value = var2[key]
                if not self._variables_are_equal_recursive(var1_value, var2_value):
                    return False
        elif isinstance(var1, list):
            if not len(var1) == len(var2):
                return False
            for i in range(len(var1)):
                var1_value = var1[i]
                var2_value = var2[i]
                if not self._variables_are_equal_recursive(var1_value, var2_value):
                    return False
        else:
            if not var1 == var2:
                return False

        return True

    # ---------------------- #
    # Memory state functions #
    # ---------------------- #

    def _set_memory_pos(self, details: dict, ram):
        addr_type = details['type']
        addr = details['addr']
        if isinstance(addr, float):
            addr = int(addr)
        if isinstance(addr, int):
            addr = str(addr)
        if addr not in ram.keys():
            if addr not in self.addrs.keys():
                self.addrs[addr] = {'addr_not_init': True, 'types': addr_type}
                self.addrs[addr]['addr_not_init'] = True
            if 'init_loc' not in self.addrs[addr].keys():
                self.addrs[addr]['init_loc'] = 'internal'
            ram[addr] = self.addrs[addr]
        cur_value = ram[addr].get('value', None)
        if ram[addr]['init_loc'] == 'external':
            ram[addr]['init_loc'] += '->internal'
        if addr_type == 'bit':
            if details['action'] == 'set':
                ram[addr]['value'] = 1
            elif details['action'] == 'unset':
                ram[addr]['value'] = 0
            elif details['action'] == 'invert':
                if cur_value is None:
                    cur_value = 0
                ram[addr]['value'] = 1 - cur_value
        else:
            ram[addr]['value'] = details['value']
        return ram

    def _get_memory_pos(self, addr, ram, addr_type=None):
        if addr not in ram.keys():
            if addr not in self.addrs.keys():
                self.addrs[addr] = {'addr_not_init': True}
                if addr_type is not None:
                    self.addrs[addr]['types'] = addr_type
            self.addrs[addr]['init_loc'] = 'external'
            cur_value = None
        else:
            cur_value = ram[addr]['value']

        if addr_type is not None:
            if 'types' not in self.addrs[addr].keys():
                self.addrs[addr]['types'] = addr_type
            else:
                cur_type = self.addrs[addr]['types']
                if addr_type not in cur_type:
                    cur_type += f'-{addr_type}'

        return cur_value

    def _get_defined_ram(self, branch=None):
        ram = {}
        if branch is None:
            for addr, addr_dict in self.addrs.items():
                if 'value' in addr_dict.keys():
                    if addr_dict['value'] is not None:
                        ram[addr] = addr_dict
        else:
            ram = branch['current ram']
        return ram

    # ------------ #
    # Jump testing #
    # ------------ #

    def _should_not_jump(self, compare, ram, isBase=False):
        params = []
        comparison = ''
        for short_compare, value in compare.items():
            if isinstance(value, dict):
                for comp, param in value.items():
                    comparison = comp
                    if isinstance(param, dict):
                        for sub_param in param.values():
                            if isinstance(sub_param, dict):
                                params.append(self._should_not_jump(sub_param, ram))
                            else:
                                params.append(sub_param)
                    elif isinstance(param, float) or \
                            isinstance(param, int) or \
                            isinstance(param, str):
                        params.append(param)
                        comparison = short_compare
                    else:
                        print(f'WARNING: unable to process param of type {type(param)}. Not performing jump')
                        return False
            else:
                params.append(value)

        param_values = []
        for param in params:
            if isinstance(param, str):
                done = False
                value = param
                while not done:
                    if re.search(': ', value):
                        addr = value.split(': ')[1].rstrip()
                    else:
                        addr = value
                    value = self._get_memory_pos(addr, ram, 'control')
                    if value is None:
                        done = True
                    elif not isinstance(value, str):
                        done = True
                param_values.append(value)
            else:
                param_values.append(param)
        if len(param_values) > 1:
            try:
                result = self.scpt_codes[comparison](param_values[0], param_values[1])
            except KeyError as e:
                print(f'Error comparison not performed: {e}')
                return False
        else:
            result = param_values[0]

        if isBase:
            if result == 1:
                return True
            else:
                return False
        return result

    def _can_jump(self, compare, ram):
        can_jump = True
        for value in compare.values():
            if isinstance(value, dict):
                if not self._can_jump(value, ram):
                    can_jump = False
            elif isinstance(value, str):
                done = False
                while not done:
                    if re.search(': ', value):
                        addr = value.split(': ')[1].rstrip()
                    else:
                        addr = value
                    value = self._get_memory_pos(addr, ram, 'control')
                    if value is None:
                        can_jump = False
                        done = True
                    elif not isinstance(value, str):
                        done = True
            else:
                is_number = isinstance(value, int) or isinstance(value, float)
                if not is_number:
                    can_jump = False
        return can_jump

    def _can_switch(self, addr, ram):
        value = self._get_memory_pos(addr, ram, 'control')
        if value is not None:
            return True
        return False

    # ------------------------------- #
    # SCPT Analyze Comparison Methods #
    # ------------------------------- #
    # True returns 1, False returns 0

    @staticmethod
    def _is_lt(in_1, in_2):
        result = in_1 < in_2
        if result:
            return 1
        return 0

    @staticmethod
    def _is_leq(in_1, in_2):
        result = in_1 <= in_2
        if result:
            return 1
        return 0

    @staticmethod
    def _is_gt(in_1, in_2):
        result = in_1 > in_2
        if result:
            return 1
        return 0

    @staticmethod
    def _is_geq(in_1, in_2):
        result = in_1 >= in_2
        if result:
            return 1
        return 0

    @staticmethod
    def _is_eq(in_1, in_2):
        result = in_1 == in_2
        if result:
            return 1
        return 0

    @staticmethod
    def _anded(in_1, in_2):
        result = in_1 & in_2
        return result

    @staticmethod
    def _ored(in_1, in_2):
        result = in_1 | in_2
        return result

    @staticmethod
    def _both_true(in_1, in_2):
        if not in_1 == 0 and not in_2 == 0:
            return 1
        return 0

    @staticmethod
    def _either_true(in_1, in_2):
        if not in_1 == 0 or not in_2 == 0:
            return 1
        return 0

    @staticmethod
    def _return_two(in_1, in_2):
        return in_2

    # ------------------------------- #
    # SCPT Analyze Arithmetic Methods #
    # ------------------------------- #

    @staticmethod
    def _multiply(in_1, in_2):
        return in_1 * in_2

    @staticmethod
    def _divide(in_1, in_2):
        return in_1 / in_2

    @staticmethod
    def _mod(in_1, in_2):
        return in_1 % in_2

    @staticmethod
    def _add(in_1, in_2):
        return in_1 + in_2

    @staticmethod
    def _subtract(in_1, in_2):
        return in_1 - in_2

    def _get_dict_depth(self, inp, size=0):
        size += 1
        if isinstance(inp, dict):
            for value in inp.values():
                size = max(self._get_dict_depth(value, size), size)
        return size
