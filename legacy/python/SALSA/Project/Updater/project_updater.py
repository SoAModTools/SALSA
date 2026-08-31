import copy
from typing import Union

from SALSA.Common.constants import sep
from SALSA.Project.Updater.pu_constants import p_attrs, p_levels, UP, PP, loop_attrs, nd_index
from SALSA.Project.Updater.pu_definitions import update_tasks, p_max_depth
from SALSA.Project.project_container import SCTProject, SCTScript, SCTSection, SCTParameter, SCTInstruction, SCTLink


class ProjectUpdater:
    """Updates a SALSA project to the current version. It does so depth first, so the deepest changes occur first.
    Changes to links are handled by their respective parameters. Separate methods should be made to handle this"""

    log_key = 'PrjUpdater'

    @classmethod
    def update_project(cls, prj: SCTProject) -> SCTProject:
        updater = cls()
        while prj.version in update_tasks:
            prj = updater._upgrade_version(prj)
            prj.version += 1
        return prj

    def _upgrade_version(self, p_piece: Union[SCTProject, SCTScript, SCTSection, SCTParameter, SCTInstruction, SCTLink],
                         cur_version=None, p_level_ind=0, tasks=None, cur_script=None):
        if cur_script is None and isinstance(p_piece, SCTScript):
            cur_script = p_piece.name
        if cur_version is None:
            cur_version = p_piece.version
            print(f'{self.log_key}: Now updating project from version v{cur_version} to v{cur_version+1}')
        if tasks is None:
            tasks = update_tasks[p_piece.version]
        max_level = p_max_depth[cur_version]

        p_level = p_levels[p_level_ind]

        if p_level == PP.script:
            print(f'{self.log_key}: Now updating script: {p_piece.name}')

        if p_level == PP.section:
            print(f'{self.log_key}: Now updating section: {p_piece.name}', end='\r')

        if p_level != max_level and p_level_ind + 1 < len(p_levels):

            p_attr_name = p_attrs[cur_version][p_level_ind]
            if p_attr_name in p_piece.__dict__:
                child_entries = p_piece.__getattribute__(p_attr_name)

                not_dict = False
                if not isinstance(child_entries, dict):
                    child_entries = {nd_index: child_entries}
                    not_dict = True

                # Handle children
                updated_children = {}
                for child_key, child_piece in child_entries.items():
                    if child_piece is not None:
                        child_piece = self._upgrade_version(p_piece=child_piece, cur_version=cur_version,
                                                            p_level_ind=p_level_ind + 1, tasks=tasks, cur_script=cur_script)
                    updated_children[child_key] = child_piece

                if not_dict:
                    updated_children = updated_children[nd_index]

                p_piece.__setattr__(p_attr_name, updated_children)

        # Handle loop parameters of instructions since those won't be captured by the other recursive section above
        if p_level == PP.instruction and p_level != max_level:
            loop_params = []
            loop_attr_name = loop_attrs[cur_version]
            if loop_attr_name in p_piece.__dict__:
                for loop in p_piece.__getattribute__(loop_attr_name):
                    new_loop = {}
                    for param_id, param in loop.items():
                        param = self._upgrade_version(p_piece=param, cur_version=cur_version, cur_script=cur_script,
                                                      p_level_ind=p_level_ind + 1, tasks=tasks)
                        new_loop[param_id] = param
                    loop_params.append(new_loop)
                p_piece.__setattr__(loop_attr_name, loop_params)

        if p_level in tasks:
            for task_num, task in tasks[p_level].items():
                lcls = locals()
                args: list = [p_piece] + task.get(UP.arguments, []) + [lcls[a] for a in task.get(UP.up_args, [])]
                p_piece = self.__getattribute__(task[UP.callable])(*args)

        if p_level == PP.script:
            print(f'{self.log_key}: Finished updating script: {p_piece.name}')

        return p_piece

    @staticmethod
    def _retype_link_v1(cur_piece):
        if cur_piece.link is None:
            return cur_piece

        if getattr(cur_piece, 'link_value', None) is None:
            return cur_piece

        if cur_piece.link_value[0] == 'Footer':
            cur_piece.link.type = 'Footer'

        return cur_piece

    @staticmethod
    def _del_attr(cur_piece, *attrs):
        for attr in attrs:
            if attr in cur_piece.__dict__:
                cur_piece.__delattr__(attr)
        return cur_piece

    @staticmethod
    def _add_attr(cur_piece, *attrs):
        for new_attr in attrs:
            if new_attr[0] not in cur_piece.__dict__:
                setattr(cur_piece, new_attr[0], new_attr[1])
        return cur_piece

    @staticmethod
    def _change_attribute_names(cur_piece, *changes):
        for change in changes:
            old_attr = change[0]
            if old_attr in cur_piece.__dict__:
                new_attr = change[1]
                setattr(cur_piece, new_attr, getattr(cur_piece, old_attr))
                cur_piece.__delattr__(old_attr)
        return cur_piece

    # ------------------------- #
    # Script specific functions #
    # ------------------------- #

    @staticmethod
    def _modify_section_groups_v1(cur_piece: SCTScript):
        changes = {}
        for i, entry in enumerate(cur_piece.sect_tree):
            if not isinstance(entry, dict):
                continue
            new_entry = {}
            for k, v in entry.items():
                if k == v[0]:
                    new_entry[f'{k}{sep}group'] = v[1:]
            changes[i] = new_entry
        for pos, entry in changes.items():
            cur_piece.sect_tree.pop(pos)
            cur_piece.sect_tree.insert(pos, entry)
        return cur_piece

    @staticmethod
    def _move_string_garbage_v1(cur_piece: SCTScript):
        cur_piece.string_garbage = {}
        for name, sect in cur_piece.string_sections.items():
            if 'end' in sect.garbage:
                cur_piece.string_garbage[name] = sect.garbage
        cur_piece.__delattr__('string_sections')
        return cur_piece

    @staticmethod
    def _move_sect_labels_v1(cur_piece: SCTSection):
        if len(cur_piece.internal_sections_inst) == 0:
            labels = [cur_piece.name]
        else:
            labels, poses = [k for k in cur_piece.internal_sections_inst.keys()], [v for v in cur_piece.internal_sections_inst.values()]
            poses, labels = zip(*sorted(zip(poses, labels)))
            labels = list(labels)
        for inst in cur_piece.insts.values():
            if inst.base_id == 9:
                if len(labels) == 0:
                    inst.label = '??Unknown??'
                else:
                    inst.label = labels.pop(0)
        return cur_piece

    def _refactor_logical_sections(self, cur_piece: SCTScript):
        name_changes = {}
        for name, sect in cur_piece.sects.items():
            sect.is_compound = False
            if '(0)' not in name:
                continue
            mul_label = False
            inst_ind = 0
            while not mul_label:
                inst_ind += 1
                if len(sect.inst_list) <= inst_ind:
                    break
                if sect.insts[sect.inst_list[inst_ind]].base_id == 9:
                    mul_label = True

            if mul_label:
                sect.is_compound = True
                new_name = sect.name.replace('(0)', '')
                name_changes[name] = new_name
                sect.name = new_name

        for old, new in name_changes.items():
            # Change the name in sects
            cur_piece.sects[new] = cur_piece.sects.pop(old)

            # Change the name in the list and tree
            cur_piece.sect_list[cur_piece.sect_list.index(old)] = new
            self._recursive_name_replacer(cur_piece.sect_tree, old, new)

            # Change the name in other items
            if old in cur_piece.folded_sects:
                cur_piece.folded_sects[new] = cur_piece.folded_sects.pop(old)

            if old in cur_piece.folded_sects.values():
                folded_keys = list(cur_piece.folded_sects.keys())
                folded_ind = [i for i, x in enumerate(list(cur_piece.folded_sects.values())) if x == old]
                keys_to_change = [folded_keys[i] for i in folded_ind]
                for key in keys_to_change:
                    cur_piece.folded_sects[key] = new

            if old in cur_piece.unused_sections:
                cur_piece.unused_sections[cur_piece.unused_sections.index(old)] = new

            for sect_list in cur_piece.inst_locations:
                if old in sect_list:
                    sect_list[sect_list.index(old)] = new

            for inst in cur_piece.sects[new].insts.values():
                for link in inst.links_in:
                    link.target_trace[0] = new
                for link in inst.links_out:
                    link.origin_trace[0] = new

        return cur_piece

    def _recursive_name_replacer(self, tree, old, new):
        if isinstance(tree, list):
            if old in tree:
                tree[tree.index(old)] = new
                return True

            for entry in tree:
                if isinstance(entry, dict):
                    replaced = self._recursive_name_replacer(entry, old, new)
                    if replaced:
                        return True

        if isinstance(tree, dict):
            if old in list(tree.keys())[0]:
                old_key = list(tree.keys())[0]
                new_key = f'{new}{old_key.replace(old, "").replace("(0)", "")}'
                tree[new_key] = tree.pop(old_key)
                return True

            for value in tree.values():
                replaced = self._recursive_name_replacer(value, old, new)
                if replaced:
                    return True

    @staticmethod
    def _add_script_to_links(cur_piece, scr):
        if cur_piece.link is None:
            return cur_piece
        cur_piece.link.script = scr
        return cur_piece


if __name__ == '__main__':
    import os
    cur_dir = os.path.dirname(__file__)
    os.chdir(cur_dir)
    os.chdir(os.path.pardir)
    os.chdir(os.path.pardir)
    os.chdir(os.path.pardir)
    from SALSA.BaseInstructions.bi_facade import BaseInstLibFacade
    from SALSA.FileModels.project_model import ProjectModel
    from SALSA.Scripts.script_encoder import SCTEncoder
    os.chdir(cur_dir)

    model = ProjectModel()
    project: SCTProject = model.load_project(filepath=os.path.join('./test', 'test_UwU v3.prj'), ignore_dir=True)
    if getattr(project, 'version', None) is None:
        project.version = 1

    # specific_script = 'me034c'
    specific_script = None

    # Only check the first script?
    if 'scripts' in project.__dict__:
        project_first_script_name = list(project.scripts.keys())[0]
        if specific_script is not None:
            project_first_script_name = specific_script
        project.scripts = {project_first_script_name: project.scripts[project_first_script_name]}
    else:
        project_first_script_name = list(project.scts.keys())[0]
        if specific_script is not None:
            project_first_script_name = specific_script
        project.scts = {project_first_script_name: project.scts[project_first_script_name]}

    project = ProjectUpdater.update_project(project)

    base_insts = BaseInstLibFacade()
    for script in project.scts.values():
        sct = SCTEncoder.encode_sct_file_from_project_script(project_script=script, base_insts=base_insts, endian='big')

    print('Success...')
