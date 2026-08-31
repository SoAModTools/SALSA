import json
import os.path
import pickle

from SALSA.Common.setting_class import settings


class ProjectModel:

    log_key = 'PrjFileModel'

    def __init__(self):
        self.callbacks = {}
        if self.log_key not in settings.keys():
            settings.add_group(self.log_key)
        self.recent_files = []
        self.max_recents = 10
        self._load_recent_filelist()

    def load_project(self, filepath, ignore_dir=False):
        if filepath == '' or filepath is None:
            print('Unable to save file, no filepath')
            return
        if not os.path.exists(filepath):
            print('File does not exist')
            return

        if not ignore_dir:
            settings.set_single(self.log_key, 'directory', os.path.dirname(filepath))
        with open(filepath, 'rb') as fh:
            proj = pickle.load(fh)

        return proj

    def save_project(self, proj, filepath):
        if filepath is None or filepath == '':
            print('No file path for saving')
            return

        settings.set_single(self.log_key, 'directory', os.path.dirname(filepath))
        with open(filepath, 'wb') as file:
            pickle.dump(proj, file)

        print(f'{self.log_key}: Project Saved: {filepath}')

    def get_project_directory(self):
        return settings[self.log_key].get('directory', None)

    def add_recent_file(self, filepath):
        if filepath in self.recent_files:
            file_index = self.recent_files.index(filepath)
            self.recent_files.pop(file_index)
        self.recent_files.insert(0, filepath)
        if len(self.recent_files) > self.max_recents:
            self.recent_files.pop()
        self._save_recent_filelist()
        self.callbacks['menu->update_recents'](self.get_recent_filenames())

    def _save_recent_filelist(self):
        set_list = []
        for i in range(0, self.max_recents):
            key = f'recent_file_{i}'
            if i >= len(self.recent_files):
                if key in settings[self.log_key].keys():
                    settings[self.log_key].pop(key)
                continue
            cur_path = self.recent_files[i]
            set_list.append([self.log_key, key, cur_path])
        settings.set_multiple(set_list)

    def _load_recent_filelist(self):
        self.recent_files = []
        update_settings_recent = False
        for i in range(0, self.max_recents):
            key = f'recent_file_{i}'
            if key not in settings[self.log_key].keys():
                break
            filepath = settings[self.log_key].get(key, None)
            if filepath is None:
                break
            if not os.path.exists(filepath):
                print(f'Recent file {filepath} does not exist, skipping...')
                update_settings_recent = True
                continue
            self.recent_files.append(filepath)
        if update_settings_recent:
            self._save_recent_filelist()

    def get_recent_filepath(self, index):
        if index >= len(self.recent_files):
            raise IndexError(f'Recent File number {index} outside of recent file list: max entry is {len(self.recent_files)-1}')
        return self.recent_files[index]

    def get_recent_filenames(self):
        return [os.path.basename(_) for _ in self.recent_files]

    def add_callback(self, name, callback):
        self.callbacks[name] = callback
