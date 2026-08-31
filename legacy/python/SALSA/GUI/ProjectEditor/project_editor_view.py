import tkinter as tk
import tkinter.ttk as ttk
import json
from typing import List, Dict

import SALSA.GUI.Widgets.widgets as w
from SALSA.GUI.Widgets.data_treeview import DataTreeview
from SALSA.GUI.ProjectEditor.inst_group_handler import InstGroupHandlerDialog
from SALSA.Common.setting_class import settings
from SALSA.BaseInstructions.bi_defaults import loop_count_name

default_headers = {
    'script': ['name'],
    'section': ['name', 'absolute_offset'],
    'instruction': ['ungrouped_position', 'name', 'condition', 'delay_param', 'skip_refresh', 'base_id', 'absolute_offset'],
    'parameter': ['ID', 'name', 'type', 'value']
}

default_header_order = {
    'script': ['name', 'section_num'],
    'section': ['name', 'absolute_offset'],
    'instruction': ['ungrouped_position', 'name', 'condition', 'base_id', 'delay_param',
                    'skip_refresh', 'synopsis', 'absolute_offset'],
    'parameter': ['ID', 'name', 'type', 'value', 'formatted_value'],
}

header_settings = {
    'script': {
        'name': {'label': 'Name', 'width': 80, 'stretch': True},
        'section_num': {'label': 'Section #', 'width': 50, 'stretch': True}
    },
    'section': {
        'name': {'label': 'Name', 'width': 200, 'stretch': True},
        'absolute_offset': {'label': 'Offset', 'width': 100, 'stretch': True}
    },
    'instruction': {
        'ungrouped_position': {'label': 'Pos', 'width': 180, 'stretch': True},
        'name': {'label': 'Name', 'width': 180, 'stretch': True},
        'condition': {'label': 'Condition', 'width': 250, 'stretch': True},
        'base_id': {'label': 'ID', 'width': 40, 'stretch': False},
        'delay_param': {'label': 'Delay', 'width': 40, 'stretch': False},
        'skip_refresh': {'label': 'SR', 'width': 40, 'stretch': False},
        'synopsis': {'label': 'Synopsis', 'width': 50, 'stretch': True},
        'absolute_offset': {'label': 'Offset', 'width': 100, 'stretch': True}
    },
    'parameter': {
        'ID': {'label': 'ID', 'width': 50, 'stretch': False},
        'name': {'label': 'Name', 'width': 80, 'stretch': True},
        'type': {'label': 'Type', 'width': 70, 'stretch': True},
        'value': {'label': 'Value', 'width': 200, 'stretch': True},
        'formatted_value': {'label': 'ID', 'width': 100, 'stretch': False}
    }
}

default_tree_width = 100
default_tree_minwidth = 10
default_tree_anchor = tk.W
default_tree_stretch = False
default_tree_label = ''

flash_count = 3
flash_dur = 600


class ProjectEditorView(ttk.Frame):
    log_name = 'PrjEditorView'

    def __init__(self, parent, theme, *args, **kwargs):
        super().__init__(parent, *args, **kwargs)
        if self.log_name not in settings.keys():
            settings[self.log_name] = {}

        self.callbacks = {}
        self.theme = theme

        # if 'headers' not in settings[self.log_name].keys():
        settings.set_single(self.log_name, 'headers', json.dumps(default_headers))

        self.visible_headers: Dict[str, List[str]] = json.loads(settings[self.log_name]['headers'])

        self.columnconfigure(0, weight=1)
        self.rowconfigure(1, weight=1)

        header_frame = ttk.Frame(self)
        header_frame.grid(row=0, column=0, sticky=tk.W+tk.E)
        self.save_button = ttk.Button(header_frame, text='Save',
                                      command=lambda: self.use_future_callback('save_project'))
        self.save_button.grid(row=0, column=0)

        mem_offset_label = ttk.Label(header_frame, text='Memory Offset: 0x')
        mem_offset_label.grid(row=0, column=1, sticky=tk.W, padx='30 0')
        self.mem_offset_var = tk.StringVar(header_frame, '')
        mem_offset_entry = w.HexEntry(master=header_frame, textvariable=self.mem_offset_var, hex_max_length=8,
                                      hex_min_length=0, width=10)
        mem_offset_entry.grid(row=0, column=2, sticky=tk.W)
        mem_offset_button = ttk.Button(header_frame, text='Set',
                                       command=lambda: self.use_future_callback('set_mem_offset'))
        mem_offset_button.grid(row=0, column=3)
        self.cur_mem_offset = None

        refresh_offset = ttk.Button(header_frame, text='Refresh Offsets',
                                    command=lambda: self.use_future_callback('refresh_offsets'))
        refresh_offset.grid(row=0, column=4, padx=5)

        search_button = ttk.Button(header_frame, text='Search in Project',
                                   command=lambda: self.use_future_callback('show_search'))
        search_button.grid(row=0, column=5, padx=5)

        separator = ttk.Label(header_frame, text=' ')
        separator.grid(row=0, column=6, sticky='NSEW')
        header_frame.columnconfigure(6, weight=1)

        self.error_button = ttk.Button(header_frame, text='Show Encoding Errors', style='error.TButton',
                                       command=lambda: self.use_future_callback('show_project_errors'))
        self.error_button.grid(row=0, column=7, sticky=tk.E)
        self.error_button.grid_remove()

        self.pane_frame = ttk.PanedWindow(self, orient=tk.HORIZONTAL)
        self.pane_frame.grid(row=1, column=0, sticky='NSEW')

        script_tree_frame = ttk.Frame(self.pane_frame, width=400)
        script_tree_frame.grid(row=1, column=0, sticky='NSEW')
        script_tree_frame.rowconfigure(1, weight=1)
        script_tree_frame.columnconfigure(0, weight=1)

        script_tree_label = ttk.Label(script_tree_frame, text='Project Scripts')
        script_tree_label.grid(row=0, column=0, sticky=tk.W)
        self.pane_frame.add(script_tree_frame, weight=1)

        columns = list(header_settings['script'].keys())[1:]
        self.scripts_tree = DataTreeview(script_tree_frame, name='script', columns=columns, selectmode='none',
                                         can_open=True)
        self.scripts_tree.grid(row=1, column=0, sticky='NSEW')
        first = True
        for name, d in header_settings['script'].items():
            label = d.get('label', default_tree_label)
            anchor = d.get('anchor', default_tree_anchor)
            minwidth = d.get('minwidth', default_tree_minwidth)
            width = d.get('width', default_tree_width)
            stretch = d.get('stretch', default_tree_stretch)
            if first:
                name = '#0'
                first = False
            self.scripts_tree.heading(name, text=label, anchor=anchor)
            self.scripts_tree.column(name, anchor=anchor, minwidth=minwidth, width=width, stretch=stretch)
        self.scripts_tree['displaycolumns'] = self.visible_headers['script'][1:]
        self.script_tree_scrollbar = ttk.Scrollbar(script_tree_frame, orient='vertical', command=self.scripts_tree.yview)
        self.script_tree_scrollbar.grid(row=1, column=1, sticky=tk.N + tk.S)
        self.scripts_tree.config(yscrollcommand=self.script_tree_scrollbar.set)

        section_tree_frame = ttk.Frame(self.pane_frame, width=400)
        section_tree_frame.grid(row=0, column=0, sticky='NSEW')
        section_tree_frame.rowconfigure(1, weight=1)
        section_tree_frame.columnconfigure(0, weight=1)

        section_tree_label = ttk.Label(section_tree_frame, text='Sections (Subscripts)')
        section_tree_label.grid(row=0, column=0, sticky=tk.W)
        self.pane_frame.add(section_tree_frame, weight=1)

        columns = list(header_settings['section'].keys())[1:]
        self.sections_tree = DataTreeview(section_tree_frame, name='section', columns=columns,
                                          can_move=True, can_select_multiple=True, selectmode='none', theme=theme)
        self.sections_tree.configure('columns')
        self.sections_tree.grid(row=1, column=0, sticky='NSEW')
        first = True
        for name, d in header_settings['section'].items():
            label = d.get('label', default_tree_label)
            anchor = d.get('anchor', default_tree_anchor)
            minwidth = d.get('minwidth', default_tree_minwidth)
            width = d.get('width', default_tree_width)
            stretch = d.get('stretch', default_tree_stretch)
            if first:
                name = '#0'
                first = False
            self.sections_tree.heading(name, text=label, anchor=anchor)
            self.sections_tree.column(name, anchor=anchor, minwidth=minwidth, width=width, stretch=stretch)
        self.sections_tree['displaycolumns'] = self.visible_headers['section'][1:]
        self.section_tree_scrollbar = ttk.Scrollbar(section_tree_frame, orient='vertical', command=self.sections_tree.yview)
        self.section_tree_scrollbar.grid(row=1, column=1, sticky=tk.N + tk.S)
        self.sections_tree.config(yscrollcommand=self.section_tree_scrollbar.set)
        self.sections_tree.bind('<Double-1>', lambda e: self.callbacks['show_rename_widget']('section', e))

        self.inst_tree_frame = ttk.Frame(self.pane_frame, width=400)
        self.inst_tree_frame.grid(row=0, column=0, sticky='NSEW')
        self.inst_tree_frame.rowconfigure(1, weight=1)
        self.inst_tree_frame.columnconfigure(0, weight=1)

        inst_tree_label = ttk.Label(self.inst_tree_frame, text='Instructions')
        inst_tree_label.grid(row=0, column=0, sticky=tk.W)
        self.pane_frame.add(self.inst_tree_frame, weight=1)

        columns = list(header_settings['instruction'].keys())[1:]
        self.insts_tree = DataTreeview(self.inst_tree_frame, name='instruction', columns=columns,
                                       can_move=True, can_select_multiple=True, selectmode='none', theme=self.theme,
                                       prevent_extreme_selection=True, keep_group_ends=True)
        self.insts_tree.grid(row=1, column=0, sticky='NSEW')
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
            self.insts_tree.heading(name, text=label, anchor=anchor)
            self.insts_tree.column(name, anchor=anchor, minwidth=minwidth, width=width, stretch=stretch)
        self.insts_tree['displaycolumns'] = self.visible_headers['instruction'][1:]
        self.inst_tree_scrollbar = ttk.Scrollbar(self.inst_tree_frame, orient='vertical', command=self.insts_tree.yview)
        self.inst_tree_scrollbar.grid(row=1, column=1, sticky=tk.N + tk.S)
        self.insts_tree.config(yscrollcommand=self.inst_tree_scrollbar.set)
        self.insts_tree.bind('<Double-1>', lambda e: self.callbacks['show_rename_widget']('instruction', e))

        # Instruction details frame setup
        self.inst_frame = ttk.Frame(self.pane_frame, width=400)
        self.inst_frame.grid(row=0, column=0, sticky='NSEW')
        self.inst_frame.columnconfigure(0, weight=1)
        self.inst_frame.rowconfigure(2, weight=5)
        self.inst_frame.rowconfigure(3, weight=1)
        self.pane_frame.add(self.inst_frame, weight=3)

        inst_frame_label = ttk.Label(self.inst_frame, text='Instruction Details')
        inst_frame_label.grid(row=0, column=0, sticky=tk.W)

        inst_top_frame = ttk.Frame(self.inst_frame)
        inst_top_frame.grid(row=1, column=0, sticky=tk.E + tk.W)
        inst_top_frame.columnconfigure(0, weight=1)

        self.inst_label = ttk.Label(inst_top_frame, text='ID - Name')
        self.inst_label.grid(row=0, column=0, sticky=tk.W + tk.N)

        skip_frame = ttk.Frame(inst_top_frame)
        skip_frame.grid(row=0, column=1, sticky=tk.E)

        self.skip_ckeck_var = tk.IntVar()
        skip_check = ttk.Checkbutton(skip_frame, variable=self.skip_ckeck_var,
                                     command=lambda: self.callbacks['update_field']('skip_refresh',
                                                                                    (self.skip_ckeck_var.get() == 1)))
        skip_check.grid(row=0, column=0)
        skip_label = ttk.Label(skip_frame, text='Skip Frame Refresh')
        skip_label.grid(row=0, column=1)
        self.skip_error_label = ttk.Label(skip_frame, text='')
        self.skip_error_label.grid(row=1, column=0, columnspan=2, sticky='NSEW')

        self.delay_frame_label = ttk.Label(inst_top_frame, text='Instruction Delay')
        self.delay_frame = ttk.LabelFrame(inst_top_frame, labelwidget=self.delay_frame_label)
        self.delay_frame.grid(row=2, column=0, columnspan=2, sticky='NSEW')
        self.delay_frame.columnconfigure(0, weight=1)

        self.delay_label = ttk.Label(self.delay_frame, text=' ', relief=tk.RAISED)
        self.delay_label.grid(row=0, column=0, sticky='NSEW')
        self.delay_label.bind('<Double-1>', lambda e: self.on_param_double_click('delay', e))

        self.inst_desc_frame = w.ScrollLabelFrame(self.inst_frame, text='Description',
                                                  size={'width': 100, 'height': 200})
        self.inst_desc_frame.grid(row=2, column=0, sticky='NSEW')
        self.inst_desc_frame.columnconfigure(0, weight=1)
        self.inst_desc_frame.rowconfigure(0, weight=1)

        self.inst_description = tk.Message(self.inst_desc_frame.scroll_frame)
        self.inst_desc_frame.canvas.bind("<Configure>", lambda e: self.inst_description.configure(width=e.width - 10),
                                         add='+')
        self.inst_description.grid(row=0, column=0, sticky='NSEW')

        param_frame = ttk.LabelFrame(self.inst_frame, text='Parameters')
        param_frame.grid(row=3, column=0, sticky='NSEW')
        param_frame.columnconfigure(0, weight=1)
        param_frame.rowconfigure(0, weight=1)

        columns = list(header_settings['parameter'].keys())[1:]
        self.param_tree = DataTreeview(param_frame, name='parameter', columns=columns, can_open=False)
        self.param_tree.grid(row=0, column=0, sticky='NSEW')
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
            self.param_tree.heading(name, text=label, anchor=anchor)
            self.param_tree.column(name, anchor=anchor, minwidth=minwidth, width=width, stretch=stretch)
        self.param_tree['displaycolumns'] = self.visible_headers['parameter'][1:]
        self.param_tree_scrollbar = ttk.Scrollbar(param_frame, orient='vertical', command=self.param_tree.yview)
        self.param_tree_scrollbar.grid(row=0, column=1, sticky=tk.N + tk.S)
        self.param_tree.config(yscrollcommand=self.param_tree_scrollbar.set)
        self.param_tree.bind('<Double-1>', lambda e: self.on_param_double_click('param', e))
        self.param_tree.bind('<Button-3>', self.param_right_click)

        link_frame = ttk.LabelFrame(self.inst_frame, text='Links')
        link_frame.grid(row=4, column=0, sticky='NSEW')
        link_frame.columnconfigure(0, weight=1)
        link_frame.columnconfigure(2, weight=1)

        link_in_label = ttk.Label(link_frame, text='Incoming Links')
        link_in_label.grid(row=0, column=0, sticky=tk.W)
        self.link_in = w.ScrollFrame(link_frame, size={'width': 100, 'height': 100})
        self.link_in.grid(row=1, column=0, sticky=tk.W + tk.E, padx='0 5')
        self.link_in.columnconfigure(0, weight=1)
        self.link_in.rowconfigure(0, weight=1)

        link_out_label = ttk.Label(link_frame, text='Outgoing Links')
        link_out_label.grid(row=0, column=2, sticky=tk.W)
        self.link_out = w.ScrollFrame(link_frame, size={'width': 100, 'height': 100})
        self.link_out.grid(row=1, column=2, sticky=tk.W + tk.E, padx='5 0')
        self.link_out.columnconfigure(0, weight=1)
        self.link_out.rowconfigure(0, weight=1)

    def use_future_callback(self, key):
        self.callbacks[key]()

    def get_headers(self, tree_key=None, get_all=False):
        if tree_key is None:
            if get_all:
                return {k: list(header_settings[k].keys()) for k in header_settings.keys()}
            return {k: list(header_settings[k].keys()) for k in header_settings.keys()}

        if get_all:
            return header_settings[tree_key]
        return list(header_settings[tree_key].keys())

    def update_field(self, field_name, value):
        self.callbacks['update_field'](field_name, value)

    def add_callback(self, key, callback):
        self.callbacks[key] = callback

    def add_and_bind_tree_callbacks(self, callbacks):
        self.callbacks = callbacks
        self.scripts_tree.bind('<Button-3>', self.callbacks['show_header_selection_menu'])
        self.scripts_tree.bind('<Button-3>', self.callbacks['show_script_menu'], add='+')
        self.sections_tree.bind('<Button-3>', self.callbacks['show_header_selection_menu'])
        self.sections_tree.bind('<Button-3>', self.callbacks['show_section_menu'], add='+')
        self.insts_tree.bind('<Button-3>', self.callbacks['show_header_selection_menu'])
        self.insts_tree.bind('<Button-3>', self.callbacks['show_inst_menu'], add='+')

    def on_param_double_click(self, param, e):
        if param != 'delay':
            # get item id and values associated with the item
            selected_iid = self.param_tree.focus()
            param = self.param_tree.row_data[selected_iid]
            if param is None:
                return
            type_column_ind = list(self.param_tree['columns']).index('type')
            if loop_count_name in self.param_tree.item(selected_iid)['values'][type_column_ind]:
                return
        self.callbacks['edit_param'](param, e)

    def param_right_click(self, e):
        if e.widget.identify('region', e.x, e.y) == 'heading':
            return self.callbacks['show_header_selection_menu'](e)
        row = self.param_tree.identify_row(e.y)
        self.param_tree.focus(row)
        self.param_tree.selection_set([row])
        self.callbacks['param_rcm'](row, e)

    def sort_visible_headers(self, tree):
        self.visible_headers[tree] = [_ for _ in default_header_order[tree] if _ in self.visible_headers[tree]]

    def get_header_order(self, tree):
        return default_header_order[tree]

    def fit_headers(self, cur_tree: DataTreeview):
        widget_width = cur_tree.winfo_width()
        widths = {}
        first = '#0'
        width_sum = 0
        for col in self.visible_headers[cur_tree.name]:
            if first:
                col = first
                first = ''
            widths[col] = cur_tree.column(col, 'width')
            width_sum += widths[col]

        if width_sum == widget_width:
            return

        width_ratios = {}
        for k, v in widths.items():
            width_ratios[k] = v / width_sum

        for k, v in width_ratios.items():
            cur_tree.column(k, width=int(v * widget_width))

    def inst_group_handling(self, cur_inst_id, new_id, children, end_callback, end_kwargs, force_delete_all):
        # create message to decide how to handle group entries
        children = [{k: children[k]} for k in children.keys()]
        if cur_inst_id == 0:
            labels = ['Group']
        elif cur_inst_id == 3:
            labels = ['Switch Case']
        else:
            return ''

        labels += ['Delete', 'Move Above', 'Move Below']
        if new_id == 0:
            labels += ['Insert in If', 'Insert in Else']
        elif new_id == 3:
            labels += ['Insert in Switch Case']

        radio_vars = []
        entry_vars = []
        row_labels = []

        for child in children:
            radio_vars.append(tk.IntVar(self, 1))
            row_labels.append(list(child.keys())[0])
            if new_id == 3:
                entry_vars.append(tk.IntVar(self))

        handler = InstGroupHandlerDialog(self, title='Group Handling', radio_vars=radio_vars, entry_vars=entry_vars,
                                         head_labels=labels, row_labels=row_labels, inst_id=cur_inst_id,
                                         new_inst_id=new_id, theme=self.theme, end_callback=end_callback,
                                         end_kwargs=end_kwargs)

        if force_delete_all:
            handler.close(cancel=False)

    def flash_delay_param(self, count=flash_count, in_flash=False):
        if in_flash:
            self.delay_frame_label.configure(style='TLabel')
            self.delay_frame.configure(style='TLabelframe')
            self.delay_label.configure(style='TLabel')
            if count > 1:
                self.after(flash_dur//2, self.flash_delay_param, count - 1, False)
        else:
            self.delay_frame_label.configure(style='flash.TLabel')
            self.delay_frame.configure(style='flash.TLabelframe')
            self.delay_label.configure(style='flash.TLabel')
            self.after(flash_dur//2, self.flash_delay_param, count, True)

    @staticmethod
    def _lock_scroll(e):
        return "break"

    def lock_tree_scrolling(self):
        self.scripts_tree.bind("<MouseWheel>", self._lock_scroll)
        self.scripts_tree.bind("<Button-4>", self._lock_scroll)
        self.scripts_tree.bind("<Button-5>", self._lock_scroll)
        self.script_tree_scrollbar.config(command= lambda *args: None)

        self.sections_tree.bind("<MouseWheel>", self._lock_scroll)
        self.sections_tree.bind("<Button-4>", self._lock_scroll)
        self.sections_tree.bind("<Button-5>", self._lock_scroll)
        self.section_tree_scrollbar.config(command= lambda *args: None)

        self.insts_tree.bind("<MouseWheel>", self._lock_scroll)
        self.insts_tree.bind("<Button-4>", self._lock_scroll)
        self.insts_tree.bind("<Button-5>", self._lock_scroll)
        self.inst_tree_scrollbar.config(command= lambda *args: None)

    def unlock_tree_scrolling(self):
        self.scripts_tree.unbind("<MouseWheel>")
        self.scripts_tree.unbind("<Button-4>")
        self.scripts_tree.unbind("<Button-5>")
        self.script_tree_scrollbar.config(command=self.scripts_tree.yview)

        self.sections_tree.unbind("<MouseWheel>")
        self.sections_tree.unbind("<Button-4>")
        self.sections_tree.unbind("<Button-5>")
        self.section_tree_scrollbar.config(command=self.sections_tree.yview)

        self.insts_tree.unbind("<MouseWheel>")
        self.insts_tree.unbind("<Button-4>")
        self.insts_tree.unbind("<Button-5>")
        self.inst_tree_scrollbar.config(command=self.insts_tree.yview)


    def change_theme(self, theme):
        self.theme = theme

        self.inst_description.configure(**theme['Tmessage']['configure'])

        self.inst_desc_frame.change_theme(theme)
        self.link_in.change_theme(theme)
        self.link_out.change_theme(theme)
        self.insts_tree.set_theme(theme)
        self.sections_tree.set_theme(theme)
