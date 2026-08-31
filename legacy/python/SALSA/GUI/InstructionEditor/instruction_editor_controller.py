import copy
import tkinter as tk
from tkinter import messagebox as msg

from SALSA.GUI.ParamEditorPopups.param_editor_controller import ParamEditController
from SALSA.GUI.InstructionEditor.instruction_editor_view import InstructionEditorView
from SALSA.BaseInstructions.bi_facade import BaseInstLibFacade
from SALSA.Common.constants import sep, LOCK


class InstructionEditorController:

    log_name = 'InstEditCtrlr'

    def __init__(self, parent, name, inst_lib_facade: BaseInstLibFacade, callbacks, theme):
        self.name = name
        self.inst_lib = inst_lib_facade

        view_callbacks = {
            'on_select_instruction': self.on_select_instruction,
            'save': self.on_save_changes,
            'set_change': self.add_change,
            'on_close': self.on_close,
            'check_locked': self.check_item_is_locked,
            'user_type': self.get_user_type,
            'show_param_editor': self.show_param_editor
        }
        self.view = InstructionEditorView(parent=parent, callbacks=view_callbacks, theme=theme)
        self.callbacks = callbacks

        self.cur_inst = None
        self.param_details = None
        self.changed_values = {}

        self.param_editor = ParamEditController(self.view, callbacks={'set_change': self.add_param_editor_change},
                                                theme=theme)

        self.populate_instruction_tree()

    def populate_instruction_tree(self):
        headers = self.view.get_headers('instruction')
        entries = self.inst_lib.get_tree_entries(headers)
        self.populate_tree(self.view.inst_list_tree, headers, entries)

    def populate_tree(self, tree, headers, entries):
        for item in tree.get_children():
            tree.delete(item)

        for entry in entries:
            first = True
            values = []
            text = ''
            for header in headers:
                if first:
                    text = entry[header]
                    first = False
                    continue
                values.append(entry[header])
            kwargs = {'text': text, 'values': values}
            tree.insert(parent='', index='end', **kwargs)

    def on_select_instruction(self, newID):
        newID = int(newID)
        self.cur_inst = newID
        details = copy.deepcopy(self.inst_lib.get_inst(newID))
        self.replace_changed_values(details)
        self.view.id_label.config(text=f'{LOCK} {details.instruction_id}')
        skip_type = 'Can skip frame advance after instruction'
        if details.no_new_frame:
            skip_type = 'Frame never advances after instruction'
        elif details.forced_new_frame:
            skip_type = 'Frame always advances after instruction'
        self.view.skip_label.config(text=f'{LOCK} {skip_type}')
        self.view.param2_label.config(text=f'{LOCK} {details.param2}')
        self.view.location_label.config(text=f'{LOCK} {details.location}')
        self.view.link_label.config(text=f'{LOCK} {details.link_type if details.link_type is not None else "None"}')

        loop_text = "None"
        if details.loop is not None:
            loop_items = [str(_) for _ in details.loop]
            loop_text = ', '.join(loop_items)
        self.view.loop_params_label.config(text=f'{LOCK} {loop_text}')
        if details.warning is None:
            self.view.no_warning.tkraise()
        else:
            self.view.warning_label.config(text=f'{LOCK} {details.warning if details.warning is not None else "None"}')
            self.view.warning_frame.tkraise()

        if self.inst_lib.check_locked([self.cur_inst, 'name']):
            self.view.details_name_label.config(text=f'{LOCK} {details.name}')
            self.view.details_name_label.tkraise()
        else:
            self.view.details_name_entry.delete(0, tk.END)
            self.view.details_name_entry.insert(0, f'{details.name}')
            self.view.details_name_entry.tkraise()

        self.view.details_desc_text.delete(1.0, tk.END)
        self.view.details_desc_text.insert(tk.INSERT, details.description)

        user_type = self.get_user_type()

        if user_type == 'default':
            self.view.default_notes_text.delete(1.0, tk.END)
            self.view.default_notes_text.insert(tk.INSERT, f'{details.default_notes}')
        elif user_type == 'user':
            self.view.default_notes_msg.config(text=f'{details.default_notes}')
            self.view.user_notes_text.delete(1.0, tk.END)
            self.view.user_notes_text.insert(tk.INSERT, f'{details.user_notes}')
        else:
            raise KeyError(f'{self.log_name}: Unknown user type: {user_type}')

        self.refresh_param_tree(ID=newID)

    def refresh_param_tree(self, ID=None):
        if ID is None:
            ID = self.cur_inst
        headers = self.view.get_headers('parameter')
        entries = self.inst_lib.get_tree_entries(headers=self.view.get_headers('parameter'), inst_id=ID)
        self.populate_tree(self.view.param_list_tree, headers, entries)

    def replace_changed_values(self, details):
        if self.cur_inst not in self.changed_values:
            return
        for field, value in self.changed_values[self.cur_inst].items():
            if sep in field:
                field_parts = field.split(sep)
                param_id = int(field_parts[0])
                setattr(details.params[param_id], field_parts[1], value)
            else:
                setattr(details, field, value)

    def check_item_is_locked(self, item_code: list):
        return self.inst_lib.check_locked([self.cur_inst, *item_code])

    def add_change(self, key, value):
        if self.cur_inst not in self.changed_values:
            self.changed_values[self.cur_inst] = {}
        self.changed_values[self.cur_inst][key] = value
        self.prune_changes()
        if len(self.changed_values) > 0:
            self.view.save['state'] = 'normal'
        else:
            self.view.save['state'] = 'disabled'

    def has_changes_remaining(self):
        return len(self.changed_values) > 0

    def on_close(self):
        should_close = True
        if self.has_changes_remaining():
            should_close = self.ask_save_changes()
        if should_close:
            self.callbacks['close'](self.name, self.view)

    def ask_save_changes(self):
        response = msg.askyesnocancel("Instructions have Unsaved Changes", "Some instructions have unsaved changes.\n\nWould you like to save these changes?", parent=self.view)
        if response is None:
            return False
        elif response:
            self.on_save_changes()
        return True

    def on_save_changes(self):
        if len(self.changed_values) == 0:
            return
        for inst_id, changes in self.changed_values.items():
            for field, value in changes.items():
                field_parts = field.split(sep)
                kwargs = {'inst_id': inst_id, 'field': field_parts[0] if len(field_parts) == 1 else field_parts[1],
                          'param_id': field_parts[0] if len(field_parts) > 1 else None}
                self.inst_lib.set_single_inst_detail(value=value, **kwargs)
        self.inst_lib.save_user_insts()
        self.changed_values = {}

    def prune_changes(self):
        actual_changes = {}
        for inst_id, changes in self.changed_values.items():
            base_details = self.inst_lib.get_inst(inst_id)
            for field, value in changes.items():
                add = False
                if sep in field:
                    field_parts = field.split(sep)
                    param_id = int(field_parts[0])
                    if value != getattr(base_details.params[param_id], field_parts[1], None):
                        add = True
                else:
                    if value != getattr(base_details, field, None):
                        add = True
                if add:
                    if inst_id not in actual_changes:
                        actual_changes[inst_id] = {}
                    actual_changes[inst_id][field] = value
        self.changed_values = actual_changes

    def get_user_type(self):
        return self.inst_lib.get_user_type()

    def show_param_editor(self, param_id, column_id):
        self.param_editor.show_param_editor(None, self.inst_lib.get_inst(self.cur_inst).params[int(param_id)], param_id, column_id)

    def add_param_editor_change(self, param_id, column_id, value):
        self.view.set_value(param_id, column_id, value)
        column_name = self.view.get_headers('parameter')[column_id]
        self.add_change(key=f'{param_id}{sep}{column_name}', value=value)

    def change_theme(self, theme):
        self.view.change_theme(theme)
        self.param_editor.change_theme(theme)
