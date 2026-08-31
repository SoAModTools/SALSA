from __future__ import annotations

import enum
from dataclasses import dataclass
from typing import Union, List

from SALSA.Common.script_string_utils import SAstr_to_visible
from SALSA.Common.constants import sep, alt_sep
from SALSA.BaseInstructions.bi_facade import BaseInstLibFacade
from SALSA.Project.project_container import SCTProject


class TokenType (enum.Enum):
    flag = 0
    search = 1
    filter = 2


@dataclass
class Token:
    type_: TokenType
    subtype: Union[None, str]
    value: str

    def __contains__(self, item: Union[TokenType, str]):
        if isinstance(item, str):
            if item == self.subtype:
                return True
            return item in self.value
        return item == self.type_

    def __eq__(self, other):
        if isinstance(other, Token):
            return self.value == other.value and self.type_ == other.type_
        if isinstance(other, TokenType):
            return self.type_ == other
        if isinstance(other, int):
            if self.value.isdecimal():
                return other == int(self.value)
            other = str(other)
        if isinstance(other, str):
            if other == self.subtype:
                return True
            return other == self.value
        return False


class TokenList (list):

    def __contains__(self, item: Union[Token, str, TokenType]):
        if isinstance(item, Token):
            return super.__contains__(item)
        if isinstance(item, TokenType) or isinstance(item, str):
            for t in self:
                if item in t:
                    return True
        return False


# Special characters

class SearchTokens:

    def __init__(self, string: str, special_tokens, filter_tokens):
        self.search = TokenList()
        self.locs = TokenList()
        self.filters = TokenList()
        if len(string) == 0:
            return
        combined_part = ''
        combine_level = 0
        for part in string.strip().split(' '):
            if len(part) == 0:
                if combine_level > 0:
                    combined_part += ' '
                continue

            if part[0] == '"':
                combine_level += 1
                if combine_level == 1:
                    if len(part) > 1:
                        combined_part += part[1:]
                    continue

            if combine_level > 0 and part[-1] == '"':
                combine_level -= 1
                part = (combined_part + f' {part[:-1]}').strip()
                combined_part = ''

            if combine_level > 0:
                combined_part += f' {part}'
                continue

            if part in special_tokens:
                self.locs.append(Token(TokenType.flag, None, part))
                continue

            is_search = True
            for f_token in filter_tokens:
                if len(part) <= len(f_token):
                    continue
                if f_token == part[:len(f_token)]:
                    self.filters.append(Token(TokenType.filter, f_token, part[len(f_token):]))
                    is_search = False
                    break

            if is_search:
                self.search.append(Token(TokenType.search, None, part))

    @classmethod
    def tokenize(cls, string, type_tokens, filter_tokens):
        return cls(string, type_tokens, filter_tokens)

    def __contains__(self, item: TokenType):
        for t in self.search + self.locs + self.filters:
            if item in t:
                return True
        return False

    def get_filter_list(self, key):
        return [i for i in self.filters if key in i]


loc_tokens = {
        'loc:inst': 'Search base inst details',
        'loc:param': 'Search parameter values',
        'loc:dialog': 'Search dialogue strings'
    }


filter_tokens = {
        'sct:': 'Search within specific script(s)',
        'sect:': 'Searches within specific section(s)',
        'inst:': 'Searches within a specific instruction(s)'
    }


dialog_result_string_width = 30


@dataclass
class PrjResult:
    row_data: str
    display: str

    def __repr__(self):
        return self.display


@dataclass
class PrjResultGroup:
    name: str
    contents: List[Union[PrjResultGroup, PrjResult]]

    def __repr__(self):
        return self.name

    def __eq__(self, other: Union[str, PrjResultGroup]):
        if isinstance(other, str):
            return self.name == other
        if not isinstance(other, PrjResultGroup):
            return False
        return self.name == other.name


class ProjectSearcher:

    def __init__(self, base_insts: BaseInstLibFacade, project: SCTProject):
        self.b_insts = base_insts
        self.prj = project

        self.search_loc_fxns = {
            'loc:inst': self.search_insts,
            'loc:param': self.search_params,
            'loc:dialog': self.search_dialogue
        }

        self.keep_case = False

    def search(self, search_string, keep_case):
        self.keep_case = keep_case
        tokens = SearchTokens.tokenize(search_string, list(loc_tokens.keys()), list(filter_tokens.keys()))

        search_locs = []
        if TokenType.flag not in tokens:
            search_locs += [k for k in self.search_loc_fxns.keys()]
        else:
            for token in tokens.locs:
                if token.value in self.search_loc_fxns.keys():
                    search_locs.append(token.value)

        links = []
        for loc in search_locs:
            results = self.search_loc_fxns[loc](tokens)
            if len(results) > 0:
                links.append(PrjResultGroup(loc[4:], results))

        return links

    def search_insts(self, tokens: SearchTokens):
        r_insts = [int(t.value) for t in tokens.get_filter_list('inst:')]

        if len(r_insts) == 0:
            if len(tokens.search) > 0:
                r_insts = []
                for token in tokens.search:
                    for ind, inst in enumerate(self.b_insts.get_all_insts()):
                        if self.str_comp(token.value, inst.name, in_=True):
                            r_insts.append(ind)
                        elif self.str_comp(token.value, str(inst.instruction_id)):
                            r_insts.append(ind)
                        elif self.str_comp(token.value, inst.description, in_=True):
                            r_insts.append(ind)
                        elif self.str_comp(token.value, inst.default_notes, in_=True):
                            r_insts.append(ind)
                        elif inst.link_type is not None:
                            if self.str_comp(token.value, inst.link_type, in_=True):
                                r_insts.append(ind)
                        else:
                            for param in inst.params.values():
                                if self.str_comp(token.value, param.name, in_=True):
                                    r_insts.append(ind)
                                    break
                                if param.default_value is not None:
                                    if isinstance(param.default_value, str):
                                        if self.str_comp(token.value, param.default_value, in_=True):
                                            r_insts.append(ind)
                                            break
                                    elif token.value == param.default_value:
                                        r_insts.append(ind)
                                        break

            else:
                r_insts = [i for i, _ in enumerate(self.b_insts.get_all_insts())]

        sct_filters = tokens.get_filter_list('sct:')
        sect_filters = tokens.get_filter_list('sect:')
        links = []
        for sct_name, sct in self.prj.scts.items():
            if len(sct_filters) > 0:
                if sct_name not in sct_filters:
                    continue
            for sect_name, sect in sct.sects.items():
                if len(sect_filters) > 0:
                    if sect_name not in sect_filters:
                        continue
                for inst_id, inst in sect.insts.items():
                    if inst.base_id in r_insts:
                        row_data = f'{sect_name}{alt_sep}{inst_id}'
                        display = f'{sect_name} - {sect.inst_list.index(inst_id)}'
                        if PrjResultGroup(sct_name, []) not in links:
                            links.append(PrjResultGroup(sct_name, [PrjResult(row_data, display)]))
                        else:
                            links[(links.index(sct_name))].contents.append(PrjResult(row_data, display))

        return links

    def search_params(self, tokens: SearchTokens):
        if len(tokens.search) == 0:
            return {}

        sct_filters = tokens.get_filter_list('sct:')
        sect_filters = tokens.get_filter_list('sect:')
        inst_filters = tokens.get_filter_list('inst:')
        links = []
        for sct_name, sct in self.prj.scts.items():
            if len(sct_filters) > 0:
                if sct_name not in sct_filters:
                    continue

            for sect_name, sect in sct.sects.items():
                if len(sect_filters) > 0:
                    if sect_name not in sect_filters:
                        continue

                for inst_id, inst in sect.insts.items():
                    if len(inst_filters) > 0:
                        if inst.base_id not in inst_filters:
                            continue

                    for token in tokens.search:
                        if inst.delay_param is not None:
                            if self.str_comp(token.value, str(inst.delay_param.value), in_=True):
                                row_data = f'{sect_name}{alt_sep}{inst_id}{alt_sep}delay'
                                display = f'{sect_name} - {sect.inst_list.index(inst_id)}'
                                if PrjResultGroup(sct_name, []) not in links:
                                    links.append(PrjResultGroup(sct_name, [PrjResult(row_data, display)]))
                                else:
                                    links[(links.index(sct_name))].contents.append(PrjResult(row_data, display))
                        for param_id, param in inst.params.items():
                            if 'jump' in param.type:
                                continue
                            if 'footer' in param.type or 'string' in param.type:
                                if self.str_comp(token.value, str(param.linked_string), in_=True):
                                    row_data = f'{sect_name}{alt_sep}{inst_id}{alt_sep}{param_id}'
                                    display = f'{sect_name} - {sect.inst_list.index(inst_id)}'
                                    if PrjResultGroup(sct_name, []) not in links:
                                        links.append(PrjResultGroup(sct_name, [PrjResult(row_data, display)]))
                                    else:
                                        links[(links.index(sct_name))].contents.append(PrjResult(row_data, display))
                            else:
                                if self.str_comp(token.value, str(param.value)):
                                    row_data = f'{sect_name}{alt_sep}{inst_id}{alt_sep}{param_id}'
                                    display = f'{sect_name} - {sect.inst_list.index(inst_id)}'
                                    if PrjResultGroup(sct_name, []) not in links:
                                        links.append(PrjResultGroup(sct_name, [PrjResult(row_data, display)]))
                                    else:
                                        links[(links.index(sct_name))].contents.append(PrjResult(row_data, display))
                        for loop_ind, loop in enumerate(inst.l_params):
                            for param_id, param in loop.items():
                                if self.str_comp(token.value, str(param.value)):
                                    row_data = f'{sect_name}{alt_sep}{inst_id}{alt_sep}{loop_ind}{sep}{param_id}'
                                    display = f'{sect_name} - {sect.inst_list.index(inst_id)} - {loop_ind} {param_id}'
                                    if PrjResultGroup(sct_name, []) not in links:
                                        links.append(PrjResultGroup(sct_name, [PrjResult(row_data, display)]))
                                    else:
                                        links[(links.index(sct_name))].contents.append(PrjResult(row_data, display))

        return links

    def search_dialogue(self, tokens: SearchTokens):
        d_strings = []
        sct_filters = tokens.get_filter_list('sct:')
        for sct_name, sct in self.prj.scts.items():
            if len(sct_filters) > 0:
                if sct_name not in sct_filters:
                    continue

            for sid, str_ in sct.strings.items():
                s = SAstr_to_visible(str_).replace('\n', ' ')
                for token in tokens.search:
                    if self.str_comp(token.value, sid) or self.str_comp(token.value, s, in_=True):
                        row_data = f'{sct.string_locations[sid]}{alt_sep}{sid}'
                        if sid == token.value:
                            display = f'{sct.string_locations[sid]} - {sid}'
                        else:
                            s_ind = s if self.keep_case else s.lower()
                            v = token.value if self.keep_case else token.value.lower()
                            t_index = s_ind.index(v)
                            outer_char_num = dialog_result_string_width - len(token.value) // 2
                            if outer_char_num > 0:
                                t_start = t_index - outer_char_num
                                t_end = t_index + len(token.value) + outer_char_num
                                substring = f'{"... " if t_start > 0 else ""}' \
                                            f'{s[max(t_start, 0): min(t_end, len(s))]}' \
                                            f'{" ..." if t_end < len(s) else ""}'
                            else:
                                substring = token.value
                            display = f'{sct.string_locations[sid]} - {substring}'

                        if PrjResultGroup(sct_name, []) not in d_strings:
                            d_strings.append(PrjResultGroup(sct_name, [PrjResult(row_data, display)]))
                        else:
                            d_strings[(d_strings.index(sct_name))].contents.append(PrjResult(row_data, display))

        return d_strings

    def str_comp(self, s1, s2, in_=False):
        if not self.keep_case:
            s1 = s1.lower()
            s2 = s2.lower()
        if in_:
            return s1 in s2
        return s1 == s2
