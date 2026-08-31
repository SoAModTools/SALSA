import json
import tkinter as tk
from tkinter import ttk

import SALSA.GUI.Widgets.widgets as w
from SALSA.GUI.Widgets.data_treeview import DataTreeview
from SALSA.Common.setting_class import settings


class SearchWidget(w.ValidatedMixin, ttk.Entry):

    def __init__(self, parent, callbacks, *a, **kw):
        super().__init__(parent, *a, **kw)

        self.callbacks = callbacks

    def _key_validate(self, proposed, current, char, event, index, action):
        self.callbacks['search'](proposed)

        return True


default_headers = {
    'instruction': ['inst_ID', 'name']
}

header_settings = {
    'instruction': {
        'inst_ID': {'label': 'ID', 'width': 40, 'stretch': False},
        'name': {'label': 'Name', 'width': 250, 'stretch': False}
    }
}


default_tree_width = 100
default_tree_minwidth = 10
default_tree_anchor = tk.W
default_tree_stretch = False
default_tree_label = ''


class InstructionSelectorWidget(ttk.Frame):

    log_name = 'InstSelView'
    entry_max = 10
    shift_up_idx = 7
    shift_down_idx = 3

    def __init__(self, parent, callbacks, inst_trace, *args, **kwargs):
        super().__init__(parent, *args, **kwargs)

        self.callbacks = callbacks
        self.inst_trace = inst_trace

        if self.log_name not in settings.keys():
            settings[self.log_name] = {}

        # if 'headers' not in settings[self.log_name].keys():
        settings.set_single(self.log_name, 'headers', json.dumps(default_headers))

        self.visible_headers = json.loads(settings[self.log_name]['headers'])

        search_label = ttk.Label(self, text='Search: ')
        search_label.grid(row=0, column=0)
        self.search_var = tk.StringVar(self, '')
        self.search = SearchWidget(self, textvariable=self.search_var, callbacks={'search': self.update_search})
        self.search.grid(row=0, column=1, sticky=tk.E+tk.W)

        r_callbacks = {
            'select': self.select_inst
        }
        self.dropdown_root = self.winfo_toplevel()
        self.dropdown_popup = tk.Toplevel(self.dropdown_root)
        self.dropdown_popup.withdraw()
        self.dropdown_popup.overrideredirect(True)
        self.dropdown_popup.transient(self.dropdown_root)
        self.dropdown_popup.columnconfigure(0, weight=1)
        self.dropdown_popup.rowconfigure(0, weight=1)

        self.result_dropdown = DataTreeview(self.dropdown_popup, name='result', callbacks=r_callbacks)
        self.result_dropdown.grid(row=0, column=0, sticky=tk.NSEW)
        self.result_dropdown.heading('#0', text='Relevant Instructions')
        self.result_dropdown.configure(show='tree')
        self.result_dropdown.configure(height=1)

        self._last_dropdown_geometry = None
        self._last_root_position = self.get_root_position()
        self._root_configure_binding = self.dropdown_root.bind('<Configure>', self.reposition_dropdown_on_root_move, add='+')
        self.bind('<Destroy>', self.destroy_dropdown, add='+')

        self.relevant_entries = []
        self.relevant_index = 0

        self.search.focus_set()

        self.search.bind('<KeyPress-Escape>', lambda event: self.cancel())
        self.search.bind('<KeyPress-Return>', self.select_inst_by_enter)
        self.search.bind('<KeyPress-Down>', self.move_to_results)

        self.result_dropdown.bind('<KeyPress-Escape>', lambda event: self.cancel())
        self.result_dropdown.bind('<KeyPress-Return>', self.select_inst_by_enter)
        self.result_dropdown.bind('<KeyPress-Up>', self.handle_dropdown_up_keypress)
        self.result_dropdown.bind('<KeyPress-Down>', self.handle_dropdown_down_keypress)

        # Bind scroll wheel for Windows and macOS
        self.result_dropdown.bind("<MouseWheel>", self.on_mouse_wheel)
        # Bind scroll wheel for Linux
        self.result_dropdown.bind("<Button-4>", self.on_mouse_wheel)
        self.result_dropdown.bind("<Button-5>", self.on_mouse_wheel)

        self.result_dropdown.bind("<Button-1>", self.select_inst_by_click)

        self.result_dropdown.bind("<Motion>", self.on_hover)

        self.after(10, self.update_search, '')
        self.after_idle(self.reposition_dropdown)

    def get_root_position(self):
        return self.dropdown_root.winfo_rootx(), self.dropdown_root.winfo_rooty()

    def reposition_dropdown_on_root_move(self, event=None):
        if event is not None and event.widget is not self.dropdown_root:
            return

        root_position = self.get_root_position()
        if root_position == self._last_root_position:
            return

        self._last_root_position = root_position
        self.reposition_dropdown()

    def reposition_dropdown(self):
        if not self.winfo_exists() or not self.dropdown_popup.winfo_exists():
            return
        if not self.search.winfo_ismapped():
            self.dropdown_popup.withdraw()
            return

        dropdown_width = max(self.search.winfo_width(), self.result_dropdown.winfo_reqwidth())
        dropdown_height = self.result_dropdown.winfo_reqheight()
        x = self.search.winfo_rootx()
        y = self.search.winfo_rooty() + self.search.winfo_height()
        new_geometry = f'{dropdown_width}x{dropdown_height}+{x}+{y}'

        if new_geometry != self._last_dropdown_geometry:
            self.dropdown_popup.geometry(new_geometry)
            self._last_dropdown_geometry = new_geometry
            self.dropdown_popup.update()
        if self.dropdown_popup.state() == 'withdrawn':
            self.dropdown_popup.deiconify()
        self.dropdown_popup.lift(self.dropdown_root)

    def update_search(self, current):
        self.update_relevant_entries(self.callbacks['get_relevant'](current, exclude_modifiers=True))

    def update_relevant_entries(self, relevant_entries, start_index=0):
        self.relevant_entries = relevant_entries
        self.relevant_index = start_index
        self.update_menu(start_index)

    def update_menu(self, start_index=0):
        self.result_dropdown.clear_all_entries()
        if len(self.relevant_entries) == 0:
            self.result_dropdown.configure(height=1)
            self.after_idle(self.reposition_dropdown)
            return
        rows = min(self.entry_max, len(self.relevant_entries))
        rows = max(rows, 1)
        if len(self.relevant_entries) <= rows:
            displayed_entries = self.relevant_entries
        else:
            start_index = min(start_index, len(self.relevant_entries) - rows)
            displayed_entries = self.relevant_entries[start_index:rows+start_index]

        self.result_dropdown.configure(height=rows)
        for entry in displayed_entries:
            entry_parts = entry.split(' - ')
            inst_id = entry_parts[0]
            self.result_dropdown.insert_entry(parent='', index='end', text=entry, values=[], row_data=inst_id)
        self.result_dropdown.selection_set(self.result_dropdown.get_children('')[0])
        self.after_idle(self.reposition_dropdown)

    def select_inst(self, name, new_ID):
        end_callback = self.finish_select_inst
        end_kwargs = {'result': None, 'new_ID': new_ID}
        children = self.callbacks['get_children'](*self.inst_trace)
        if self.callbacks['is_group'](*self.inst_trace):
            return self.callbacks['group_inst_handler'](children=children, new_id=new_ID, end_callback=end_callback, end_kwargs=end_kwargs)
        end_callback(**end_kwargs)

    def finish_select_inst(self, result, new_ID):
        if result is not None:
            if result == 'cancel':
                return
        self.callbacks['set_inst_id'](*self.inst_trace, new_id=new_ID, change_type=result)
        self.close()
        self.callbacks['update_inst']('instruction', self.inst_trace[2])

    def move_to_results(self, e):
        children = self.result_dropdown.get_children('')
        if len(children) == 0:
            return
        self.result_dropdown.focus_set()
        if len(children) == 1:
            target_index = 0
        else:
            target_index = 1
        targeted_item = self.result_dropdown.get_children('')[target_index]
        self.result_dropdown.focus(targeted_item)
        self.result_dropdown.selection_set([targeted_item])

    def move_to_search(self, e):
            self.search.focus_set()

    def handle_dropdown_up_keypress(self, e):
        children = e.widget.get_children('')
        selection = e.widget.selection()
        if len(children) == 0 or len(selection) == 0:
            return
        cur_item_ind = children.index(selection[0])
        if cur_item_ind == 0:
            self.search.focus_set()
        elif cur_item_ind < self.shift_down_idx and self.relevant_index > 0:
            self.try_shift_results_down()

    def handle_dropdown_down_keypress(self, e):
        children = e.widget.get_children('')
        selection = e.widget.selection()
        if len(children) == 0 or len(selection) == 0:
            return
        cur_item_ind = children.index(selection[0])
        if cur_item_ind + 1 > self.shift_up_idx and self.relevant_index + self.entry_max < len(self.relevant_entries):
            self.try_shift_results_up()

    def try_shift_results_up(self):
        cur_relevant_max = self.relevant_index + self.entry_max
        if cur_relevant_max == len(self.relevant_entries):
            return

        children = self.result_dropdown.get_children('')
        selection = self.result_dropdown.selection()
        if len(children) == 0 or len(selection) == 0:
            return
        cur_item_ind = children.index(selection[0])

        self.relevant_index += 1
        self.update_menu(self.relevant_index)

        target_ind = max(cur_item_ind - 1, 0)
        self.target_dropdown_index(target_ind)

    def try_shift_results_down(self):
        if self.relevant_index == 0:
            return
        children = self.result_dropdown.get_children('')
        selection = self.result_dropdown.selection()
        if len(children) == 0 or len(selection) == 0:
            return
        cur_item_ind = children.index(selection[0])

        self.relevant_index -= 1
        self.update_menu(self.relevant_index)

        target_ind = min(self.entry_max, cur_item_ind + 1)
        self.target_dropdown_index(target_ind)

    def target_dropdown_index(self, target_ind):
        if target_ind < 0:
            target_ind = 0
        children = self.result_dropdown.get_children('')
        if len(children) == 0:
            return
        if target_ind >= len(children):
            target_ind = len(children) - 1

        target_iid = children[target_ind]
        self.result_dropdown.selection_set([target_iid])
        self.result_dropdown.focus(target_iid)

    def on_mouse_wheel(self, e):
        if e.num == 4 or e.delta > 0:
            self.try_shift_results_up()
        elif e.num == 5 or e.delta < 0:
            self.try_shift_results_down()

    def on_hover(self, e):
        row = self.result_dropdown.identify_row(e.y)
        if row == '':
            return
        target_ind = self.result_dropdown.get_children().index(row)
        self.target_dropdown_index(target_ind)

    def select_inst_by_enter(self, e):
        if len(self.result_dropdown.get_children('')) == 0 or len(self.result_dropdown.selection()) == 0:
            return
        self.select_inst(name=None, new_ID=self.result_dropdown.row_data[self.result_dropdown.selection()[0]])

    def select_inst_by_click(self, e):
        if len(self.result_dropdown.get_children('')) == 0:
            return
        row = self.result_dropdown.identify_row(e.y)
        if row == '':
            return
        self.select_inst(name=None, new_ID=self.result_dropdown.row_data[row])

    def destroy_dropdown(self, event=None):
        if event is not None and event.widget is not self:
            return
        if self._root_configure_binding is not None:
            self.dropdown_root.unbind('<Configure>', self._root_configure_binding)
            self._root_configure_binding = None
        if self.dropdown_popup.winfo_exists():
            self.dropdown_popup.destroy()

    def close(self):
        self.destroy_dropdown()
        self.callbacks['destroy_widget']()

    def cancel(self):
        self.destroy_dropdown()
        self.callbacks['cancel_add_inst'](self.inst_trace[2])
