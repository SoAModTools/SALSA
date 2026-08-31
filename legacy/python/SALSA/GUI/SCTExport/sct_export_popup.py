import os.path
import tkinter as tk
from tkinter import filedialog, ttk

from SALSA.GUI.themes import dark_theme, light_theme
from SALSA.Common.setting_class import settings


class SCTExportPopup(tk.Toplevel):

    t = 'Export Script(s) as SCT file'
    log_key = 'SCTExportPopup'
    w = 250
    h = 400

    option_settings_check = {
        'use_garbage': {'text': 'Add garbage from original files', 'default': 'True'},
        'combine_footer': {'text': 'Combine footer entries', 'default': 'False'},
        'all_refresh': {'text': 'Add unnecessary refresh skips', 'default': 'True'},
        'compress_aklz': {'text': 'Compress file using AKLZ compression', 'default': 'False'}
    }
    option_settings_radio = {
        'system': {'label': 'Select target system:', 'entries': ['Dreamcast', 'GameCube'], 'default': 'GameCube'}
    }

    def __init__(self, parent, callbacks, name, selected, theme, *args, **kwargs):
        super().__init__(parent, *args, **kwargs)

        self.parent: tk.Tk = parent
        self.name = name
        self.callbacks = callbacks
        self.selected = selected
        self.script_ids = {}
        self.protocol('WM_DELETE_WINDOW', self.close)
        self.configure(**theme['Ttoplevel']['configure'])

        if self.log_key not in settings:
            settings[self.log_key] = {}

        for s, v in self.option_settings_check.items():
            if s not in settings[self.log_key]:
                settings[self.log_key][s] = v['default']

        for s, v in self.option_settings_radio.items():
            if s not in settings[self.log_key]:
                settings[self.log_key][s] = v['default']

        self.columnconfigure(0, weight=1)

        directory_frame = ttk.LabelFrame(self, text='Select a directory for export')
        directory_frame.grid(row=0, column=0, sticky='NSEW', padx=5)
        directory_frame.columnconfigure(0, weight=1)

        self.directory_var = tk.StringVar()
        if 'script export dir' in settings[self.log_key]:
            self.directory_var.set(settings[self.log_key]['script export dir'])
        directory_entry = ttk.Entry(directory_frame, textvariable=self.directory_var, width=30)
        directory_entry.grid(row=0, column=0, sticky='NSEW')

        directory_button = ttk.Button(directory_frame, text='...', command=self.get_directory)
        directory_button.grid(row=0, column=1, sticky=tk.W, padx=4)

        script_tree_frame = ttk.LabelFrame(self, text='Select script(s) to export')
        script_tree_frame.grid(row=1, column=0, sticky='NSEW', padx=5)
        script_tree_frame.columnconfigure(0, weight=1)
        script_tree_frame.rowconfigure(0, weight=1)
        self.rowconfigure(1, weight=1)

        self.scripts = ttk.Treeview(script_tree_frame, name='script', columns=[])
        self.scripts.grid(row=0, column=0, sticky='NSEW')
        self.scripts.heading('#0', text='Script')
        script_tree_scrollbar = ttk.Scrollbar(script_tree_frame, orient='vertical', command=self.scripts.yview)
        script_tree_scrollbar.grid(row=0, column=1, sticky=tk.N+tk.S)
        self.scripts.config(yscrollcommand=script_tree_scrollbar.set)
        self.scripts.config(show='tree')

        options_frame = ttk.LabelFrame(self, text='Options')
        options_frame.grid(row=2, column=0, sticky='NSEW', padx=5)

        self.option_vars = {}
        row = 0
        for o_name, o_settings in self.option_settings_check.items():
            self.option_vars[o_name] = tk.StringVar()
            button = ttk.Checkbutton(options_frame, text=o_settings['text'], variable=self.option_vars[o_name],
                                     onvalue='True', offvalue='False', command=lambda x=o_name: self.change_setting(x))
            button.grid(row=row, column=0, sticky=tk.W)
            self.option_vars[o_name].set(settings[self.log_key][o_name])
            row += 1

        for o_name, o_settings in self.option_settings_radio.items():
            label = ttk.Label(options_frame, text=o_settings['label'])
            label.grid(row=row, column=0, sticky=tk.W)
            row += 1
            default = settings[self.log_key][o_name]
            self.option_vars[o_name] = tk.StringVar()
            for e in o_settings['entries']:
                button = ttk.Radiobutton(options_frame, text=e, value=e, variable=self.option_vars[o_name],
                                         command=lambda x=o_name: self.change_setting(x))
                button.grid(row=row, column=0, sticky=tk.W)
                if e == default:
                    button.invoke()
                row += 1

        button_frame = ttk.Frame(self)
        button_frame.grid(row=3, column=0, sticky=tk.E+tk.S, padx=5, pady=2)

        self.export = ttk.Button(button_frame, text='Export', command=self.export)
        self.export.grid(row=0, column=0, padx=2)

        self.quit = ttk.Button(button_frame, text='Cancel', command=self.close)
        self.quit.grid(row=0, column=1, padx=2)

        self.title(self.t)
        self.resizable(width=False, height=False)

        posX = self.parent.winfo_x() + (self.parent.winfo_width() - self.w)//2
        posY = self.parent.winfo_y() + (self.parent.winfo_height() - self.h)//2
        pos = f'{self.w}x{self.h}+{posX}+{posY}'
        self.geometry(pos)

        self._populate_script_tree()

        self.set_option_state()

    def _populate_script_tree(self):
        all_scripts = self.callbacks['get_tree']()
        iid = 0
        for script in all_scripts:
            self.scripts.insert(parent='', iid=str(iid), text=script['name'], values=[], index='end')
            self.script_ids[iid] = script['name']
            iid += 1

    def get_directory(self):
        kwargs = {}
        cur_dir = self.directory_var.get()
        if os.path.exists(cur_dir):
            if os.path.isdir(cur_dir):
                kwargs['initialdir'] = cur_dir
        new_dir = filedialog.askdirectory(**kwargs)
        if new_dir == '':
            return
        self.directory_var.set(new_dir)
        settings.set_single(self.log_key, 'script export dir', new_dir)
        self.tkraise()

    def change_setting(self, setting):
        settings[self.log_key][setting] = self.option_vars[setting].get()
        self.set_option_state()

    def set_option_state(self):
        compress_ind = list(self.option_settings_check.keys()).index('compress_aklz') + 1
        compress_checkbox = self.children['!labelframe3'].children[f'!checkbutton{compress_ind if compress_ind != 1 else ""}']
        compress_disabled_state = 'disabled' if self.option_vars['system'].get() == 'Dreamcast' else '!disabled'
        compress_checkbox.state([compress_disabled_state])

    def export(self):
        directory = self.directory_var.get()
        if not os.path.exists(directory):
            return
        compress = self.option_vars['compress_aklz'].get() == 'True'
        if self.option_vars['system'].get() == 'Dreamcast':
            compress = False
        options = {
            'use_garbage': self.option_vars['use_garbage'].get() == 'True',
            'combine_footer_links': self.option_vars['combine_footer'].get() == 'True',
            'add_spurious_refresh': self.option_vars['all_refresh'].get() == 'True',
            'compress_aklz': compress,
            'endian': 'little' if self.option_vars['system'].get() == 'Dreamcast' else 'big'
        }
        scripts = [self.script_ids[int(s)] for s in self.scripts.selection()]
        if len(scripts) > 0:
            self.callbacks['export'](directory=directory, scripts=scripts, options=options)
        self.close()

    def close(self):
        self.callbacks['close'](self.name, self)

    def change_theme(self, theme):
        self.configure(**theme['Ttoplevel']['configure'])
