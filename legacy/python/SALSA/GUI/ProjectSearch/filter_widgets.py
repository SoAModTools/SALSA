import tkinter as tk
from tkinter import ttk

import SALSA.GUI.Widgets.widgets as w


class FilterSearchWidget(w.ValidatedMixin, ttk.Entry):

    def __init__(self, parent, callbacks, default_text, *a, **kw):
        super().__init__(parent, *a, **kw)
        self.callbacks = callbacks
        self.default_text = default_text

        self.bind('<FocusIn>', lambda e: self.remove_default_text())
        self.bind('<FocusOut>', lambda e: self.add_default_text())

        self.add_default_text()

    def _key_validate(self, proposed, current, char, event, index, action):
        if proposed == self.default_text:
            return True
        self.callbacks['search'](proposed)

        return True

    def add_default_text(self):
        if len(self.get()) == 0:
            self.insert(0, self.default_text)
            self.configure(style='tooltip.TEntry')

    def remove_default_text(self):
        if self.get() == self.default_text:
            self.delete(0, tk.END)
            self.configure(style='TEntry')


class SearchableToggleDataTreeview(ttk.Treeview):

    def __init__(self, parent, *args, **kwargs):

        super().__init__(parent, *args, selectmode='none', **kwargs)

        self.row_data = {}
        self.all_rows = []
        self.selected_texts = []

        self.bind('<ButtonRelease-1>', self.toggle_selection)

    def insert_entry(self, row_data, **kwargs):
        self.all_rows.append(kwargs)
        self.insert(**kwargs)
        self.row_data[kwargs['text']] = row_data

    def clear_rows(self):
        for child in self.get_children():
            self.delete(child)

    def reset_tree(self):
        self.row_data.clear()
        self.all_rows.clear()
        self.selected_texts.clear()
        self.clear_rows()

    def filter_entries(self, s_string):
        self.selection_clear()
        self.clear_rows()
        for row in self.all_rows:
            if len(s_string) > 0 and s_string.lower() not in row['text'].lower():
                continue

            iid = self.insert(**row)
            if row['text'] in self.selected_texts:
                self.selection_add(iid)

    def toggle_selection(self, e):
        if self.identify_region(e.x, e.y) != 'tree':
            return
        iid = self.identify_row(e.y)
        if iid == '':
            return

        if iid in self.selection():
            self.selection_remove(iid)
            self.selected_texts.remove(self.item(iid)['text'])
        else:
            self.selection_add(iid)
            self.selected_texts.append(self.item(iid)['text'])

    def clear_selection(self):
        self.selection_clear()
        self.selected_texts.clear()

    def selection_clear(self):
        for iid in self.selection():
            self.selection_remove(iid)

