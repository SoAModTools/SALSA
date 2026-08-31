import os.path
import queue
from tkinter.filedialog import askdirectory

from SALSA.BaseInstructions.bi_facade import BaseInstLibFacade
from SALSA.FileModels.sct_model import SCTModel
from SALSA.Project.project_container import SCTProject
from SALSA.Scripts.script_decoder import SCTDecoder


class TBStringToParamRepair:

    @classmethod
    def repair_project(cls, project: SCTProject, inst_lib: BaseInstLibFacade, sct_model: SCTModel, status_queue: queue.SimpleQueue):

        directory = sct_model.get_default_directory()
        prj_repair = cls()
        missing_files = prj_repair.check_sct_files_exist(directory, list(project.scts.keys()))
        if len(missing_files) > 0:
            directory = askdirectory(title='Where are the scripts located?',
                                     initialdir=sct_model.get_default_directory())

            missing_files = prj_repair.check_sct_files_exist(directory, list(project.scts.keys()))

            if len(missing_files) > 0:
                print('Some script files were missing from the chosen directory:')
                for file in missing_files:
                    print(f'\t{file}')
                print('Please add them or select a different directory')
                return None

        sct_model.set_default_directory(directory)

        for name, script in project.scts.items():
            print(f'Repairing script: {name}')
            status_queue.put({'sub_msg': f'{name}'})
            _, sct = sct_model.read_sct_file(f'{name}.sct')
            dec_script = SCTDecoder.decode_sct_from_file(name, sct, inst_lib, strings_only=True)
            for sect in script.sects.values():
                for inst in sect.insts.values():
                    if inst.base_id not in (144, 155):
                        continue
                    if inst.base_id == 144:
                        linked_str = inst.params[0].linked_string
                    else:
                        linked_str = inst.params[1].linked_string
                    if linked_str in dec_script.sects:
                        if '\\c' in dec_script.sects[linked_str].string:
                            if inst.base_id == 144:
                                if inst.params[1].value != 'decimal: 1+0/256':
                                    inst.params[1].set_value('decimal: 1+0/256')
                            if inst.base_id == 155:
                                if inst.params[2].value != 'decimal: 1+0/256':
                                    inst.params[2].set_value('decimal: 1+0/256')

        return project

    @staticmethod
    def check_sct_files_exist(directory, files) -> list:
        missing_script_files = []
        for script in files:
            if not os.path.exists(os.path.join(directory, f'{script}.sct')):
                missing_script_files.append(script)
        return missing_script_files
