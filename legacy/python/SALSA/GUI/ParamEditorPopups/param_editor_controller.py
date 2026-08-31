import copy
from typing import Union, Dict

from SALSA.BaseInstructions.bi_container import BaseParam
from SALSA.Common.constants import sep, override_str
from SALSA.Common.are_same_checker import are_same
from SALSA.Project.project_container import SCTParameter, SCTLink
from SALSA.GUI.ParamEditorPopups.param_editor_popup import ParamEditPopup, SCPTEditWidget, IntEditWidget, \
    FooterEditWidget, ObjectSelectionWidget, VarSelectionWidget, StringSelectionWidget
from SALSA.Scripts.scpt_param_codes import SCPTParamCodes


class ParamEditController:
    log_code = 'ParamEditCtrlr'

    def __init__(self, parent, callbacks, theme):
        self.column_id = None
        self.param_id = None
        self.parent = parent
        self.callbacks = callbacks
        self.view: Union[None, ParamEditPopup] = None
        self.theme = theme
        self.end_callback = None
        self.end_kwargs = None

        self.setup_view_fxns = {
            'int': self.setup_int_view,
            'scpt': self.setup_scpt_view
        }
        self.param: Union[None, SCTParameter] = None
        self.base_param: Union[None, BaseParam] = None
        self.scpt_codes = SCPTParamCodes()

        self.scpt_fields: Dict[str, SCPTEditWidget] = {}
        self.scpt_callbacks = {'add_child_scpt_rows': self.add_scpt_rows,
                               'regrid_rows': self.regrid_scpt_rows,
                               'has_changes': self.scpt_has_changed,
                               'save': self.scpt_save, 'close': self.close_view,
                               'clear_value': lambda: self.clear_value('scpt')}

        self.int_callbacks = {'save': self.int_save, 'close': self.close_view,
                              'clear_value': lambda: self.clear_value('int')}

        self.int_field: Union[None, IntEditWidget, ObjectSelectionWidget, FooterEditWidget] = None

        self.cur_trace: Union[None, Dict[str, str]] = None

    # ------------------------------------- #
    # Param Editor Show and Close functions #
    # ------------------------------------- #

    def show_param_editor(self, param: Union[SCTParameter, None], base_param: BaseParam, param_id=None, column_id=None,
                          end_callback=None, end_kwargs=None, cur_trace=None):
        self.end_callback = end_callback
        self.end_kwargs = end_kwargs
        self.cur_trace = cur_trace
        if self.view is not None:
            return
        self.view = ParamEditPopup(self.parent, theme=self.theme)
        self.view.main_frame_label.config(text=base_param.type)
        self.param = param
        self.param_id = param_id
        if self.param_id is None:
            self.param_id == ''
        if self.param is not None:
            self.scpt_callbacks |= {'get_var_alias': self.callbacks['get_var_alias']}
        else:
            self.column_id = column_id
        self.base_param = base_param
        base_type = base_param.type.split(sep)[0]
        self.setup_view_fxns[base_type]()

    def close_view(self):
        self.view.destroy()
        self.view = None
        self.param = None
        self.base_param = None
        self.param_id = None
        self.column_id = None
        self.scpt_fields = {}
        if self.end_callback is not None:
            self.end_callback(**self.end_kwargs)

    def clear_value(self, param_base_type):
        if param_base_type == 'scpt':
            self.clear_scpt()
        elif param_base_type == 'int':
            self.clear_int()
        else:
            print(f'{self.log_code}: Unrecognized reset value option: {param_base_type}')

    # ------------------------------------------- #
    # SCPT parameter editor initial setup methods #
    # ------------------------------------------- #

    # Initial SCPT Parameter View Setup
    def setup_scpt_view(self, cleared=False):
        self.view.set_callbacks(self.scpt_callbacks)
        self.scpt_fields['0'] = SCPTEditWidget(parent=self.view.main_frame, callbacks=self.scpt_callbacks, key='0',
                                               is_base=self.param is None, is_delay=self.param_id == 'delay')
        self.scpt_fields['0'].grid(row=0, column=0, sticky='W')
        if cleared:
            self.scpt_fields['0'].set_widget_values(None)
            return
        value = self.param.value if self.param is not None else self.base_param.default_value

        kwargs = {'value': value, 'cur_id': '0'}
        kwargs |= {'is_base': False} if self.param is not None else {'is_base': True}
        if self.param is not None:
            kwargs |= {'override': True} if self.param.override is not None else {}

        self.setup_scpt_from_value(**kwargs)

    # Load scpt value into scpt widgets
    def setup_scpt_from_value(self, value, cur_id, is_base, override=False):
        if isinstance(value, dict):
            for key, parts in value.items():
                self.scpt_fields[cur_id].set_widget_values(key)
                new_keys = self.add_scpt_rows(cur_id, is_base=is_base)
                for i, value in enumerate(parts.values()):
                    self.setup_scpt_from_value(value=value, cur_id=new_keys[i], is_base=is_base, override=override)
        else:
            self.scpt_fields[cur_id].set_widget_values(value=value, override=override)

    # Clear SCPT value
    def clear_scpt(self):
        for field in self.scpt_fields.values():
            field.destroy()
        self.scpt_fields = {}
        self.setup_scpt_view(cleared=True)

    # ------------------------------------------- #
    # SCPT parameter editor functionality methods #
    # ------------------------------------------- #

    def add_scpt_rows(self, p_key, is_base):
        if p_key not in self.scpt_fields:
            return

        new_keys = [f'{p_key}{sep}{j}' for j in range(0, 2)]
        for i, key in enumerate(new_keys):
            if key not in self.scpt_fields:
                self.scpt_fields[key] = SCPTEditWidget(self.view.main_frame, self.scpt_callbacks, key, prefix=i + 1,
                                                       is_base=is_base)

        return new_keys

    def regrid_scpt_rows(self):
        for field in self.scpt_fields.values():
            field.grid_remove()

        field_keys = ['0']
        i = 0
        while len(field_keys) > 0:
            key = field_keys.pop(0)
            self.scpt_fields[key].grid(row=i, column=0, sticky='W')
            if not self.scpt_fields[key].suppress_children and len(self.scpt_fields[key].child_fields) > 0:
                field_keys = sorted([*self.scpt_fields[key].child_fields, *field_keys])

            i += 1


    # ---------------------------------------- #
    # SCPT Parameter Editor Validation methods #
    # ---------------------------------------- #

    def scpt_save(self):
        new_scpt_value = self.convert_scpt_widgets_to_value()
        if not self.scpt_has_changed(new_scpt_value):
            return

        # If editing a base parameter
        if self.param is None:
            self.callbacks['set_change'](self.param_id, self.column_id, new_scpt_value)
            return

        if self.param_id == 'delay':
            self.callbacks['ensure_delay_param'](self.cur_trace['script'], self.cur_trace['section'], self.cur_trace['instruction'],self.param)

        var_changes = self.get_var_changes(self.param.value, new_scpt_value)
        if var_changes is not None:
            self.callbacks['update_variables'](var_changes)
        if new_scpt_value == override_str:
            param_type = self.base_param.type.split(sep)[1]
            value, override_value = self.scpt_codes.overrides[param_type]
            self.param.set_value(value=value, override_value=override_value)
        else:
            self.param.set_value(new_scpt_value)
        self.callbacks['refresh_inst']()
        self.callbacks['set_change']()

    def scpt_has_changed(self, new_scpt_value=None):
        if new_scpt_value is None:
            new_scpt_value = self.convert_scpt_widgets_to_value()
        old_scpt_value = self.param.value if self.param is not None else self.base_param.default_value
        return not are_same(old_scpt_value, new_scpt_value)

    def convert_scpt_widgets_to_value(self, cur_id='0'):
        scpt_widget = self.scpt_fields[cur_id]
        scpt_class = scpt_widget.class_selection.get()
        if scpt_class in ('compare', 'arithmetic'):
            value = {scpt_widget.option_selection.get(): {
                '1': self.convert_scpt_widgets_to_value(cur_id=f'{cur_id}{sep}0'),
                '2': self.convert_scpt_widgets_to_value(cur_id=f'{cur_id}{sep}1')
            }}
        elif scpt_class == 'secondary':
            value = scpt_widget.option_selection.get()
        elif scpt_class == 'input':
            value = scpt_widget.get_input()
            if isinstance(value, float) and 'float' not in self.base_param.type and cur_id == '0':
                value = int(value)
        else:
            value = None
        return value

    def get_var_changes(self, old_param, new_param):
        old_var_list = self.get_vars(old_param)
        new_var_list = self.get_vars(new_param)
        add_list = []
        for var in new_var_list:
            if var not in old_var_list:
                add_list.append(var)
        remove_list = []
        for var in old_var_list:
            if var not in new_var_list:
                remove_list.append(var)
        changes = {'add': add_list if len(add_list) > 0 else [], 'remove': remove_list if len(remove_list) > 0 else []}
        return None if (len(add_list) == 0 and len(remove_list) == 0) else changes

    def get_vars(self, param_value, cur_list=None):
        if cur_list is None:
            cur_list = []
        if isinstance(param_value, dict):
            for value in param_value.values():
                cur_list = self.get_vars(value, cur_list=cur_list)
        elif isinstance(param_value, str):
            if 'Var: ' not in param_value:
                return cur_list
            cur_list.append(param_value)
        return cur_list

    # -------------------------- #
    # Int parameter type methods #
    # -------------------------- #

    def setup_int_view(self):
        self.view.set_callbacks(self.int_callbacks)

        value = None
        if self.param is None:
            self.int_field = IntEditWidget(self.view.main_frame, name=self.base_param.name)
            value = self.base_param.default_value
        elif 'subscript' in self.base_param.type:
            self.int_field = ObjectSelectionWidget(self.view.main_frame, name=self.base_param.name,
                                                   options=self.callbacks['get_subscript_list'](
                                                       self.cur_trace['script'], self.cur_trace['section'], True))
            if self.param.link is not None:
                if self.param.link.target_trace[0] is not None:
                    self.int_field.set_group(self.param.link.target_trace[0])
                if self.param.link.target_trace[1] is not None:
                    value = self.callbacks['get_instruction_identifier'](self.param.link)
        elif 'jump' in self.base_param.type:
            self.int_field = ObjectSelectionWidget(self.view.main_frame, name=self.base_param.name,
                                                   options=self.callbacks['get_instruction_list'](
                                                       self.cur_trace['script'], self.cur_trace['section'],
                                                       self.cur_trace['instruction']))
            if self.param.link is not None:
                if self.param.link.target_trace[0] is not None:
                    self.int_field.set_group(self.param.link.target_trace[0])
                value = self.callbacks['get_instruction_identifier'](self.param.link)
        elif 'string' in self.base_param.type:
            is_footer = 'footer' in self.base_param.type
            self.int_field = StringSelectionWidget(self.view.main_frame, name=self.base_param.name,
                                                   options=self.callbacks['get_string_list'](self.cur_trace[
                                                                                                    'script'
                                                                                                ], is_footer))
            if self.param.linked_string is not None:
                group = self.callbacks['get_string_group'](self.cur_trace['script'], self.param.linked_string)
                value = (group, self.param.linked_string)
        elif 'footer' in self.base_param.type:
            self.int_field = FooterEditWidget(self.view.main_frame, name=self.base_param.name)
            if self.param.linked_string is not None:
                value = self.param.linked_string
        elif 'var' in self.base_param.type:
            var_type = f'{self.base_param.type.split(sep)[-1].capitalize()}Var: '
            self.int_field = VarSelectionWidget(self.view.main_frame, name=f'{self.base_param.name} - {var_type}',
                                                b_min=0, var_type=var_type, signed=False,
                                                callback=self.callbacks['get_var_alias'])
            value = self.param.value
        else:
            self.int_field = IntEditWidget(self.view.main_frame, name=self.base_param.name)
            if self.param.value is not None:
                value = self.param.value
        self.int_field.grid(row=0, column=0)
        self.int_field.set_value(value)

    def int_save(self):
        value = self.int_field.get_value()

        cur_value = self.param.value if self.param is not None else self.base_param.default_value

        if ('jump' in self.base_param.type or 'subscript' in self.base_param.type) and self.param is not None and self.param.link is not None:
            cur_value = self.param.link.target_trace

        if 'footer' in self.base_param.type or 'string' in self.base_param.type:
            cur_value = self.param.linked_string

        if value == 'None':
            value = None

        if value is None or cur_value is None:
            if value is None and cur_value is None:
                return

        elif value == cur_value:
            return

        if self.param is None:
            self.callbacks['set_change'](self.param_id, self.column_id, value)
            return

        if 'subscript' in self.base_param.type:
            self.param.link = SCTLink('Jump', self.cur_trace['script'], -1, [self.cur_trace['section'], self.cur_trace['instruction'], '0'], -1)
            self.param.link.target_trace = value
        elif 'jump' in self.base_param.type:
            self.callbacks['adjust_inst_grouping'](self.cur_trace['script'], self.cur_trace['section'],
                                                   self.cur_trace['instruction'], value[1])
            self.param.link = SCTLink('Jump', self.cur_trace['script'], -1, [self.cur_trace['section'], self.cur_trace['instruction'], '0'], -1)
            self.param.link.target_trace = value
        elif 'footer' in self.base_param.type or 'string' in self.base_param.type:
            self.param.linked_string = value
        else:
            self.param.value = value
        if 'jump' in self.base_param.type or 'subscript' in self.base_param.type:
            self.callbacks['refresh_links'](self.cur_trace['script'], self.cur_trace['section'], self.cur_trace['instruction'])

        self.callbacks['refresh_inst']()
        self.callbacks['set_change']()

    def clear_int(self):
        self.int_field.remove_value()

    def change_theme(self, theme):
        if self.view is not None:
            self.view.change_theme(theme)
