import os.path
import tkinter as tk
from tkinter import ttk

from SALSA.GUI.ProjectSearch.filter_widgets import SearchableToggleDataTreeview, FilterSearchWidget
from SALSA.Project.project_searcher import loc_tokens, filter_tokens, PrjResult, PrjResultGroup
from SALSA.Common.constants import alt_sep
from SALSA.GUI.Widgets.data_treeview import DataTreeview

header_settings = {
    'results': {
        'link': {'label': 'Script', 'width': 300, 'stretch': True}
    }
}

filter_trees = {
    'sct:': {
        'filter': {'label': 'Script', 'width': 80, 'stretch': True}
    },
    'sect:': {
        'filter': {'label': 'Section', 'width': 90, 'stretch': True}
    },
    'inst:': {
        'filter': {'label': 'Instruction ID', 'width': 150, 'stretch': True}
    },
}

default_text = {
    'sct:': 'Look for Scripts',
    'sect:': 'Look for Sections',
    'inst:': 'Look for Insts'
}

default_tree_width = 100
default_tree_minwidth = 10
default_tree_anchor = tk.W
default_tree_stretch = False
default_tree_label = ''

filter_labels = {
    'loc:inst': 'Base Insts',
    'loc:param': 'Parameter Values',
    'loc:dialog': 'Dialog Strings'
}


class ProjectSearchPopup(tk.Toplevel):

    t = 'Project Search'
    log_key = 'PRJSearchPopup'
    w = 250
    h = 400

    def __init__(self, parent, callbacks, name, theme, *args, search_string=None, **kwargs):
        super().__init__(parent, *args, **kwargs)

        self.name = name
        self.callbacks = callbacks
        self.configure(**theme['Ttoplevel']['configure'])
        self.columnconfigure(0, weight=1)
        self.rowconfigure(2, weight=1)

        self.search_icon = tk.BitmapImage(file=os.path.join(os.path.dirname(__file__),
                                                            "button_icons/search_icon_20x20.xbm"),
                                          foreground=theme['TButton']['configure']['foreground'],
                                          background=theme['TButton']['configure']['background'])
        self.filter_icon = tk.BitmapImage(file=os.path.join(os.path.dirname(__file__),
                                                            "button_icons/filtering_icon_20x20.xbm"),
                                          foreground=theme['TButton']['configure']['foreground'],
                                          background=theme['TButton']['configure']['background'])

        search_frame = ttk.Frame(self)
        search_frame.grid(row=0, column=0, sticky='NSEW')
        search_frame.columnconfigure(0, weight=1)

        if search_string is None:
            search_string = ""
        self.search_string = tk.StringVar(search_frame, search_string)
        self.search_entry = ttk.Entry(search_frame, textvariable=self.search_string)
        self.search_entry.grid(row=0, column=0, sticky='NSEW')
        self.search_entry.bind('<Return>', func=lambda event: self.start_search(), add='+')
        self.search_entry.bind('<KP_Enter>', func=lambda event: self.start_search(), add='+')

        self.search_button = ttk.Button(search_frame, image=self.search_icon, command=self.start_search)
        self.search_button.grid(row=0, column=1, sticky=tk.N + tk.S + tk.E)

        self.filter_button = ttk.Button(search_frame, image=self.filter_icon, command=self.toggle_filter_frame)
        self.filter_button.grid(row=0, column=2, sticky=tk.N + tk.S + tk.E)
        self.filter_is_up = False

        self.filter_frame = ttk.Frame(self)
        self.filter_frame.grid(row=2, column=0, sticky='NSEW')

        ff_upper = ttk.Frame(self.filter_frame)
        ff_upper.grid(row=0, column=0, padx=10, pady='10 0', sticky='NSEW')
        fw_up_label = ttk.Label(ff_upper, text='Search Locations:')
        fw_up_label.grid(row=0, column=0)

        self.loc_checkbutton_vars = {}
        loc_checkbuttons = {}
        column = 1
        for k, v in loc_tokens.items():
            self.loc_checkbutton_vars[k] = tk.IntVar(self, 0)
            loc_checkbuttons[k] = ttk.Checkbutton(ff_upper, offvalue=0, onvalue=1, text=filter_labels[k],
                                                  variable=self.loc_checkbutton_vars[k])
            loc_checkbuttons[k].grid(row=0, column=column)
            ff_upper.columnconfigure(column, weight=1)
            column += 1

        self.filter_frame.columnconfigure(0, weight=1)
        self.filter_frame.rowconfigure(1, weight=1)
        fw_lower = ttk.Frame(self.filter_frame)
        fw_lower.grid(row=1, column=0, padx=10, pady=10, sticky='NSEW')
        fw_lower.rowconfigure(0, weight=1)
        column = 0
        self.filter_trees = {}
        self.filter_searches = {}
        for k, v in filter_tokens.items():
            fw_lower.columnconfigure(column, weight=1)

            ff = ttk.Frame(fw_lower)
            ff.grid(row=0, column=column, padx=2, pady=2, sticky='NSEW')
            ff.columnconfigure(0, weight=1)
            ff.rowconfigure(0, weight=1)

            columns = list(filter_trees[k].keys())[1:]
            self.filter_trees[k] = SearchableToggleDataTreeview(ff, name='results', columns=columns)
            self.filter_trees[k].grid(row=0, column=0, sticky='NSEW')
            first = True
            for name, d in filter_trees[k].items():
                label = d.get('label', default_tree_label)
                anchor = d.get('anchor', default_tree_anchor)
                minwidth = d.get('minwidth', default_tree_minwidth)
                width = d.get('width', default_tree_width)
                stretch = d.get('stretch', default_tree_stretch)
                if first:
                    name = '#0'
                    first = False
                self.filter_trees[k].heading(name, text=label, anchor=anchor)
                self.filter_trees[k].column(name, anchor=anchor, minwidth=minwidth, width=width, stretch=stretch)
            filter_scroll = ttk.Scrollbar(ff, orient='vertical',
                                          command=self.filter_trees[k].yview)
            filter_scroll.grid(row=0, column=1, sticky=tk.N + tk.S)
            self.filter_trees[k].config(yscrollcommand=filter_scroll.set)

            self.filter_searches[k] = FilterSearchWidget(ff, callbacks={'search': self.filter_trees[k].filter_entries},
                                                         default_text=default_text[k])
            self.filter_searches[k].grid(row=1, column=0, columnspan=2, sticky=tk.E+tk.W, pady=2)

            clear_button = ttk.Button(ff, text=f'Clear {filter_trees[k]["filter"]["label"]} filters',
                                      command=self.filter_trees[k].clear_selection)
            clear_button.grid(row=2, column=0, sticky=tk.NW)
            column += 1

        self.keep_case = tk.IntVar(self, 0)
        keep_case_checkbox = ttk.Checkbutton(self, offvalue=0, onvalue=1, text='Keep Case', variable=self.keep_case)
        keep_case_checkbox.grid(row=1, column=0, sticky=tk.W)

        self.result_frame = ttk.Frame(self)
        self.result_frame.grid(row=2, column=0, sticky='NSEW')
        self.result_frame.columnconfigure(0, weight=1)
        self.result_frame.rowconfigure(0, weight=1)

        columns = list(header_settings['results'].keys())[1:]
        self.result_tree = DataTreeview(self.result_frame, name='results', columns=columns, can_open=True, can_move=False,
                                        callbacks={'select': self.on_result_select})
        self.result_tree.grid(row=0, column=0, sticky='NSEW')
        first = True
        for name, d in header_settings['results'].items():
            label = d.get('label', default_tree_label)
            anchor = d.get('anchor', default_tree_anchor)
            minwidth = d.get('minwidth', default_tree_minwidth)
            width = d.get('width', default_tree_width)
            stretch = d.get('stretch', default_tree_stretch)
            if first:
                name = '#0'
                first = False
            self.result_tree.heading(name, text=label, anchor=anchor)
            self.result_tree.column(name, anchor=anchor, minwidth=minwidth, width=width, stretch=stretch)
        error_tree_scrollbar = ttk.Scrollbar(self.result_frame, orient='vertical', command=self.result_tree.yview)
        error_tree_scrollbar.grid(row=0, column=1, sticky=tk.N + tk.S)
        self.result_tree.config(yscrollcommand=error_tree_scrollbar.set)

        close_button = ttk.Button(self, text='Close', command=self.close)
        close_button.grid(row=3, column=0, sticky=tk.E)

        self.protocol('WM_DELETE_WINDOW', self.close)

        self.populate_filter_trees()

    # -------------------- #
    # Filter Frame Methods #
    # -------------------- #

    def toggle_filter_frame(self):
        if self.filter_is_up:
            self.result_frame.tkraise()
            self.filter_is_up = False
        else:
            self.filter_frame.tkraise()
            self.filter_is_up = True

    def clear_filter_trees(self):
        for tree in self.filter_trees.values():
            for child in tree.get_children():
                tree.delete(child)

    def populate_filter_trees(self):
        self.clear_filter_trees()
        filters = self.callbacks['get_filters']()
        for k, tree in self.filter_trees.items():
            for item in filters[k]:
                tree.insert_entry(parent='', index='end', text=item[0], values=[], row_data=item[1])

    # -------------- #
    # Search Methods #
    # -------------- #

    def start_search(self):
        if self.filter_is_up:
            self.toggle_filter_frame()

        self.result_tree.clear_all_entries()
        self.result_tree.insert_entry('', '', [])
        self.result_tree.insert_entry('', 'Searching...', [])

        self.after(10, self.continue_search)

    def continue_search(self):
        search_string = self.search_string.get() + self.get_filter_items()
        if len(search_string) == 0:
            return
        results = self.callbacks['search'](search_string, self.keep_case.get() == 1)
        self.result_tree.clear_all_entries()

        if results is not None:
            if len(results) == 0:
                results = None

        self.populate_result_tree(results)

    def get_filter_items(self):
        filters = []
        for key, var in self.loc_checkbutton_vars.items():
            if var.get() == 1:
                filters.append(f'{key}')

        for key, tree in self.filter_trees.items():
            if len(tree.selection()) == 0:
                continue
            for item in tree.selection():
                filters.append(f'{key}{tree.row_data.get(tree.item(item)["text"])}')

        if len(filters) == 0:
            return ''
        return ' ' + ' '.join(filters)

    # ------------------- #
    # Result Tree Methods #
    # ------------------- #

    def populate_result_tree(self, results):
        if results is None:
            self.result_tree.insert_entry(parent='', index='end', text='', values=[], row_data=None)
            self.result_tree.insert_entry(parent='', index='end', text='No Results Found', values=[], row_data=None)
            return
        for result in results:
            self.insert_result_group(result)

    def insert_result_group(self, entry, parent=''):
        if isinstance(entry, PrjResult):
            self.result_tree.insert_entry(parent=parent, index='end', text=entry.display, values=[], row_data=entry.row_data)
        elif isinstance(entry, PrjResultGroup):
            p_iid = self.result_tree.insert_entry(parent=parent, index='end', text=entry.name, values=[], row_data=None)
            for e in entry.contents:
                self.insert_result_group(e, p_iid)

    def on_result_select(self, name, row_data):
        if row_data is None:
            return
        sel_iid = self.result_tree.focus()
        script_entry = self.result_tree.parent(sel_iid)
        script = self.result_tree.item(script_entry)['text']
        location = self.result_tree.item(self.result_tree.parent(script_entry))['text']
        if location == 'dialog':
            group, s_id = row_data.split(alt_sep)
            self.callbacks['goto_dialog'](script, group, s_id)
            return
        pts = row_data.split(alt_sep)
        section = pts[0]
        inst = pts[1]
        if len(pts) > 2:
            param = pts[2]
        else:
            param = None
        self.callbacks['goto_result'](script=script, sect=section, inst=inst, param=param)

    # --------------------- #
    # Popup Control Methods #
    # --------------------- #

    def close(self):
        self.callbacks['close'](self.name, self)

    def change_theme(self, theme):
        self.configure(**theme['Ttoplevel']['configure'])
        self.search_icon.configure(foreground=theme['TButton']['configure']['foreground'],
                                   background=theme['TButton']['configure']['background'])
        self.filter_icon.configure(foreground=theme['TButton']['configure']['foreground'],
                                   background=theme['TButton']['configure']['background'])
