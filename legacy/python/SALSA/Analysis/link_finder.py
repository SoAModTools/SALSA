from __future__ import annotations

from typing import List, Optional

import enum

from SALSA.BaseInstructions.bi_facade import BaseInstLibFacade
from SALSA.Project.project_container import SCTProject, SCTSection, SCTScript


class NodeType(enum.Enum):
    root = 0
    sect = 1
    group = 2
    link = 3


class LinkNode:
    children: List[LinkNode]
    content_type: NodeType
    content_str: str
    content_display: Optional[str]
    link_trace: Optional[tuple]

    def __init__(self, _type, _str):
        self.content_type = _type
        self.content_str = _str
        self.children = []
        self.content_display = None
        self.link_trace = None

    def find_child(self, content_str):
        if self.children is None:
            return None
        for child in self.children:
            if child.content_str == content_str:
                return child
        return None

    def add_child(self, child: LinkNode):
        self.children.append(child)

    def __repr__(self):
        if self.content_display:
            return self.content_display
        return self.content_str

    def get_rows(self, parents=None, rows = None):
        if rows is None:
            rows = []
        if parents is None:
            parents = []

        if self.content_type == NodeType.root:
            for child in self.children:
                rows = child.get_rows(rows=rows)
            return rows

        if len(self.children) == 0:
            rows.append([*parents, (self.__repr__(), self.link_trace)])
        else:
            if self.content_type == NodeType.link:
                parents.append((self.__repr__(), self.link_trace))
            else:
                parents.append((self.__repr__(),))
            for child in self.children:
                rows = child.get_rows(parents, rows)
            parents.pop(-1)


        return rows


class LinkFinder:
    links_in: LinkNode
    links_out: LinkNode
    target_sct: str
    target_sect: str
    base_insts: BaseInstLibFacade


    def get_inst_path_recursive(self, sect: SCTSection, target_inst, cur_tree=None):
        if cur_tree is None:
            cur_tree = sect.inst_tree

        if target_inst in cur_tree:
            target_inst_idx = sect.inst_list.index(target_inst)
            return [(NodeType.link, target_inst, (sect.name, target_inst),
                              f'{target_inst_idx} - {self.base_insts.get_inst(sect.insts[target_inst].base_id).name}')]

        for child in cur_tree:
            if isinstance(child, str):
                continue
            dict_key = list(child.keys())[0]
            result = self.get_inst_path_recursive(sect, target_inst, child[dict_key])
            if result is None:
                continue
            parent_id, group_type = dict_key.split('|')
            parent_index = sect.inst_list.index(parent_id)
            return [(NodeType.group, parent_id, (sect.name, parent_id), f'{parent_index} - {group_type}'), *result]

        return None

    def find_links_in(self, sct: SCTScript, sect_label_inst):
        root = LinkNode(NodeType.root, 'links_in')
        self.links_in = root

        for link in sect_label_inst.links_in:
            sect_name = link.origin_trace[0]
            sect_node = root.find_child(sect_name)
            if sect_node is None:
                sect_node = LinkNode(NodeType.sect, sect_name)
                root.add_child(sect_node)
            sect_node.link_trace = (sect_name, )
            inst_path = self.get_inst_path_recursive(sct.sects[sect_name], link.origin_trace[1])
            if inst_path is None:
                continue
            parent_node = sect_node
            for spec in inst_path:
                inst_node = parent_node.find_child(spec[1])
                if inst_node is None:
                    inst_node = LinkNode(spec[0], spec[1])
                    inst_node.link_trace = spec[2]
                    inst_node.content_display = spec[3]
                    parent_node.add_child(inst_node)
                    parent_node = inst_node

    def find_links_out(self, sct: SCTScript, sect: SCTSection):
        all_links_out = []
        for inst in sect.insts.values():
            for link in inst.links_out:
                if link.target_trace is None:
                    continue
                if link.target_trace[0] != sect.name:
                    all_links_out.append(link)

        root = LinkNode(NodeType.root, 'links_out')
        self.links_out = root

        for link in all_links_out:
            inst_path = self.get_inst_path_recursive(sect, link.origin_trace[1])
            if inst_path is None:
                continue
            parent_node = root
            for spec in inst_path:
                inst_node = parent_node.find_child(spec[1])
                if inst_node is None:
                    inst_node = LinkNode(spec[0], spec[1])
                    inst_node.link_trace = spec[2]
                    inst_node.content_display = spec[3]
                    parent_node.add_child(inst_node)
                    parent_node = inst_node
            tgt_sect = link.target_trace[0]
            sect_node = parent_node.find_child(tgt_sect)
            if sect_node is None:
                sect_node = LinkNode(NodeType.sect, tgt_sect)
                parent_node.add_child(sect_node)
            tgt_inst_idx = sct.sects[tgt_sect].inst_list.index(link.target_trace[1])
            tgt_node = LinkNode(NodeType.link, link.target_trace[1])
            tgt_inst_name = self.base_insts.get_inst(sct.sects[tgt_sect].insts[link.target_trace[1]].base_id).name
            tgt_node.content_display = f'{tgt_inst_idx} - {tgt_inst_name}'
            tgt_node.link_trace = link.target_trace
            sect_node.add_child(tgt_node)


    @classmethod
    def find_links(cls, prj: SCTProject, sct, sect, base_insts: BaseInstLibFacade):

        if sct not in prj.scts:
            return None

        target_sct = prj.scts[sct]

        if sect not in target_sct.sects:
            return None

        target = target_sct.sects[sect]

        lf = cls()
        lf.target_sect = sect
        lf.target_sct = sct
        lf.base_insts = base_insts

        sect_label = target.insts[target.inst_list[0]]
        lf.find_links_in(target_sct, sect_label)

        lf.find_links_out(target_sct, target)

        return lf

    def get_in_tree(self, full):
        return self.links_in.get_rows()

    def get_out_tree(self):
        pass

    def get_last_idx(self, sect, item):
        last_idx = 0
        if isinstance(item, dict):
            dict_keys = list(item.keys())
            for key in dict_keys:
                if '|' in key:
                    key_id = key.split('|')[0]
                    last_idx = max(last_idx, sect.inst_list.index(key_id))
                last_idx = max(last_idx, self.get_last_idx(sect, item[key]))
        elif isinstance(item, list) and len(item) > 0:
            last_idx = max(last_idx, self.get_last_idx(sect, item[-1]))
        elif item is None:
            return 0
        else:
            last_idx = sect.inst_list.index(item)
        return last_idx

    def get_first_index(self, sect, item):
        first_idx = 10000000
        if isinstance(item, dict):
            dict_keys = list(item.keys())
            for key in dict_keys:
                if '|' in key:
                    key_id = key.split('|')[0]
                    first_idx = min(first_idx, sect.inst_list.index(key_id))
                first_idx = min(first_idx, self.get_first_index(sect, item[key]))
        elif isinstance(item, list) and len(item) > 0:
            first_idx = min(first_idx, self.get_first_index(sect, item[0]))
        elif item is None:
            return 10000000
        else:
            first_idx = sect.inst_list.index(item)
        return first_idx