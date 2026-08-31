import json
import tkinter as tk
from tkinter import ttk
from typing import Literal

from SALSA.Common.setting_class import settings
from SALSA.GUI.StringEditor.special_chars_widget import SpecialCharSelectWidget
from SALSA.GUI.Widgets.hover_tooltip import schedule_tooltip
from SALSA.GUI.Widgets.toggle_button import ToggleButton
from SALSA.GUI.Widgets.data_treeview import DataTreeview
from SALSA.GUI.Widgets import widgets as w
from SALSA.GUI.themes import dark_theme, light_theme

tree_settings = {
    'script': {
        'name': {'label': 'Name', 'width': 80, 'stretch': False}
    },
    'string': {
        'location': {'label': 'Name', 'width': 110, 'stretch': False},
        'string': {'label': 'Name', 'width': 100, 'stretch': True}
    }
}

default_tree_width = 100
default_tree_minwidth = 10
default_tree_anchor = tk.W
default_tree_stretch = False
default_tree_label = ''

note_font = {}

tooltips = {
    'no_head': 'Removing the header will allow for 5 lines of body text instead of 4 and delete the current header.',
    'body': 'NOTE: [ and ] are used for open and close quotes respectively. Using " or \' will only give the close quote.'
}

tooltip_delay = 500
warning_time = 1800

quote_types = {
    'US/JP': ('《', '》'),
    'EU': ('«', '»')
}

quote_replacement = {
    'US/JP': [[quote_types['EU'][i], quote_types['US/JP'][i]] for i in range(len(quote_types['US/JP']))],
    'EU': [[quote_types['US/JP'][i], quote_types['EU'][i]] for i in range(len(quote_types['EU']))]
}

rename_widget_offset = 16

default_settings = {
    'recents': json.dumps([])
}


class StringPopup(tk.Toplevel):
    t = 'SALSA - String Editor'
    log_key = 'StrEditPopup'
    w = 600
    h = 500

    def __init__(self, parent, callbacks, name, theme, *args, **kwargs):
        super().__init__(parent, *args, **kwargs)

        self.parent: tk.Tk = parent
        self.callbacks = callbacks
        self.name = name
        self.protocol('WM_DELETE_WINDOW', self.start_close)
        self.theme = theme
        self.configure(**self.theme['Ttoplevel']['configure'])
        self.cur_script = ''
        self.cur_string_id = ''
        self.string_defaults = {}
        self.string_changes = {}
        self.header_invalid = False
        self.cur_encoding: Literal['US/JP', 'EU'] = 'US/JP'
        self.cur_script_encoding: Literal['US/JP', 'EU'] = 'US/JP'
        self.scheduled_tooltip = None
        self.active_tooltip = None
        self.rename_active = False

        if self.log_key not in settings:
            settings.add_group(self.log_key)

        for k, v in default_settings.items():
            if k not in settings[self.log_key]:
                settings.set_single(self.log_key, k, v)

        self.columnconfigure(0, weight=1)
        self.rowconfigure(1, weight=1)

        upper_frame = ttk.Frame(self)
        upper_frame.grid(row=1, column=0, sticky='NSEW')
        upper_frame.columnconfigure(1, weight=1)
        upper_frame.rowconfigure(0, weight=1)

        script_tree_frame = ttk.LabelFrame(upper_frame, text='Scripts')
        script_tree_frame.grid(row=0, column=0, sticky='NSEW', padx=5)
        script_tree_frame.rowconfigure(0, weight=1)

        columns = list(tree_settings['script'].keys())[1:]
        self.scripts_tree = DataTreeview(script_tree_frame, name='script', columns=columns)
        self.scripts_tree.grid(row=0, column=0, sticky='NSEW')
        first = True
        for name, d in tree_settings['script'].items():
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
        script_tree_scrollbar = ttk.Scrollbar(script_tree_frame, orient='vertical', command=self.scripts_tree.yview)
        script_tree_scrollbar.grid(row=0, column=1, sticky=tk.N + tk.S)
        self.scripts_tree.config(yscrollcommand=script_tree_scrollbar.set)
        self.scripts_tree.config(show='tree')
        self.scripts_tree.add_callback('select', self.on_script_select)

        string_tree_frame = ttk.LabelFrame(upper_frame, text='Strings')
        string_tree_frame.grid(row=0, column=1, sticky='NSEW', padx=5)
        string_tree_frame.columnconfigure(0, weight=1)
        string_tree_frame.rowconfigure(0, weight=1)

        columns = list(tree_settings['string'].keys())[1:]
        self.string_tree = DataTreeview(string_tree_frame, name='strings', columns=columns)
        self.string_tree.grid(row=0, column=0, sticky='NSEW')
        first = True
        for name, d in tree_settings['string'].items():
            label = d.get('label', default_tree_label)
            anchor = d.get('anchor', default_tree_anchor)
            minwidth = d.get('minwidth', default_tree_minwidth)
            width = d.get('width', default_tree_width)
            stretch = d.get('stretch', default_tree_stretch)
            if first:
                name = '#0'
                first = False
            self.string_tree.heading(name, text=label, anchor=anchor)
            self.string_tree.column(name, anchor=anchor, minwidth=minwidth, width=width, stretch=stretch)
        string_tree_scrollbar = ttk.Scrollbar(string_tree_frame, orient='vertical', command=self.string_tree.yview)
        string_tree_scrollbar.grid(row=0, column=1, sticky=tk.N + tk.S)
        self.string_tree.config(yscrollcommand=string_tree_scrollbar.set)
        self.string_tree.config(show='tree')
        self.string_tree.add_callback('select', self.edit_string)
        self.string_tree.bind('<ButtonRelease-3>', self.on_string_right_click)
        self.string_tree.bind('<Double-Button-1>', self.double_click_rename)
        self.string_tree.bind('<Double-Button-1>', lambda e: 'break', add='+')

        lower_frame = ttk.Frame(self)
        lower_frame.grid(row=2, column=0, sticky='NSEW', padx=5)
        lower_frame.columnconfigure(0, weight=1)

        encode_frame = ttk.Frame(lower_frame)
        encode_frame.grid(row=0, column=0, sticky=tk.W)

        str_encode_label = ttk.Label(encode_frame, text='String Encoding:   ')
        str_encode_label.grid(row=0, column=0, pady=1)
        self.str_encode_toggle = ToggleButton(encode_frame, on_text='US/JP', off_text='EU', theme=self.theme,
                                              command=self.set_encoding, bd=0, highlightthickness=0)
        self.str_encode_toggle.grid(row=0, column=1)

        no_head_frame = ttk.Frame(lower_frame)
        no_head_frame.grid(row=1, column=0, sticky=tk.W)

        self.no_head_var = tk.IntVar()
        self.no_head = ttk.Checkbutton(no_head_frame, text=' Remove Header', variable=self.no_head_var, onvalue=1, offvalue=0,
                                       command=lambda: self.save('no_head', self.no_head_var.get() == 1), state='disabled')
        self.no_head.grid(row=0, column=0, sticky=tk.W)
        no_head_warning_label = ttk.Label(no_head_frame, text='⚠', anchor='w', justify=tk.LEFT)
        no_head_warning_label.grid(row=0, column=1)
        no_head_warning_label.bind('<Enter>', lambda e: schedule_tooltip(no_head_warning_label, tooltips['no_head']))

        head_frame = ttk.Frame(lower_frame)
        head_frame.grid(row=2, column=0, sticky=tk.W)

        head_label = ttk.Label(head_frame, text='Textbox Header')
        head_label.grid(row=1, column=0, sticky=tk.W)
        self.head_entry = ttk.Entry(head_frame, state='disabled', width=65)
        self.head_entry.grid(row=1, column=0, sticky=tk.W)
        self.head_entry.bind('<FocusIn>', self.on_entry_focus_in)
        self.head_entry.bind('<FocusOut>', lambda e, k='head': self.on_entry_focus_out(k, e))
        self.head_add_quote_label = ttk.Label(head_frame, text='')
        self.head_add_quote_label.grid(row=1, column=1, sticky=tk.W)
        self.head_add_quote_button = ttk.Button(head_frame, text='add header quotes', command=self.add_quotes_to_head)
        self.head_add_quote_button.grid(row=1, column=3, sticky=tk.W, padx=3)

        body_label_frame = ttk.Frame(lower_frame)
        body_label_frame.grid(row=3, column=0, sticky=tk.W)
        body_label = ttk.Label(body_label_frame, text='Textbox Body')
        body_label.grid(row=0, column=0)
        body_tooltip_label = ttk.Label(body_label_frame, text='ⓘ')
        body_tooltip_label.grid(row=0, column=1)
        body_tooltip_label.bind('<Enter>', lambda e: schedule_tooltip(body_tooltip_label, tooltips['body']))

        insert_frame = ttk.Frame(lower_frame)
        insert_frame.grid(row=4, column=0, sticky=tk.E+tk.W)

        self.body_entry = tk.Text(lower_frame, wrap=tk.WORD, height=5, **self.theme['text']['configure'],
                                  undo=True, maxundo=-1, autoseparators=True)
        self.body_entry.grid(row=5, column=0, sticky=tk.W + tk.E, pady='0 5')
        self.body_entry.bind('<FocusIn>', self.on_text_focus_in)
        self.body_entry.bind('<FocusOut>', lambda e, k='body': self.on_text_focus_out(k, e))

        self.sp_chars = SpecialCharSelectWidget(master=self, insert_callback=self.insert_sp_char, theme=theme,
                                                cur_enc=self.cur_encoding, location='above',
                                                recents=json.loads(settings[self.log_key]['recents']), button_num=16)
        self.sp_chars.grid(row=3, column=0, sticky=tk.W, padx=5, pady=5)
        self.bind('<Escape>', lambda e: self.sp_chars.contract_widget(), add='+')

        self.update_scripts()

        self.title(self.t)

        posX = self.parent.winfo_x() + (self.parent.winfo_width() - self.w)//2
        posY = self.parent.winfo_y() + (self.parent.winfo_height() - self.h)//2
        pos = f'{self.w}x{self.h}+{posX}+{posY}'
        self.geometry(pos)

    def _change_editor_state(self, state):
        self.no_head.configure(state=state)
        self.head_entry.configure(state=state)
        self.head_add_quote_button.configure(state=state)
        self.body_entry.configure(state=state)
        self.str_encode_toggle.set_widget_state(state=state)
        self.sp_chars.set_state(state)

    def _clear_editor_fields(self):
        self.no_head_var.set(0)
        self.head_entry.delete(0, tk.END)
        self.body_entry.delete(1.0, tk.END)

    def update_scripts(self):
        cur_tree_height = self.scripts_tree.yview()[0]
        script_tree = self.callbacks['get_scripts']()
        for entry in script_tree:
            self.scripts_tree.insert_entry(parent='', index='end', text=entry['name'], values=[], row_data=entry['row_data'])
        if self.cur_script != '':
            cur_row = self.scripts_tree.get_iid_from_rowdata(self.cur_script)
            if cur_row is not None:
                self.scripts_tree.selection_set((cur_row,))
            else:
                self._clear_editor_fields()
                self._change_editor_state('disabled')
                self.cur_script = ''
            self.scripts_tree.yview_moveto(cur_tree_height)
        else:
            self._clear_editor_fields()
            self._change_editor_state('disabled')

    def on_script_select(self, name, script):
        self.cur_script = script
        self.cur_string_id = ''
        self.string_tree.clear_all_entries()
        self.update_strings()

    def update_strings(self):
        if self.cur_script == '':
            return
        cur_tree_height = self.string_tree.yview()[0]
        open_elements = []
        if len(self.string_tree.get_children('')) > 0:
            open_elements = self.string_tree.get_open_elements()
        self._populate_string_tree()
        if self.cur_string_id != '':
            cur_row = self.string_tree.get_iid_from_rowdata(self.cur_string_id)
            if cur_row is not None:
                self.string_tree.selection_set((cur_row,))
            else:
                self._clear_editor_fields()
                self._change_editor_state('disabled')
                self.cur_string_id = ''
        else:
            self._clear_editor_fields()
            self._change_editor_state('disabled')
        self.string_tree.open_tree_elements(open_elements)
        self.string_tree.yview_moveto(cur_tree_height)

    def _populate_string_tree(self):
        self.string_tree.clear_all_entries()
        headers = list(tree_settings['string'].keys())
        string_tree = self.callbacks['get_string_tree'](self.cur_script, headers)
        parent_list = ['']
        prev_iid = -1
        encoding_set = False
        for entry in string_tree:
            if isinstance(entry, str):
                if entry == 'group':
                    parent_list.append(prev_iid)
                elif entry == 'ungroup':
                    if len(parent_list) == 1:
                        raise RuntimeError(f'{self.log_key}: unable to lower group level, not enough groups left')
                    parent_list.pop()
                else:
                    raise ValueError(f'{self.log_key}: Unknown command in tree list sent to _add_tree_entries')
                continue
            if not encoding_set and self.cur_string_id == '':
                if 'string' in entry:
                    if '《' in entry['string']:
                        self.cur_script_encoding = 'US/JP'
                        encoding_set = True
                        self.str_encode_toggle.set_state_by_value(self.cur_script_encoding)
                        self.sp_chars.set_encoding(self.cur_script_encoding)
                    if '«' in entry['string']:
                        self.cur_script_encoding = 'EU'
                        encoding_set = True
                        self.str_encode_toggle.set_state_by_value(self.cur_script_encoding)
                        self.sp_chars.set_encoding(self.cur_script_encoding)
            kwargs = {'parent': parent_list[-1], 'index': 'end'}
            values = []
            first = True
            for col in headers:
                if col == 'name':
                    if 'group_type' in entry.keys():
                        entry[col] += f' ({entry["group_type"]})'
                if first:
                    kwargs['text'] = entry[col]
                    first = False
                else:
                    values.append(entry[col])
                entry.pop(col)
            kwargs['values'] = values
            kwargs = {**kwargs, **entry}
            prev_iid = self.string_tree.insert_entry(**kwargs)

    def refresh_popup(self):
        self.scripts_tree.clear_all_entries()
        self.update_scripts()
        if self.cur_script == '':
            return
        scr_iid = self.scripts_tree.get_iid_from_rowdata(self.cur_script)
        self.scripts_tree.selection_set(scr_iid)
        self.scripts_tree.focus(scr_iid)
        open_elements = self.string_tree.get_open_elements()
        self.string_tree.clear_all_entries()
        self.update_strings()
        self.string_tree.open_tree_elements(open_elements)
        if self.cur_string_id == '':
            return
        str_iid = self.string_tree.get_iid_from_rowdata(self.cur_string_id)
        self.string_tree.see(str_iid)
        self.string_tree.selection_set(str_iid)
        self.string_tree.focus(str_iid)

    def edit_string(self, name, string_id):
        self._change_editor_state('normal')
        self._clear_editor_fields()

        self.cur_string_id = string_id
        self.string_tree.selection_set((self.string_tree.get_iid_from_rowdata(string_id),))
        no_head, head, body = self.callbacks['get_string_to_edit'](string_id, self.cur_script)
        self.header_invalid = False
        self.body_entry.insert(tk.INSERT, body)
        if head is None:
            self.head_entry.insert(0, 'This instruction does not support headers')
            self._change_editor_state('disabled')
            self.body_entry.configure(state='normal')
            self.header_invalid = True
            return

        no_head = 1 if no_head else 0
        self.no_head_var.set(no_head)
        self.head_entry.insert(0, head)
        if '《' in head:
            self.cur_encoding = 'US/JP'
        elif '«' in head:
            self.cur_encoding = 'EU'
        else:
            self.cur_encoding = self.cur_script_encoding

        self.str_encode_toggle.set_state_by_value(self.cur_encoding)

    def add_quotes_to_head(self):
        left, right = quote_types[self.cur_encoding]
        self.head_entry.insert(0, left)
        self.head_entry.insert(tk.END, right)
        self.save('head', self.head_entry.get())

    def set_encoding(self, new_encoding: Literal['US/JP', 'EU']):
        self.cur_encoding = new_encoding
        head = self.head_entry.get()
        for quote_set in quote_replacement[self.cur_encoding]:
            head = head.replace(quote_set[0], quote_set[1])

        self.head_entry.delete(0, tk.END)
        self.head_entry.insert(0, head)
        self.save('head', self.head_entry.get())
        self.sp_chars.set_encoding(new_encoding)

    def insert_sp_char(self, char):
        if self.cur_script == '' or self.cur_string_id == '':
            return
        self.body_entry.focus_set()
        self.after(10, self.body_entry.insert, tk.INSERT, char)

    @staticmethod
    def on_entry_focus_in(e):
        e.widget.cur_value = e.widget.get()

    def on_entry_focus_out(self, key, e):
        if e.widget.get() == e.widget.cur_value:
            print('same value')
            return
        self.save(key=key, value=e.widget.get())

    @staticmethod
    def on_text_focus_in(e):
        e.widget.cur_value = e.widget.get(1.0, tk.END).rstrip(' \n\t')

    def on_text_focus_out(self, key, e):
        new_value: str = e.widget.get(1.0, tk.END).rstrip(' \n\t')
        if new_value == e.widget.cur_value:
            print('same text')
            return
        self.save(key=key, value=new_value)

    def save(self, key, value):
        self.callbacks['save'](self.cur_script, self.cur_string_id, {key: value})
        self.update_strings()

    def start_close(self):
        self.focus()
        self.after(10, self.close)

    def close(self):
        settings.set_single(self.log_key, 'recents', json.dumps(self.sp_chars.recents))
        self.callbacks['close'](self.name, self)

    def change_theme(self, theme):
        self.theme = theme
        self.body_entry.configure(**self.theme['text']['configure'])
        self.configure(**self.theme['Ttoplevel']['configure'])
        self.str_encode_toggle.change_theme(self.theme)
        self.sp_chars.change_theme(theme)

    # ---------------- #
    # Right Click Menu #
    # ---------------- #

    def on_string_right_click(self, e):
        if len(self.string_tree.get_children('')) == 0:
            return
        sel_iid = self.string_tree.identify_row(e.y)
        self.string_tree.focus(sel_iid)
        self.string_tree.selection_set([sel_iid])
        row_data = self.string_tree.row_data.get(sel_iid, None)

        m = tk.Menu(self, tearoff=0)
        if row_data is None:
            m.add_command(label='Add String Group', command=self.string_group_add)
        if sel_iid != '':
            if row_data is None:
                m.add_command(label='Rename String Group', command=lambda: self.show_rename_widget(sel_iid))
            m.add_command(label='Add String', command=lambda: self.string_add(sel_iid))

            if row_data is not None:
                m.add_command(label='Change String ID', command=lambda: self.show_rename_widget(sel_iid))
                m.add_separator()
                m.add_command(label='Delete String', command=lambda: self.string_delete(sel_iid))
                m.add_separator()
                m.add_command(label='Find String Usages', command=lambda: self.find_usage(sel_iid))

            else:
                m.add_separator()
                m.add_command(label='Delete String Group', command=lambda: self.string_group_delete(sel_iid))
                if len(self.string_tree.get_children(sel_iid)) > 0:
                    m.entryconfigure('Delete String Group', state='disabled')

        m.bind('<Escape>', m.destroy)
        try:
            m.tk_popup(e.x_root, e.y_root)
        finally:
            m.grab_release()

    def string_group_add(self):
        groupid = self.callbacks['add_string_group'](self.cur_script)
        self.update_strings()
        self.callbacks['refresh_sections']()
        self.goto_string(self.cur_script, groupid)

    def string_group_delete(self, sel_iid):
        group_name = self.string_tree.item(sel_iid)['text']
        self.callbacks['delete_string_group'](self.cur_script, group_name)
        self.cur_string_id = ''
        self.update_strings()
        self.callbacks['refresh_sections']()

    def string_add(self, sel_iid):
        if self.string_tree.parent(sel_iid) != '':
            group_iid = self.string_tree.parent(sel_iid)
        else:
            group_iid = sel_iid
        string_group = self.string_tree.item(group_iid)['text']
        new_id = self.callbacks['add_string'](self.cur_script, string_group)
        self.update_strings()
        self.goto_string(self.cur_script, string_group, new_id)

    def string_delete(self, sel_iid):
        string_id = self.string_tree.row_data[sel_iid]
        self.callbacks['delete_string'](self.cur_script, string_id)
        self.cur_string_id = ''
        self.update_strings()

    # ---------------------------------- #
    # Renaming String Groups and Strings #
    # ---------------------------------- #

    def double_click_rename(self, e):
        if self.string_tree.identify_region(e.x, e.y) == 'heading':
            return
        if self.string_tree.identify_column(e.x) != '#0':
            return
        sel_iid = self.string_tree.identify_row(e.y)
        self.show_rename_widget(sel_iid)

    def show_rename_widget(self, sel_iid):
        if self.rename_active:
            return
        self.rename_active = True
        name = self.string_tree.item(sel_iid)['text']
        bbox = list(self.string_tree.bbox(sel_iid, '#0'))
        bbox[0] += rename_widget_offset
        bbox[2] -= rename_widget_offset
        widget = w.LabelNameEntry(self.string_tree)
        widget.insert(0, name)
        widget.place(x=bbox[0], y=bbox[1], width=bbox[2], height=bbox[3])
        widget.columnconfigure(1, weight=1)
        widget.focus_set()
        widget.selection_range(0, tk.END)
        widget.bind('<Return>', lambda event: self.try_rename(widget, sel_iid))
        widget.bind('<FocusOut>', lambda event: self.try_rename(widget, sel_iid))
        widget.bind('<Escape>', lambda event: self.destroy_rename_widget(widget))

    def try_rename(self, widget, sel_iid):
        old_name = self.string_tree.row_data.get(sel_iid, None)
        is_section = False
        if old_name is None:
            old_name = self.string_tree.item(sel_iid)['text']
            is_section = True
        new_name = widget.get()
        if new_name == old_name:
            return self.destroy_rename_widget(widget)
        if self.callbacks['is_sect_name_used'](self.cur_script, new_name):
            schedule_tooltip(widget, 'This name is in use', delay=0, min_time=warning_time, position='above center', is_warning=True)
            return self.shake_widget(widget)
        if new_name == '':
            schedule_tooltip(widget, 'A name is required', delay=0, min_time=warning_time, position='above center', is_warning=True)
            return self.after(20, self.shake_widget, widget)
        self.destroy_rename_widget(widget)
        self.rename_active = False
        if is_section:
            self.callbacks['rename_string_group'](self.cur_script, old_name, new_name)
        else:
            self.callbacks['rename_string'](self.cur_script, old_name, new_name)
            self.string_tree.row_data[sel_iid] = new_name
            if self.cur_string_id == old_name:
                self.cur_string_id = new_name
        self.string_tree.item(sel_iid, text=new_name)
        self.callbacks['refresh_sections']()
        self.update_strings()

    def shake_widget(self, widget):
        shake_speed = 70
        shake_intensity = 2
        widget_x = widget.winfo_x()
        self.string_tree.after(shake_speed * 1, lambda: widget.place_configure(x=widget_x + shake_intensity))
        self.string_tree.after(shake_speed * 2, lambda: widget.place_configure(x=widget_x - shake_intensity))
        self.string_tree.after(shake_speed * 3, lambda: widget.place_configure(x=widget_x + shake_intensity))
        self.string_tree.after(shake_speed * 4, lambda: widget.place_configure(x=widget_x - shake_intensity))
        self.string_tree.after(shake_speed * 5, lambda: widget.place_configure(x=widget_x))

    def destroy_rename_widget(self, widget):
        self.rename_active = False
        widget.destroy()
            
    # ----------------- #
    # Goto string/group #
    # ----------------- #

    def goto_string(self, script, group, string=None):
        if script is not None:
            self.update_scripts()
            s_iid = self.scripts_tree.get_iid_from_rowdata(script)
            self.scripts_tree.selection_set(s_iid)
            self.on_script_select('script', script)
            return self.after(10, self.goto_string, None, group, string)
        self.string_tree.close_all_groups()
        if string is None:
            return self.goto_string_section(group)
        s_iid = self.string_tree.get_iid_from_rowdata(string)
        self.string_tree.see(s_iid)
        self.string_tree.selection_set(s_iid)
        self.string_tree.focus(s_iid)

    def goto_string_section(self, name):
        items = self.string_tree.get_children()
        for item in items:
            if self.string_tree.item(item)['text'] == name:
                self.string_tree.selection_set([item,])
                return

    # ---------- #
    # Find Usage #
    # ---------- #

    def find_usage(self, sel_iid):
        string = self.string_tree.item(sel_iid, "text")
        self.callbacks['find_usage'](string)
