import os
import queue

from SALSA.Common.setting_class import settings
from SALSA.AKLZ.aklz import Aklz
from SALSA.Scripts.script_decoder import SCTDecoder
from SALSA.Scripts.script_encoder import SCTEncoder


class SCTModel:
    """Creates an object which can read in and decode *.sct files based on an instruction object"""
    log_key = 'SctFileModel'

    def __init__(self):
        if self.log_key not in settings.keys():
            settings[self.log_key] = {}

    def set_default_directory(self, directory):
        if os.path.exists(directory) and os.path.isdir(directory):
            settings.set_single(self.log_key, 'directory', directory)

    def get_default_directory(self):
        if 'directory' in settings[self.log_key]:
            return settings[self.log_key]['directory']
        return ''

    def export_script_as_sct(self, filepath, script, base_insts, options, compress=False):
        print(f'Exporting {script.name}...', end='\r')
        sct_file = SCTEncoder.encode_sct_file_from_project_script(project_script=script, base_insts=base_insts, **options)
        self.save_sct_file(filepath=filepath, sct_file=sct_file, compress=compress)

    def save_sct_file(self, filepath, sct_file, compress=False):
        path_dir = os.path.dirname(filepath)
        if not os.path.exists(path_dir):
            print(f'{self.log_key}: Unable to save, directory does not exist: {path_dir}')
            return

        if compress:
            print(f'{self.log_key}: Compressing sct file')
            sct_file = Aklz.compress(sct_file)
            print(f'{self.log_key}: Finished compressing sct file')

        with open(filepath, 'wb') as sct:
            sct.write(sct_file)

        print(f'{self.log_key}: SCT file saved to {filepath}')

    def load_sct(self, insts, file: str, status: queue.SimpleQueue = None):
        out = self.read_sct_file(file)
        if out is None:
            return None
        name = out[0]
        status.put({'msg': f'Decoding script file: {name}'})
        sct_raw = out[1]
        sct_out = SCTDecoder.decode_sct_from_file(name=name, sct=sct_raw, inst_lib=insts, status=status)
        return name, sct_out

    def read_sct_file(self, filepath: str, use_slow=False) -> (str, bytearray):
        if '/' not in filepath:
            filename = filepath.split('.')[0]
            if 'directory' not in settings[self.log_key]:
                print(f'{self.log_key}: Unable to load {filepath}, no directory given')
                return
            directory = settings[self.log_key]['directory']
            filepath = os.path.join(directory, filepath)
        else:
            filename = filepath.split('/')[-1].split('.')[0]
        if os.path.exists(filepath):
            with open(filepath, 'rb') as fh:
                ba = bytearray(fh.read())
        else:
            raise FileExistsError(f'{self.log_key}: {filename} does not exist.')

        if Aklz.is_compressed(ba):
            ba = Aklz.decompress(ba)

        return filename, ba
