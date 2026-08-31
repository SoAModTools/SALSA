import tkinter as tk
from tkinter import ttk

from SALSA.Common.constants import sep, alt_alt_sep, alt_sep
from SALSA.GUI.themes import dark_theme, light_theme


class InstGroupHandlerDialog(tk.Toplevel):

    def __init__(self, master, title, radio_vars, entry_vars, head_labels, row_labels, inst_id, new_inst_id,
                 theme, end_callback, end_kwargs, *args, **kwargs):
        super().__init__(master, *args, **kwargs)
        self.title(title)
        self.configure(**theme['Ttoplevel']['configure'])
        self.end_callback = end_callback
        self.end_kwargs = end_kwargs

        self.transient(master)
        self.grab_set()
        self.wm_focusmodel('active')
        self.protocol('WM_DELETE_WINDOW', lambda event: self.close(True))

        x, y, w, h = master.winfo_rootx(), master.winfo_rooty(), master.winfo_width(), master.winfo_height()
        x += int(w/2.1)
        y += int(h/2.1)
        self.after(10, self.center, x, y)

        self.labels = head_labels
        self.row_labels = row_labels
        self.radio_vars = radio_vars
        self.entry_vars = entry_vars
        self.new_id = new_inst_id

        message_frame = ttk.Frame(self)
        message_frame.grid(row=0, column=0, padx=10, pady=20)

        cur_col = 0
        for label in head_labels:
            ttk.Label(message_frame, text=label).grid(row=0, column=cur_col)
            cur_col += 1

        for i, row_label in enumerate(row_labels):
            cur_col = 0
            if inst_id == 0:
                row_label = row_label.split(sep)[1]
            ttk.Label(message_frame, text=row_label).grid(row=i + 1, column=cur_col)
            cur_col += 1
            for j in range(len(head_labels)-1):
                ttk.Radiobutton(message_frame, text='', variable=radio_vars[i], value=cur_col).grid(row=i + 1, column=cur_col)
                cur_col += 1
            if new_inst_id == 3:
                ttk.Entry(message_frame, textvariable=entry_vars[i]).grid(row=i + 1, column=cur_col)

        button_frame = ttk.Frame(self)
        button_frame.grid(row=1, column=0, sticky=tk.E, pady=5, padx=5)

        ok_button = ttk.Button(button_frame, text='OK', command=lambda: self.close(False))
        ok_button.grid(row=0, column=0, padx=5)
        cancel_button = ttk.Button(button_frame, text='Cancel', command=lambda: self.close(True))
        cancel_button.grid(row=0, column=1, padx=5)

    def center(self, x, y):
        x -= self.winfo_width()//2
        y -= self.winfo_height()//2
        self.geometry(f'+{x}+{y}')

    def close(self, cancel: bool):
        self.destroy()
        response = ''
        if cancel:
            response = 'cancel'
        else:
            for i, row_label in enumerate(self.row_labels):
                if i > 0:
                    response += f'{alt_alt_sep}'
                choice = self.labels[self.radio_vars[i].get()]
                response += f'{row_label}{alt_sep}{choice}'
                if self.new_id == 3 and choice == 'Insert in Switch Case':
                    response += f'{alt_sep} {self.entry_vars[i].get()}'

        self.end_kwargs['result'] = response
        self.end_callback(**self.end_kwargs)
