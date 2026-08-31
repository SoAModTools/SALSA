import os
import configparser
from typing import Tuple, List, Union


class Settings(configparser.ConfigParser):
    defaults = {
        'DEFAULT': {}
    }

    def __init__(self):
        super().__init__()
        self.filename = './UserSettings/config.ini'
        self._load_settings()

    def _load_settings(self):
        if not os.path.exists(self.filename):
            self._new_settings_file()

        with open(self.filename, 'r') as configfile:
            self.read_file(configfile)

    def _new_settings_file(self):
        for name, group in self.defaults.items():
            self[name] = group
        if not os.path.exists(os.path.dirname(self.filename)):
            os.mkdir(os.path.dirname(self.filename))
        with open(self.filename, 'w') as configfile:
            self.write(configfile)

    def _save_settings(self):
        with open(self.filename, 'w') as configfile:
            self.write(configfile)

    def add_group(self, section: str):
        self[section] = {}
        self._save_settings()

    def set_single(self, group: str, setting: str, value: str):
        self[group][setting] = value
        self._save_settings()

    def set_multiple(self, setting_list: List[Tuple[str, str, str]]):
        for s in setting_list:
            self[s[0]][s[1]] = s[2]
        self._save_settings()


settings = Settings()
