import tkinter as tk
from tkinter import ttk
import json

from SALSA.GUI.themes import dark_theme, light_theme
from SALSA.GUI.Widgets.widgets import ScrollLabelFrame
from SALSA.Common.setting_class import settings
from SALSA.Common.constants import sep
from SALSA.GUI.Widgets import widgets as w

default_headers = {
    'instruction': ['inst_ID', 'name'],
    'parameter': ['param_ID', 'name', 'type', 'mask', 'is_signed', 'default_value']
}

header_settings = {
    'instruction': {
        'inst_ID': {'label': 'ID', 'width': 40, 'stretch': False},
        'name': {'label': 'Name', 'width': 250, 'stretch': False}
    },
    'parameter': {
        'param_ID': {'label': 'ID', 'width': 40, 'stretch': False},
        'name': {'label': 'Name', 'width': 150, 'stretch': True},
        'type': {'label': 'Type', 'width': 100, 'stretch': True},
        'mask': {'label': 'Mask', 'width': 100, 'stretch': True},
        'is_signed': {'label': 'Is Signed', 'width': 50, 'stretch': True},
        'default_value': {'label': 'Default Value', 'width': 100, 'stretch': True}
    }
}

param_edit_fields = {
    'name': {'widget': ttk.Entry, 'args': {}, 'kwargs': {}},
    'default_value': {'widget': 'param_edit', 'args': {}, 'kwargs': {}}
}

default_tree_width = 100
default_tree_minwidth = 10
default_tree_anchor = tk.W
default_tree_stretch = False
default_tree_label = ''


class InstructionEditorView(tk.Toplevel):

    log_name = 'InstEditorView'
    nm_key = '<<ChangeTheme>>'
    nm_priority = 1

    def __init__(self, parent, callbacks, theme, *args, **kwargs):
        super().__init__(parent, *args, **kwargs)

        self.title('SALSA - Instruction Editor')
        self.callbacks = callbacks

        if self.log_name not in settings.keys():
            settings[self.log_name] = {}

        # if 'headers' not in settings[self.log_name].keys():
        settings.set_single(self.log_name, 'headers', json.dumps(default_headers))

        self.visible_headers = json.loads(settings[self.log_name]['headers'])

        self.columnconfigure(1, weight=2)
        self.rowconfigure(1, weight=1)
        self.columnconfigure(2, weight=1)

        self.protocol('WM_DELETE_WINDOW', self.on_close)

        # Save, undo, redo buttons

        self.save = ttk.Button(self, text='Save', command=self.on_save)
        self.save.grid(row=0, column=0, sticky=tk.W+tk.N, padx=2, pady=2)
        self.save['state'] = 'disabled'

        inst_tree_frame = ttk.Frame(self)
        inst_tree_frame.grid(row=1, column=0, sticky='NSEW', padx=2, pady='0 2')
        inst_tree_frame.rowconfigure(0, weight=1)

        columns = list(header_settings['instruction'].keys())[1:]
        self.inst_list_tree = ttk.Treeview(inst_tree_frame, name='instruction', columns=columns, padding=0)
        self.inst_list_tree.grid(row=0, column=0, sticky='NSEW')
        first = True
        for name, d in header_settings['instruction'].items():
            label = d.get('label', default_tree_label)
            anchor = d.get('anchor', default_tree_anchor)
            minwidth = d.get('minwidth', default_tree_minwidth)
            width = d.get('width', default_tree_width)
            stretch = d.get('stretch', default_tree_stretch)
            if first:
                name = '#0'
                first = False
            self.inst_list_tree.heading(name, text=label, anchor=anchor)
            self.inst_list_tree.column(name, anchor=anchor, minwidth=minwidth, width=width, stretch=stretch)
        self.inst_list_tree['displaycolumns'] = self.visible_headers['instruction'][1:]
        inst_tree_scrollbar = ttk.Scrollbar(inst_tree_frame, orient='vertical', command=self.inst_list_tree.yview)
        inst_tree_scrollbar.grid(row=0, column=1, sticky=tk.N+tk.S)
        self.inst_list_tree.config(yscrollcommand=inst_tree_scrollbar.set)
        self.inst_list_tree.bind('<<TreeviewSelect>>', self.on_select_instruction)

        self.inst_details_frame = ttk.Frame(self)
        self.inst_details_frame.grid(row=1, column=1, sticky='NSEW')
        self.inst_details_frame.columnconfigure(0, weight=1)
        self.inst_details_frame.rowconfigure(0, weight=1)
        self.inst_details_frame.rowconfigure(1, weight=1)

        top_details_frame = ttk.Frame(self.inst_details_frame)
        top_details_frame.grid(row=0, column=0, sticky='NSEW')
        top_details_frame.columnconfigure(0, weight=1)
        top_details_frame.columnconfigure(1, weight=1)
        top_details_frame.rowconfigure(6, weight=1)

        # Instruction details - Hardcoded

        id_frame = ttk.Frame(top_details_frame)
        id_frame.grid(row=0, column=0, sticky=tk.W)
        id_label_label = ttk.Label(id_frame, text=f'Instruction ID: ')
        id_label_label.grid(row=0, column=0)
        self.id_label = ttk.Label(id_frame, text='')
        self.id_label.grid(row=0, column=1)

        skip_frame = ttk.Frame(top_details_frame)
        skip_frame.grid(row=0, column=1, sticky=tk.W)
        skip_label_label = ttk.Label(skip_frame, text=f'Frame Refresh: ')
        skip_label_label.grid(row=0, column=0)
        self.skip_label = ttk.Label(skip_frame, text='')
        self.skip_label.grid(row=0, column=1)

        param2_frame = ttk.Frame(top_details_frame)
        param2_frame.grid(row=1, column=0, sticky=tk.W)
        param2_label_label = ttk.Label(param2_frame, text=f'FxnParameter2: ')
        param2_label_label.grid(row=0, column=0)
        self.param2_label = ttk.Label(param2_frame, text='')
        self.param2_label.grid(row=0, column=1)

        location_frame = ttk.Frame(top_details_frame)
        location_frame.grid(row=1, column=1, sticky=tk.W)
        location_label_label = ttk.Label(location_frame, text=f'Function Location: ')
        location_label_label.grid(row=0, column=0)
        self.location_label = ttk.Label(location_frame, text='')
        self.location_label.grid(row=0, column=1)

        link_frame = ttk.Frame(top_details_frame)
        link_frame.grid(row=2, column=0, sticky=tk.W)
        link_label_label = ttk.Label(link_frame, text=f'Link Type: ')
        link_label_label.grid(row=0, column=0)
        self.link_label = ttk.Label(link_frame, text='')
        self.link_label.grid(row=0, column=1)

        loop_params_frame = ttk.Frame(top_details_frame)
        loop_params_frame.grid(row=3, column=0, sticky=tk.W)
        loop_params_label_label = ttk.Label(loop_params_frame, text=f'Loop Params: ')
        loop_params_label_label.grid(row=0, column=0)
        self.loop_params_label = ttk.Label(loop_params_frame, text='')
        self.loop_params_label.grid(row=0, column=1)

        self.warning_frame = ttk.Frame(top_details_frame)
        self.warning_frame.grid(row=4, column=0, sticky=tk.W)
        warning_label_label = ttk.Label(self.warning_frame, text=f'Warning: ')
        warning_label_label.grid(row=0, column=0)
        self.warning_label = ttk.Label(self.warning_frame, text='')
        self.warning_label.grid(row=0, column=1)
        self.no_warning = ttk.Label(top_details_frame, text=' ')
        self.no_warning.grid(row=4, column=0, columnspan=2, sticky='NSEW')

        # Instruction details - Editable

        name_frame = ttk.Frame(top_details_frame)
        name_frame.grid(row=5, column=0, columnspan=2, sticky=tk.W+tk.E)
        name_label_label = ttk.Label(name_frame, text='Name:')
        name_label_label.grid(row=0, column=0)
        self.details_name_entry = ttk.Entry(name_frame)
        self.details_name_entry.grid(row=0, column=1, sticky='nsew')
        self.details_name_entry.bind('<FocusIn>', self.on_entry_focus_in)
        self.details_name_entry.bind('<FocusOut>', lambda e, k='name': self.on_entry_focus_out(k, e))
        self.details_name_label = ttk.Label(name_frame, text='', anchor=tk.W)
        self.details_name_label.grid(row=0, column=1, sticky='nsew')

        desc_frame = ttk.LabelFrame(top_details_frame, text='Description')
        desc_frame.grid(row=6, column=0, sticky='NSEW', columnspan=2)
        desc_frame.columnconfigure(0, weight=1)
        desc_frame.rowconfigure(0, weight=1)
        self.details_desc_text = w.ThemedScrolledText(desc_frame, wrap=tk.WORD, height=15, width=50)
        self.details_desc_text.grid(row=0, column=0, sticky='NSEW')
        self.details_desc_text.bind('<FocusIn>', self.on_text_focus_in)
        self.details_desc_text.bind('<FocusOut>', lambda e, k='description': self.on_text_focus_out(k, e))

        # Instruction parameter setup frame
        self.parameters_frame = ttk.LabelFrame(self.inst_details_frame, text='Parameters')
        self.parameters_frame.grid(row=1, column=0, sticky='NSEW')
        self.parameters_frame.columnconfigure(0, weight=1)
        self.parameters_frame.rowconfigure(0, weight=1)

        columns = list(header_settings['parameter'].keys())[1:]
        self.param_list_tree = ttk.Treeview(self.parameters_frame, name='parameter', columns=columns)
        self.param_list_tree.grid(row=0, column=0, sticky='NSEW')
        first = True
        for name, d in header_settings['parameter'].items():
            label = d.get('label', default_tree_label)
            anchor = d.get('anchor', default_tree_anchor)
            minwidth = d.get('minwidth', default_tree_minwidth)
            width = d.get('width', default_tree_width)
            stretch = d.get('stretch', default_tree_stretch)
            if first:
                name = '#0'
                first = False
            self.param_list_tree.heading(name, text=label, anchor=anchor)
            self.param_list_tree.column(name, anchor=anchor, minwidth=minwidth, width=width, stretch=stretch)
        self.param_list_tree['displaycolumns'] = self.visible_headers['parameter'][1:]
        param_tree_scrollbar = ttk.Scrollbar(self.parameters_frame, orient='vertical', command=self.param_list_tree.yview)
        param_tree_scrollbar.grid(row=0, column=1, sticky=tk.N+tk.S)
        self.param_list_tree.config(yscrollcommand=param_tree_scrollbar.set)

        self.param_list_tree.bind('<Double-1>', self.on_param_double_click)

        # Instruction notes

        inst_notes_frame = ttk.LabelFrame(self, text='Notes')
        inst_notes_frame.grid(row=1, column=2, sticky='NSEW')
        inst_notes_frame.rowconfigure(1, weight=1)
        inst_notes_frame.rowconfigure(3, weight=1)
        inst_notes_frame.columnconfigure(0, weight=1)

        default_notes_label = ttk.Label(inst_notes_frame, text='Instruction Notes')
        default_notes_label.grid(row=0, column=0, sticky=tk.W)

        self.default_notes_text = w.ThemedScrolledText(inst_notes_frame, wrap=tk.WORD, height=15, width=35)
        self.default_notes_text.grid(row=1, column=0, sticky='NSEW')
        self.default_notes_text.bind('<FocusIn>', self.on_text_focus_in)
        self.default_notes_text.bind('<FocusOut>', lambda e, k='default_notes': self.on_text_focus_out(k, e))

        self.default_notes_frame = ScrollLabelFrame(inst_notes_frame, has_label=False, size={'width': 50, 'height': 25})
        self.default_notes_frame.grid(row=1, column=0, sticky='NSEW')

        self.default_notes_msg = tk.Message(self.default_notes_frame.scroll_frame)
        self.default_notes_msg.grid(row=0, column=0, sticky='NSEW')
        self.default_notes_frame.canvas.bind("<Configure>", lambda e: self.default_notes_msg.configure(width=e.width - 10), add='+')

        def config_scrollframe():
            new_width = self.default_notes_frame.scroll_frame.winfo_width() - 10
            self.default_notes_msg.configure(width=new_width)
        self.after(10, config_scrollframe)

        user_notes_label = ttk.Label(inst_notes_frame, text='User Notes')
        user_notes_label.grid(row=2, column=0, sticky=tk.W)

        self.user_notes_text = w.ThemedScrolledText(inst_notes_frame, wrap=tk.WORD, height=15, width=35)
        self.user_notes_text.grid(row=3, column=0, sticky='NSEW')
        self.user_notes_text.bind('<FocusIn>', self.on_text_focus_in)
        self.user_notes_text.bind('<FocusOut>', lambda e, k='user_notes': self.on_text_focus_out(k, e))

        self.user_notes_blocker = ttk.Label(inst_notes_frame, text=' ')
        self.user_notes_blocker.grid(row=3, column=0, sticky='NSEW')

        user_type = self.callbacks['user_type']()

        if user_type == 'default':
            self.default_notes_frame.grid_forget()
        elif user_type == 'user':
            self.user_notes_blocker.grid_forget()

        self.change_theme(theme)

    # ------------------------------------------------------------ #
    # Methods to generate or use the instruction or parameter tree #
    # ------------------------------------------------------------ #

    def get_headers(self, tree_key):
        if tree_key is None:
            return {k: list(header_settings[k].keys()) for k in header_settings.keys()}
        return list(header_settings[tree_key].keys())

    def on_select_instruction(self, e):
        if self.inst_list_tree.identify_region(e.x, e.y) == 'heading':
            return
        selected_iid = self.inst_list_tree.focus()
        inst_id = self.inst_list_tree.item(selected_iid).get('text')
        self.callbacks['on_select_instruction'](inst_id)

    # -------------------------------------------------- #
    # Methods to detect and change entry and text fields #
    # -------------------------------------------------- #

    def on_entry_focus_in(self, e):
        e.widget.cur_value = e.widget.get()

    def on_entry_focus_out(self, key, e):
        if e.widget.get() == e.widget.cur_value:
            print('same value')
            return
        self.callbacks['set_change'](key=key, value=e.widget.get())

    def on_text_focus_in(self, e):
        e.widget.cur_value = e.widget.get(1.0, tk.END)

    def on_text_focus_out(self, key, e):
        new_value = e.widget.get(1.0, tk.END).rstrip('\n')
        if new_value == e.widget.cur_value:
            print('same text')
            return
        self.callbacks['set_change'](key=key, value=new_value)

    # -------------------------------------------------- #
    # Methods to detect and change parameter tree values #
    # -------------------------------------------------- #

    def on_param_double_click(self, e):
        # Find the region clicked, only edit if region is a cell
        if self.param_list_tree.identify_region(e.x, e.y) != 'cell':
            return

        # get column that was clicked
        column = self.param_list_tree.identify_column(e.x)
        column_id = int(column[1:])
        column_name = self.visible_headers['parameter'][column_id]

        # get item id and values associated with the item
        selected_iid = self.param_list_tree.focus()
        selected_values = self.param_list_tree.item(selected_iid)
        current_value = selected_values.get('values')[column_id-1]
        param_id = selected_values.get('text')

        if self.callbacks['check_locked']([param_id, column_name]):
            print('This field is locked for editing')
            # Add dialog box to indicate that the selected field is not editable
            return

        if column_name not in param_edit_fields:
            print('This column is not available for editing')
            return

        widget_details = param_edit_fields[column_name]
        if widget_details['widget'] == 'param_edit':
            self.callbacks['show_param_editor'](param_id, column_id)
            return

        # get cell bounding box
        cell_bbox = self.param_list_tree.bbox(selected_iid, column)

        edit_widget = widget_details['widget'](self.parameters_frame, *widget_details['args'], **widget_details['kwargs'])
        edit_widget.place(x=cell_bbox[0], y=cell_bbox[1], w=cell_bbox[2], h=cell_bbox[3])

        edit_widget.editing_column_index = column_id
        edit_widget.editing_item_iid = selected_iid
        edit_widget.editing_column_name = column_name
        edit_widget.default_value = current_value
        if isinstance(edit_widget, tk.Entry):
            edit_widget.insert(0, current_value)
            edit_widget.select_range(0, tk.END)

        edit_widget.focus()

        edit_widget.bind('<Escape>', lambda event: edit_widget.destroy())
        edit_widget.bind('<FocusOut>', self.on_param_change)
        edit_widget.bind('<Return>', self.on_param_change)

    def on_param_change(self, e):
        new_text = e.widget.get()
        if new_text == e.widget.default_value:
            return

        selected_iid = e.widget.editing_item_iid
        column_index = e.widget.editing_column_index
        column_name = e.widget.editing_column_name

        current_values = self.param_list_tree.item(selected_iid).get('values')
        current_values[column_index - 1] = new_text

        self.param_list_tree.item(selected_iid, values=current_values)

        param_id = self.param_list_tree.item(selected_iid).get('text')
        self.callbacks['set_change'](f'{param_id}{sep}{column_name}', new_text)

        e.widget.destroy()

    def on_close(self):
        self.focus()
        self.after(10, func=self.callbacks['on_close'])

    def on_save(self):
        self.callbacks['save']()
        self.save['state'] = 'disabled'

    def set_value(self, param_id, column_id, value):
        sel_iid = self.param_list_tree.get_children('')[int(param_id)]
        values = self.param_list_tree.item(sel_iid).get('values')
        values[column_id - 1] = value
        self.param_list_tree.item(sel_iid, values=values)

    def change_theme(self, theme):

        # self - toplevel
        self.configure(background=theme['.']['configure']['background'])

        # details_desc_text - scrolledtext
        self.details_desc_text.configure(**theme['text']['configure'])
        # default_notes_text - scrolledtext
        self.default_notes_text.configure(**theme['text']['configure'])
        self.default_notes_msg.configure(**theme['Tmessage']['configure'])
        # default_notes_text - scrolledtext
        self.user_notes_text.configure(**theme['Tmessage']['configure'])

        # default_notes_frame - ScrollCanvasFrame
        self.default_notes_frame.change_theme(theme)
