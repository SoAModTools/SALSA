import tkinter as tk
from tkinter import ttk
from typing import List, Tuple, Union, Dict

from SALSA.Common.constants import sep

drag_tree_info = {
    'headers': {
        'id': {'label': 'ID', 'width': 50, 'stretch': False},
        'name': {'label': 'Name', 'width': 50, 'stretch': False}
    },
    'final_item': {'parent': '', 'index': tk.END, 'text': '...', 'values': ['']}
}

default_tree_width = 100
default_tree_minwidth = 10
default_tree_anchor = tk.W
default_tree_stretch = False
default_tree_label = ''


class DragWindow(tk.Toplevel):

    def __init__(self, master, tree_dict, theme, bbox, *args, **kwargs):
        super().__init__(master, *args, **kwargs)

        self.attributes('-alpha', 0.5)
        self.overrideredirect(True)

        self.configure(**theme['drag.Ttoplevel']['configure'], bd=0)

        tree = ttk.Treeview(self, columns=list(tree_dict['headers'].keys())[1:], style='drag.Treeview', show='tree')
        tree.grid(row=0, column=0, sticky='NSEW')
        first = True
        for name, d in tree_dict['headers'].items():
            label = d.get('label', default_tree_label)
            anchor = d.get('anchor', default_tree_anchor)
            minwidth = d.get('minwidth', default_tree_minwidth)
            width = d.get('width', default_tree_width)
            stretch = d.get('stretch', default_tree_stretch)
            if first:
                name = '#0'
                first = False
            tree.heading(name, text=label, anchor=anchor)
            tree.column(name, anchor=anchor, minwidth=minwidth, width=width, stretch=stretch)

        for item in tree_dict['items']:
            tree.insert(**item)
        tree.insert(**drag_tree_info['final_item'])

        tree.configure(height=len(tree_dict['items'])+1)

        self.mouse_box = (bbox[0], bbox[1], bbox[0]+bbox[2], bbox[1]+ bbox[3])

    def update_pos(self, x, y):
        x += 2
        y += 2
        if self.mouse_box[0] < x < self.mouse_box[2] and self.mouse_box[1] < y < self.mouse_box[3]:
            self.geometry(f'+{x}+{y}')


placeholder_text = 'PLACEHOLDER'
group_open_y_percent = 2/3  # proportion of a cell
group_open_delay = 800  # delay to open a group by hovering (milliseconds)
move_ignore_event_delay = 5

default_tree_theme = {

    "drag.Ttoplevel": {
        "configure": {
            "background": 'gray120',
            "highlightbackground": 'gray120',
            "highlightcolor": 'gray120',
            "highlightthickness": 0,
        }
    },
    # Treeview configuration for the next three items
    "drag.Treeview": {
        "configure": {
            "background": 'white',
            "foreground": 'black',
            "fieldbackground": 'white',
            "lightcolor": 'white',
            "darkcolor": 'white'
        },
    },
}


class DataTreeview(ttk.Treeview):

    def __init__(self, parent, name, callbacks=None, can_open=True, can_move=False, can_select_multiple=False,
                 selectmode='browse', prevent_extreme_selection=False, keep_group_ends=False, theme=None,
                 return_none=False, **kwargs):
        super().__init__(parent, selectmode=selectmode, **kwargs)
        self._parent = parent
        self.name = name
        self.callbacks = callbacks if callbacks is not None else {}
        self.row_data: Dict[str, str] = {}
        self.group_types = {}
        self.cur_selection = []
        self.selection_order = []
        self.return_none = return_none
        self.theme = theme if theme is not None else default_tree_theme

        self.can_open = can_open
        self.can_select_multiple = can_select_multiple
        self.can_move = can_move

        self.bind_events()

        self.in_motion = False
        self.has_shift = False
        self.first_selected = None
        self.drag_widget: Union[None, DragWindow] = None
        self.placeholder = None
        self.selection_bounds = None
        self.selected = None
        self.group_waiting = ''
        self.parent_restriction = None
        self.move_ignore_counter = 0
        self.prevent_extreme_selection = prevent_extreme_selection
        self.first_entry = None
        self.last_entry = None
        self.keep_group_ends = keep_group_ends
        self.start_index = None
        self.start_parent = None

    def unbind_events(self):
        self.unbind("<ButtonPress-1>")
        self.unbind("<ButtonRelease-1>")
        self.unbind("<B1-Motion>")
        self.unbind("<Shift-ButtonPress-1>")
        self.unbind("<Shift-ButtonRelease-1>")
        if self.can_open:
            self.after(100, self.bind_send_none)

    def bind_send_none(self):
        self.bind("<ButtonRelease-1>", self.send_none, add='+')

    def bind_events(self):
        if self.can_open:
            self.unbind("<ButtonRelease-1>")
        if self.can_move:
            self.bind("<ButtonPress-1>", self.bDown_move)
            self.bind("<ButtonRelease-1>", self.bUp_move, add='+')
            self.bind("<B1-Motion>", self.bMove, add='+')
        if self.can_select_multiple:
            self.bind("<Shift-ButtonPress-1>", self.bDown_Shift, add='')
            self.bind("<Shift-ButtonRelease-1>", self.bUp_Shift, add='')
        if self.can_open:
            self.bind("<ButtonRelease-1>", self.select_entry, add='+')

    def add_callback(self, key, callback):
        self.callbacks[key] = callback

    def set_theme(self, theme):
        self.theme = theme

    def send_none(self, e):
        return self.callbacks['select'](self.name, None)

    def select_entry(self, event):
        if len(self.get_children('')) == 0:
            return
        if self.in_motion:
            return
        if 'select' not in self.callbacks:
            raise RuntimeError(f'{type(self)}-{self.name}: no "select" callback given')
        row = self.identify_row(event.y)
        if row == '':
            return
        if len(self.selection()) > 1:
            return
        if self.item(row)['text'] == placeholder_text:
            return
        self.focus(row)
        self.selection_set([row])
        row_data = self.row_data[row]
        if row_data is not None:
            self.callbacks['select'](self.name, row_data)
        elif self.return_none:
            self.callbacks['select'](self.name, row_data)

    def insert_entry(self, parent, text, values, index='end', group_type=None, row_data=None, **kwargs):
        iid = str(len(self.row_data))
        self.row_data[iid] = row_data
        self.group_types[iid] = group_type
        super().insert(parent=parent, index=index, iid=iid, text=text, values=values, **kwargs)
        self.last_entry = iid
        self.first_entry = '0'
        return iid

    def get_iid_from_rowdata(self, row_data):
        rows = {v: k for k, v in self.row_data.items() if k is not None}
        return rows[row_data] if row_data in rows else None

    def select_by_rowdata(self, row_data):
        for iid, data in self.row_data.items():
            if row_data == data:
                self.focus(iid)
                self.selection_set(iid)
                break

    def clear_all_entries(self):
        for row in self.get_children():
            self.delete(row)
        self.row_data = {}
        self.first_entry = None
        self.last_entry = None
        self.first_selected = None

    def get_selection(self):
        return [self.row_data[self.get_children()[int(s)]] for s in self.cur_selection]

    def get_open_elements(self, parent='', open_items=None):
        if open_items is None:
            open_items = []
        children = self.get_children(parent)
        cur_ind = 0
        while cur_ind < len(children):
            is_open = self.item(children[cur_ind])['open']
            if not isinstance(is_open, bool):
                is_open = is_open == 1
            if not is_open:
                cur_ind += 1
                continue

            child_uuid = self.row_data.get(children[cur_ind], None)
            if child_uuid is None:
                child_child_uuid = self.row_data.get(self.get_children(children[cur_ind])[-1], None)
                open_items.append(('see', child_child_uuid))
            else:
                if cur_ind + 1 < len(children):
                    next_uuid = self.row_data.get(children[cur_ind + 1], None)
                    if next_uuid == child_uuid:
                        open_items.append(('open_prev', child_uuid, 1))
                    else:
                        open_items.append(('open', child_uuid))
                else:
                    open_items.append(('open', child_uuid))
            self.get_open_elements(parent=children[cur_ind], open_items=open_items)
            cur_ind += 1
        return open_items

    def open_tree_elements(self, open_items):
        rows: Dict[str, str] = {v: k for k, v in self.row_data.items() if k is not None}
        for entry in open_items:
            if entry[1] not in rows:
                continue
            if entry[0] == 'see':
                self.see(rows[entry[1]])
            elif entry[0] == 'open':
                self.item(rows[entry[1]], open=True)
            elif entry[0] == 'open_prev':
                siblings = self.get_children(self.parent(rows[entry[1]]))
                prev_iid = siblings[siblings.index(rows[entry[1]]) - entry[2]]
                self.item(prev_iid, open=True)

    def open_all_groups(self):
        for entry in self.row_data.keys():
            if len(self.get_children(entry)) > 0:
                self.item(entry, open=True)

    def close_all_groups(self):
        for entry in self.row_data.keys():
            if len(self.get_children(entry)) > 0:
                self.item(entry, open=False)

    def bDown_Shift(self, event):
        if len(self.get_children('')) == 0:
            return
        self.has_shift = True
        if self.first_selected is None:
            return self.bDown_move(event)

        clicked_iid = self.identify_row(event.y)
        if clicked_iid in self.first_selected:
            self.after(10, self.selection_set, self.first_selected)
            return

        clicked_row = int(clicked_iid)
        if isinstance(self.first_selected, list):
            first_row = int(self.first_selected[0]) if int(self.first_selected[0]) < clicked_row else int(self.first_selected[1])
        else:
            first_row = int(self.first_selected)

        select = [str(i) for i in range(min(clicked_row, first_row), max(clicked_row, first_row)+1)]

        # Make sure that the first and last entries cannot be selected (label and return)
        if self.prevent_extreme_selection:
            root_children = self.get_children('')
            first = root_children[0]
            if first in select:
                select.remove(first)
            last = root_children[-1]
            if last in select:
                select.remove(last)

        if self.keep_group_ends and len(select) > 1:
            if select[-1] == self.get_children(self.parent(select[-1]))[-1]:
                select.pop(-1)

        self.selected = select
        self.selection_set(select)

    def bUp_Shift(self, event):
        self.has_shift = False

    def bDown_move(self, event):
        if len(self.get_children('')) == 0:
            return
        sel = self.identify_row(event.y)
        if sel == '':
            return
        if sel not in self.selection():
            sel_uuid = self.row_data[sel]
            sel_ind = self.index(sel)
            sel_siblings = self.get_children(self.parent(sel))
            if self.index(sel) > 0 and sel_uuid is not None:
                prev_sel = sel_siblings[sel_ind - 1]
                prev_uuid = self.row_data[prev_sel]
                if sel_uuid == prev_uuid:
                    sel = [prev_sel, sel]
            if not isinstance(sel, list):
                if self.index(sel) + 1 < len(sel_siblings) and sel_uuid is not None:
                    next_sel = sel_siblings[sel_ind + 1]
                    next_uuid = self.row_data[next_sel]
                    if sel_uuid == next_uuid:
                        sel = [sel, next_sel]
            self.first_selected = sel
            self.selected = sel
            self.after(10, self.selection_set, sel)

    def bUp_move(self, event):
        if len(self.get_children('')) == 0:
            return
        if not self.in_motion:
            sel = self.identify_row(event.y)
            if sel == '':
                return
            self.first_selected = sel
            self.selected = sel
            self.after(10, self.selection_set, sel)
            return

        final_index = self.index(self.placeholder)
        final_parent = self.parent(self.placeholder)
        kwargs = {}
        if final_parent == self.start_parent and final_index == self.start_index:
            kwargs |= {'refresh_only': True}
        insert_after_index = 0 if final_index == 0 else final_index - 1
        insert_after_iid = self.get_children(self.parent(self.placeholder))[insert_after_index]

        insert_in_group = False
        if final_index == 0:
            insert_after_iid = self.parent(insert_after_iid)
            insert_in_group = True

        insert_after_data: Union[None, str] = self.row_data.get(insert_after_iid, None)
        if insert_after_data is None:
            case = self.item(insert_after_iid)['values'][0].split(' ')[0]
            insert_after_data = f'{self.row_data[(self.parent(insert_after_iid))]}{sep}{case}'

        # Assorted cleanup
        self.in_motion = False
        self.placeholder = None
        self.drag_widget.destroy()
        self.drag_widget = None
        self.parent_restriction = None
        self.move_ignore_counter = 0

        # Callback to have the items moved and tree refreshed
        kwargs |= {'tree_key': self.name, 'sel_bounds': self.selection_bounds,
                   'insert_after': insert_after_data, 'insert_in_group': insert_in_group}
        self.callbacks['move_items'](**kwargs)
        self.selection_bounds = None

    def bMove(self, event):
        if self.identify_region(event.x, event.y) == 'heading':
            return
        if len(self.get_children('')) == 0:
            return
        if self.has_shift:
            return
        if not self.in_motion:
            if self.move_ignore_counter < move_ignore_event_delay:
                self.move_ignore_counter += 1
                return
            # should prevent moving the first and last entries alone if prevent extreme selection is on
            if self.prevent_extreme_selection and not isinstance(self.selected, list):
                if self.first_selected in (self.first_entry, self.last_entry):
                    return
            if self.keep_group_ends and not isinstance(self.selected, list):
                if self.first_selected == self.get_children(self.parent(self.first_selected))[-1]:
                    return
            self.selection_set(self.selected)
            self.in_motion = True
            self.start_motion()
            if sep in self.selection_bounds[0]:
                self.parent_restriction = self.parent(self.placeholder)

        sel_iid = self.identify_row(event.y)
        # No need to update position bc
        if sel_iid == '':
            return

        if self.parent_restriction is not None:
            if self.parent_restriction != self.parent(sel_iid):
                return

        if sel_iid in (self.first_entry, self.last_entry):
            return

        self.drag_widget.update_pos(event.x_root, event.y_root)
        if sel_iid == self.placeholder:
            return

        if len(self.get_children(sel_iid)) > 0 and self.parent_restriction is None:
            if not self.item(sel_iid)['open'] in (True, 1):
                if self.group_waiting != '':
                    if sel_iid == self.group_waiting:
                        return
                else:
                    bbox = self.bbox(sel_iid)
                    if event.y < bbox[1] + int(bbox[3] * group_open_y_percent):
                        self.group_waiting = sel_iid
                        self.after(15, self.delayed_open_group, sel_iid, 0, group_open_delay)
                        return
        self.group_waiting = ''
        moveto = self.index(sel_iid)
        parent = self.parent(sel_iid)
        if (self.index(sel_iid) + 1 == len(self.get_children(parent))
                and parent != ''
                and self.parent_restriction is None
                and self.keep_group_ends
                and self.index(self.placeholder) < self.index(sel_iid)):
            if self.row_data.get(parent, None) is None:
                parent = self.parent(parent)
            moveto = self.index(parent) + 1
            parent = self.parent(parent)

        self.move(self.placeholder, parent, moveto)

    def start_motion(self):
        selection = list(self.selection())
        base_parent, parent_list = self.motion_get_parent_base_and_list(selection)
        selection = list(set(selection))
        selection = self.motion_select_all_children(base_parent, parent_list, selection)
        selection = self.sort_sel_iids(selection)
        base_select = list(set(selection) & set(self.get_children(base_parent)))
        bounds = (selection[0], selection[-1])
        uuid_bounds = []
        for entry in bounds:
            data = self.row_data.get(entry, None)
            if data is None:
                data = f'{self.row_data[self.parent(entry)]}{sep}{self.item(entry)["values"][0].split(" ")[0]}'
            uuid_bounds.append(data)
        self.selection_bounds = tuple(uuid_bounds)
        self.start_parent = self.parent(bounds[0])
        self.start_index = self.index(bounds[0])
        index = self.index(bounds[0])
        values = [''] * (len(self['columns']) - 1)
        self.placeholder = self.insert(parent=base_parent, index=index, iid='placeholder', text=placeholder_text, values=values)
        self.selection_set(self.placeholder)

        item_iids = selection[:5] if len(selection) > 5 else selection

        items = [{'parent': '', 'index': tk.END, 'text': self.item(i)['text'], 'values': self.item(i)['values'][0]} for i in item_iids]
        tree_dict = {**drag_tree_info, 'items': items}

        bbox = self.winfo_rootx(), self.winfo_rooty(), self.winfo_rootx()+self.winfo_width(), self.winfo_rooty()+self.winfo_height()

        self.drag_widget = DragWindow(self, theme=self.theme, bbox=bbox, tree_dict=tree_dict)

        for s in base_select:
            self.delete(s)

    def motion_get_parent_base_and_list(self, selection) -> Tuple[str, List[str]]:
        base_parent = None
        parent_list = []
        for s in selection:
            cur_parent = self.parent(s)
            if base_parent is None:
                base_parent = cur_parent
            if cur_parent == '':
                base_parent = cur_parent
            if base_parent != '':
                base_parent = str(min(int(base_parent), int(cur_parent)))
            if len(self.get_children(s)) > 0:
                parent_list.append(s)
            if cur_parent not in parent_list:
                parent_list.append(cur_parent)
        return base_parent, parent_list

    def motion_select_all_children(self, base_parent, parent_list, selection):
        i = 0
        while i < len(parent_list):
            p = parent_list[i]
            if p == base_parent:
                i += 1
                continue
            children = self.get_children(p)
            for c in children:
                if c not in selection:
                    selection.append(c)
                    if len(self.get_children(c)) > 0 and c not in parent_list:
                        parent_list.append(c)
            i += 1
        return selection

    @staticmethod
    def sort_sel_iids(selection: List[str]):
        int_sel = list({int(s) for s in selection})
        int_sel.sort()
        return [str(s) for s in int_sel]

    def delayed_open_group(self, group_iid, time_elapsed, time_finished):
        if self.group_waiting != group_iid:
            return
        if time_elapsed > time_finished:
            self.item(group_iid, open=True)
        else:
            self.after(100, self.delayed_open_group, group_iid, time_elapsed + 100, time_finished)

    def print_parent_and_index(self, event):
        iid = self.identify_row(event.y)
        print(f'parent: {self.parent(iid)}, index: {self.index(iid)}')

    def has_data(self):
        return len(self.row_data) > 0
