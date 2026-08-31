import tkinter as tk
from tkinter import ttk
from typing import Literal, Dict

cur_script_text = 'Current Script: '


class DolphinLinkPopup(tk.Toplevel):
    t = 'Dolphin Link'
    log_key = 'DolphinLinkPopup'
    w = 250
    h = 160

    def __init__(self, parent, callbacks, name, theme, *args, **kwargs):
        super().__init__(parent, *args, **kwargs)

        self.parent: tk.Tk = parent
        self.name = name
        self.callbacks = callbacks

        self.protocol('WM_DELETE_WINDOW', self.close)
        self.configure(**theme['Ttoplevel']['configure'])

        self.title(self.t)
        self.resizable(width=True, height=True)

        posX = self.parent.winfo_x() + (self.parent.winfo_width() - self.w) // 2
        posY = self.parent.winfo_y() + (self.parent.winfo_height() - self.h) // 2
        pos = f'{self.w}x{self.h}+{posX}+{posY}'
        self.geometry(pos)

        self.columnconfigure(0, weight=1)

        self.buttons: Dict[str, ttk.Button] = {
            'attach': ttk.Button(self, text='Connect to Dolphin', command=self.callbacks['attach_dolphin'])
        }
        self.buttons['attach'].grid(row=0, column=0, sticky=tk.W, padx=5, pady='5 0')

        status_label = ttk.Label(self, text='Dolphin Link Status:')
        status_label.grid(row=1, column=0, sticky=tk.W, padx=5)

        self.status_widgets = {
            'dolphin': ttk.Label(self, text='', style='warning.TLabel'),
            'cur_sct': ttk.Label(self, text=''),
            'update': ttk.Label(self, text='', style='warning.TLabel')
        }

        row = 2
        for w in self.status_widgets.values():
            w.grid(row=row, column=0, sticky=tk.E + tk.W, padx=5)
            row += 1

        self.buttons['update'] = ttk.Button(self, text='Update Current SCT', command=self.callbacks['update_sct'])
        self.buttons['update'].grid(row=row, column=0, sticky=tk.W, padx=5)
        self.buttons['update'].state(['disabled'])

        self.set_cur_inst = tk.IntVar(self, 0)
        set_cur_inst_button = ttk.Checkbutton(self, text=' Set Selected Inst as Next Scheduled Inst\n in Dolphin',
                                              variable=self.set_cur_inst, onvalue=1, offvalue=0)
        set_cur_inst_button.grid(row=row+1, column=0, sticky=tk.W, ipadx=5, padx=5)

        self.callbacks['attach_controller'](self)

    def set_status(self, stat_type: Literal['dolphin', 'update', 'cur_sct'], status, style=None):
        self.status_widgets[stat_type].configure(style=style, text=status)

    def set_active_button(self, button: Literal['attach', 'update', 'select']):
        for b_name, b in self.buttons.items():
            state_spec = ['!disabled'] if b_name == button else ['disabled']
            self.buttons[b_name].state(state_spec)

    def close(self):
        self.callbacks['attach_controller'](None)
        self.callbacks['close'](self.name, self)

    def change_theme(self, theme):
        self.configure(**theme['Ttoplevel']['configure'])
