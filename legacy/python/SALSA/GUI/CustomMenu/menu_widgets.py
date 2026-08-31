from __future__ import annotations

import tkinter as tk
from tkinter import ttk
from typing import List, Dict, Callable, Union

from SALSA.GUI.themes import dark_theme, light_theme

prohibited_labels = [None, '']

menu_default_theme = {
    "SALSAMenu": {
        'configure': {
            "background": 'grey100',
        }
    },
    "SALSAMenu.TLabel": {
        'configure': {
            "background": 'grey100',
            "foreground": 'black',
        },
        'map': {
            'background': [('selected', 'blue'), ('active', 'blue')],
            'foreground': [('selected', 'white'), ('active', 'white')]
        }
    },
    "SALSAMenu.TCheckbutton": {
        'configure': {
            "background": 'grey100',
            "foreground": 'black',
        },
        'map': {
            'background': [('selected', 'blue'), ('active', 'blue')],
            'foreground': [('selected', 'white'), ('active', 'white')]
        }
    },
    "SALSAMenuBar.TFrame": {
        'configure': {
            "background": 'grey100',
        }
    },
    "SALSAMenuBar.TLabel": {
        'configure': {
            "background": 'gray90',
            "foreground": 'black',
        },
        'map': {
            'background': [('selected', 'blue'), ('active', 'blue')],
            'foreground': [('selected', 'white'), ('active', 'white')]
        }
    },
}


class SALSAMenu(tk.Toplevel):
    log_key = 'SALSAMenu'

    cascade_timing = 1000  # Time for opening and closing a cascade
    cascade_interval = 100  # Time between checking for closing a cascade
    cascade_offset_x = 5
    cascade_offset_y = -2

    padding = 10
    style = 'SALSAMenu'
    custom_label_style = 'SALSAMenu.TLabel'
    custom_checkbox_style = 'SALSAMenu.TCheckbutton'

    def __init__(self, parent, *args, theme=None, style=None, label_style=None, checkbox_style=None, **kwargs):
        self.custom_label_style = label_style if label_style is not None else self.custom_label_style
        self.custom_checkbox_style = checkbox_style if checkbox_style is None else self.custom_checkbox_style
        self.style = style if style is not None else self.style
        self.theme = theme if theme is not None else menu_default_theme
        self.colors = self.theme[self.style]['configure']
        super().__init__(parent, *args, **self.colors, **kwargs)

        self.entries_order: List[str] = []
        self.entries: Dict[str, Union[ttk.Label, ttk.Checkbutton]] = {}
        self.cascades: Dict[str, SALSAMenu] = {}
        self.active_entry: str = ''

        self.wm_overrideredirect(True)
        self.state('withdrawn')

    def add_command(self, label: str, command: Union[None, Callable] = None):
        if label in self.entries_order:
            return print(f'{self.log_key}: Command not created, duplicate name: {label}')
        self.entries[label] = ttk.Label(self, text=label, style=self.custom_label_style)
        self.entries[label].grid(row=len(self.entries_order), column=0, sticky='NSEW', padx=self.padding)
        if command is not None:
            self.entries[label].bind('<ButtonRelease-1>', lambda event: command(), add='+')
        self.entries[label].bind('<Enter>', self.set_active_widget, add='+')
        self.entries_order.append(label)

    def add_checkbutton(self, label, variable, command, **kwargs):
        if label in self.entries_order:
            return print(f'{self.log_key}: Command not created, duplicate name: {label}')
        self.entries[label] = ttk.Checkbutton(self, text=label, variable=variable, command=command, **kwargs,
                                              style=self.custom_checkbox_style)
        self.entries[label].grid(row=0, column=len(self.entries_order), sticky='NSEW', padx=self.padding)
        self.entries[label].bind('<Enter>', self.set_active_widget, add='+')
        self.entries_order.append(label)

    def add_cascade(self, label, menu: SALSAMenu):
        if label in self.entries_order:
            return print(f'{self.log_key}: Command not created, duplicate name: {label}')
        self.add_command(label)
        prev_row = len(self.entries_order) - 1
        arrow = tk.Label(self, text='❱')
        arrow.grid(row=prev_row, column=1)
        self.cascades[label] = menu
        self.entries[label].bind('<ButtonRelease-1>', self.open_cascade, add='+')
        self.entries[label].bind('<Enter>', self.check_open_cascade, add='+')
        self.entries[label].bind('<Leave>', self.check_close_cascade, add='+')

    def entryconfig(self, label, **kwargs):
        self.entries[label].configure(**kwargs)

    def set_active_widget(self, e):
        self.active_entry = e.widget['text']

    def check_open_cascade(self, e, remaining=None):
        if remaining is None:
            remaining = self.cascade_timing
        if remaining < 0:
            return self.open_cascade(e)
        if e.widget['text'] == self.active_entry:
            return self.after(self.cascade_interval, self.check_open_cascade, e, remaining - self.cascade_interval)

    def check_close_cascade(self, e, remaining=None):
        if remaining is None:
            remaining = self.cascade_timing
        if remaining < 0:
            return self.close_cascade(e)
        if e.widget['text'] != self.active_entry:
            return self.after(self.cascade_interval, self.check_open_cascade, e, remaining - self.cascade_interval)

    def open_cascade(self, e):
        label: ttk.Label = e.widget
        x, y, width, _ = label.bbox()
        pos = f'+{x + width + self.cascade_offset_x}+{y + self.cascade_offset_y}'
        self.cascades[label['text']].display(pos)

    def close_cascade(self, e):
        self.cascades[e.widget['text']].close()

    def display(self, pos=None):
        self.state('normal')
        self.lift()
        if pos is not None:
            self.after(10, self.geometry, pos)

    def close(self):
        self.state('withdrawn')

    def change_theme(self, theme):
        self.colors = theme[self.style]['configure']
        self.configure(**self.colors)


class SALSAMenuBar(ttk.Frame):
    log_key = 'SALSAMenuBar'

    cascade_offset_x = 0
    cascade_offset_y = 0
    padding_x = 5

    menubar_style = 'SALSAMenuBar.TFrame'
    label_style = 'SALSAMenuBar.TLabel'

    def __init__(self, parent, menubar_style=None, label_style=None, theme=None, *args, **kwargs):
        super().__init__(parent, *args, **kwargs)
        theme = menu_default_theme if theme is None else theme
        self.menubar_style = menubar_style if menubar_style is not None else self.menubar_style
        self.label_style = label_style if label_style is not None else self.label_style
        self.configure(style=self.menubar_style)

        self.labels: Dict[str, ttk.Label] = {}
        self.menus: Dict[str, SALSAMenu] = {}
        self.active: str = ''

    def add_cascade(self, label, target_menu: SALSAMenu):
        if label in self.labels:
            print(f'{self.log_key}: Dropdown not created, duplicate name {label}')
        if label in prohibited_labels:
            print(f'{self.log_key}: Dropdown not created, name prohibited {label}')

        self.labels[label] = ttk.Label(self, text=label, style=self.label_style)
        self.labels[label].grid(row=0, column=len(self.menus), sticky='NSEW', ipadx=self.padding_x)
        self.labels[label].bind('<Button-1>', self.handle_click)
        self.labels[label].bind('<Enter>', self.take_focus, add='+')
        self.labels[label].bind('<Leave>', self.release_focus, add='+')
        self.labels[label].bind('<Button-1>', self.print_bbox, add='+')
        self.labels[label].bind('<Escape>', lambda: self.close_menu(label))
        self.menus[label] = target_menu

    def handle_click(self, e):
        state = e.widget.cget('state')
        if not isinstance(state, str):
            state = state.string
        if state == 'disabled':
            return
        self.toggle_menu(e.widget['text'])

    def take_focus(self, e):
        # Put hover highlight here
        if self.active == '':
            return
        state = e.widget.cget('state')
        if not isinstance(state, str):
            state = state.string
        if state == 'disabled':
            return
        if self.active != e.widget['text']:
            self.close_menu(self.active)
        self.active = e.widget['text']
        self.open_menu(e.widget['text'])

    def release_focus(self, e):
        # Put hover highlight here
        if self.active != '':
            return

    def toggle_menu(self, label):
        if self.active == '':
            self.open_menu(label)
        else:
            self.close_menu(label)

    def open_menu(self, label):
        x, y, h = (
        self.labels[label].winfo_rootx(), self.labels[label].winfo_rooty(), self.labels[label].winfo_height())
        pos = f'+{x + self.cascade_offset_x}+{y + h + self.cascade_offset_y}'
        self.menus[label].display(pos)
        self.active = label

    def close_menu(self, label):
        self.menus[label].close()
        self.active = ''

    def entryconfig(self, label, **kwargs):
        self.labels[label].configure(**kwargs)

    def print_bbox(self, e):
        label: ttk.Label = e.widget


if __name__ == '__main__':
    w = tk.Tk()

    menu = SALSAMenuBar(w)
    menu.grid(row=0, column=0, sticky=tk.W)

    default_style = 'clam'
    w.style = ttk.Style()
    w.style.theme_create('menu', parent=default_style, settings=menu_default_theme)

    for item_key, arg_dict in menu_default_theme.items():
        if 'map' not in arg_dict:
            continue
        w.style.map(item_key, **arg_dict['map'])
    w.style.theme_use('menu')

    filemenu = SALSAMenu(menu)
    filemenu.add_command('Hi', command=lambda: print('Hi'))
    filemenu.add_command('Bye', command=lambda: print('Bye'))
    menu.add_cascade('File', filemenu)
    disable_var = tk.IntVar()

    def disable_file_toggle():
        new_state = 'disabled' if disable_var.get() == 1 else 'normal'
        filemenu.entryconfig('Hi', state=new_state)
        filemenu.entryconfig('Bye', state=new_state)
        menu.entryconfig('File', state=new_state)

    viewmenu = SALSAMenu(menu)
    viewmenu.add_checkbutton('Disable File', variable=disable_var, command=lambda: disable_file_toggle(),
                             onvalue=1, offvalue=0)
    menu.add_cascade('View', viewmenu)

    w.mainloop()

    def change_theme(theme):
        filemenu.change_theme(theme=theme)
        viewmenu.change_theme(theme=theme)
