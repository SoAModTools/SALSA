import os.path
import queue
import tkinter as tk
from tkinter import ttk
from typing import Union, TypedDict, Literal, Optional
import webbrowser

from GUI.LinkViewPopup.link_view_popup import LinkViewPopup
from SALSA.GUI.ProjectSearch.project_search_popup import ProjectSearchPopup
from SALSA.GUI.dolphin_link_popup import DolphinLinkPopup
from SALSA.GUI.EncodeErrorPopup.project_error_popup import ProjectErrorPopup
from SALSA.GUI.ProjectEditor.project_editor_controller import ProjectEditorController
from SALSA.GUI.Widgets.data_treeview import DataTreeview
from SALSA.GUI.themes import light_theme, dark_theme
from SALSA.GUI.Widgets import widgets as w
from SALSA.GUI.AnalysisPopup.analysis_controller import AnalysisController
from SALSA.GUI.InstructionEditor.instruction_editor_controller import InstructionEditorController
from SALSA.GUI.other_popups import AboutView
from SALSA.BaseInstructions.bi_facade import BaseInstLibFacade
from SALSA.GUI.AnalysisPopup.analysis_view import AnalysisView
from SALSA.GUI.InstructionEditor.instruction_editor_view import InstructionEditorView
from SALSA.GUI.ProjectEditor.project_editor_view import ProjectEditorView
from SALSA.GUI.SCTExport.sct_export_popup import SCTExportPopup
from SALSA.GUI.VariableEditor.variable_editor_popup import VariablePopup
from SALSA.GUI.StringEditor.string_editor_popup import StringPopup
from SALSA.Project.project_facade import SCTProjectFacade
from SALSA.Common.containers import Vector2


class PopupTypes(TypedDict):
    inst: Optional[InstructionEditorController]
    analysis: Optional[AnalysisController]
    about: Optional[AboutView]
    variable: Optional[VariablePopup]
    string: Optional[StringPopup]
    export: Optional[SCTExportPopup]
    errors: Optional[ProjectErrorPopup]
    d_link: Optional[DolphinLinkPopup]
    search: Optional[ProjectSearchPopup]
    links: Optional[LinkViewPopup]


class GUIController:
    parent: tk.Tk
    prj_view: ProjectEditorView

    def __init__(self, parent, project_editor_controller: ProjectEditorController, project_facade: SCTProjectFacade,
                 inst_lib_facade: BaseInstLibFacade, theme):

        self.status_msg: Union[None, tk.Label] = None
        self.status_sub_msg: Union[None, tk.Label] = None
        self.status_queue: queue.SimpleQueue = queue.SimpleQueue()
        self.help_path = os.path.abspath('./SALSA/Help/Skies of Arcadia Legends Script Assistant.html')
        self.parent = parent
        self.prj_cont = project_editor_controller
        self.prj_view = self.prj_cont.view
        self.project = project_facade
        self.base_lib = inst_lib_facade
        self.theme = theme

        self.popups: PopupTypes = {'inst': None, 'analysis': None, 'about': None, 'errors': None,
                                   'variable': None, 'string': None, 'export': None, 'd_link': None,
                                   'search': None, 'links': None}

        self.callbacks = {}

        self.status_popup: Union[None, tk.Toplevel] = None

    # ------------------------------------------------------------- #
    # Functions to inactivate script view when no project is loaded #
    # ------------------------------------------------------------- #

    def enable_project_view(self):
        self._recursive_toggle(self.prj_view, 'normal')
        self.callbacks['script']()

    def disable_project_view(self):
        self._recursive_toggle(self.prj_view, 'disabled')
        self.callbacks['no_script']()

    def toggle_frame_state(self, frame, state: Literal['normal', 'disabled']):
        self._recursive_toggle(frame, state)

    def _recursive_toggle(self, parent, state):
        for child in parent.winfo_children():
            r = child.__repr__().split('.')[-1]

            # capture the children of the scroll frame
            if isinstance(child, w.ScrollLabelFrame) or isinstance(child, w.ScrollFrame):
                self._recursive_toggle(child.scroll_frame, state)

            child_type = type(child)
            # ttk containers do both
            if child_type in (ttk.PanedWindow, ttk.Frame, ttk.LabelFrame):
                ttk_state = state
                ttk_state = ttk_state if ttk_state != 'normal' else '!disabled'
                child.state([ttk_state])
                self._recursive_toggle(child, state)

            # tk containers and tk fields with no state
            elif child_type in (
                    tk.Frame, tk.LabelFrame, tk.Message, tk.Menu, tk.Menubutton, w.ScrollFrame, w.ScrollLabelFrame,
                    tk.Toplevel):
                self._recursive_toggle(child, state)

            # ttk widgets
            elif type(child) in (
                    ttk.Button, ttk.Checkbutton, DataTreeview, ttk.Treeview, ttk.Scrollbar, ttk.Label, ttk.Separator):
                ttk_state = state
                ttk_state = ttk_state if ttk_state != 'normal' else '!disabled'
                child.state([ttk_state])
            # tk widgets
            else:
                child.configure(state=state)

    # ------------------------ #
    # Functions to show popups #
    # ------------------------ #

    def show_instruction_view(self):
        if self.popups['inst'] is not None:
            self.popups['inst'].view.tkraise()
            return

        callbacks = {
            'close': self.close_popup
        }
        self.popups['inst'] = InstructionEditorController(parent=self.parent, callbacks=callbacks,
                                                          inst_lib_facade=self.base_lib, name='inst',
                                                          theme=self.theme)

    def show_about(self):
        position = Vector2(x=self.parent.winfo_x(), y=self.parent.winfo_y())
        self.popups['about'] = AboutView(parent=self.parent, position=position, callback=self.close_popup)

    def show_help(self):
        webbrowser.open(url='file://' + self.help_path)

    def show_settings(self):
        pass

    def show_project_errors(self):
        if self.popups['errors'] is not None:
            self.popups['errors'].tkraise()
            return
        callbacks = {
            'get_errors': self.project.get_project_encode_errors,
            'goto_error': self.prj_cont.goto_link_target,
            'close': self.close_popup
        }
        self.popups['errors'] = ProjectErrorPopup(self.parent, callbacks=callbacks, name='errors',
                                                  theme=self.theme)

    def show_variables_popup(self):
        if self.popups['variable'] is not None:
            self.popups['variable'].tkraise()
            return
        callbacks = {
            'get_scripts': lambda: self.project.get_tree(self.prj_view.get_headers('script')),
            'get_variables': self.project.get_script_variables_with_aliases,
            'set_alias': self.project.set_variable_alias,
            'remove_global': self.project.remove_global,
            'get_var_usage': self.project.get_variable_usages,
            'goto_link': self.prj_cont.goto_link_target,
            'refresh_prj_trees': self.prj_cont.refresh_all_trees,
            'set_change_flag': self.prj_cont.set_has_changes,
            'close': self.close_popup
        }
        self.popups['variable'] = VariablePopup(self.parent, callbacks=callbacks, name='variable',
                                                theme=self.theme)

    def show_strings_popup(self):
        if self.popups['string'] is not None:
            self.popups['string'].tkraise()
            return
        callbacks = {
            'save': self.project.edit_string, 'close': self.close_popup,
            'get_scripts': lambda: self.project.get_tree(self.prj_view.get_headers('script')),
            'get_string_tree': self.project.get_string_tree, 'get_string_to_edit': self.project.get_string_to_edit,
            'add_string_group': self.project.add_string_group, 'delete_string_group': self.project.remove_string_group,
            'rename_string_group': self.project.rename_string_group, 'add_string': self.project.add_string,
            'delete_string': self.project.delete_string, 'rename_string': self.project.change_string_id,
            'is_sect_name_used': self.project.is_sect_name_used, 'find_usage': self.send_string_to_search,
            'refresh_sections': lambda: self.prj_cont.refresh_tree('section'),
        }
        self.popups['string'] = StringPopup(self.parent, callbacks=callbacks, name='string',
                                            theme=self.theme)

    def show_sct_export_popup(self, selected=None):
        if self.popups['export'] is not None:
            self.popups['export'].tkraise()
            return
        callbacks = {
            'export': self.callbacks['export_sct'], 'close': self.close_popup,
            'get_tree': lambda: self.project.get_tree(self.prj_view.get_headers('script')),
        }
        self.popups['export'] = SCTExportPopup(self.parent, callbacks=callbacks, name='export', selected=selected,
                                               theme=self.theme)

    def show_sct_debugger_popup(self, callbacks):
        if self.popups['d_link'] is not None:
            self.popups['d_link'].tkraise()
            return
        callbacks |= {'close': self.close_popup}
        self.popups['d_link'] = DolphinLinkPopup(self.parent, callbacks=callbacks, name='d_link', theme=self.theme)

    def show_project_search_popup(self):
        if self.popups['search'] is not None:
            self.popups['search'].tkraise()
            return
        callbacks = {
            'search': self.project.search,
            'goto_result': self.prj_cont.goto_link_target,
            'goto_dialog': self.goto_string_helper,
            'get_filters': self.project.get_search_filter_trees,
            'close': self.close_popup
        }
        self.popups['search'] = ProjectSearchPopup(self.parent, callbacks=callbacks, name='search',
                                                   theme=self.theme)

    def send_string_to_search(self, string, **options):
        self.show_project_search_popup()
        self.popups['search'].search_entry.delete(0, tk.END)
        self.popups['search'].search_entry.insert(0, string)
        # self.popups['search'].set_options(options)
        self.popups['search'].start_search()

    def show_links_popup(self, link_finder):
        if self.popups['links'] is not None:
            self.popups['links'].update_link_widgets(new_registry=link_finder)
            self.popups['links'].tkraise()
            return
        callbacks = {
            'goto': self.prj_cont.goto_link_target,
            'close': self.close_popup
        }
        self.popups['links'] = LinkViewPopup(self.parent, 'links', callbacks, link_finder, self.theme)

    # ------------- #
    # Popup refresh #
    # ------------- #

    def refresh_popup(self,
                      name: Literal['inst', 'analysis', 'about', 'variable', 'string', 'export', 'errors', 'd_link']):
        if self.popups[name] is None:
            return
        self.popups[name].refresh_popup()

    # ----------------------- #
    # Popup cleanup functions #
    # ----------------------- #

    def close_popup(self,
                    name: Literal['inst', 'analysis', 'about', 'variable', 'string', 'export', 'errors', 'd_link'],
                    popup: Union[InstructionEditorView, AnalysisView, AboutView, SCTExportPopup,
                    VariablePopup, StringPopup, ProjectErrorPopup, DolphinLinkPopup, ProjectSearchPopup, LinkViewPopup]):
        popup.destroy()
        self.popups[name] = None

    def add_callbacks(self, callbacks):
        self.callbacks = {**self.callbacks, **callbacks}

    # ---------------------- #
    # Status popup functions #
    # ---------------------- #

    def show_status_popup(self, title, msg, sub_msg=''):
        if self.status_popup is not None:
            self.stop_status_popup()
        height = 50
        width = 300
        theme = dark_theme if self.callbacks['is_darkmode']() else light_theme
        bg = theme['TLabelframe']['configure']['highlightbackground']
        fg = theme['.']['configure']['foreground']
        self.status_popup = tk.Toplevel(self.prj_view, background=bg)
        self.status_popup.title = title
        self.status_popup.overrideredirect(True)
        self.status_popup.wm_focusmodel('active')
        self.status_popup.columnconfigure(0, weight=1)
        self.status_popup.rowconfigure(0, weight=1)

        self.status_msg = tk.Label(self.status_popup, text=msg, anchor=tk.CENTER, background=bg, foreground=fg)
        self.status_msg.grid(row=0, column=0)

        self.status_sub_msg = tk.Label(self.status_popup, text=sub_msg, anchor=tk.CENTER, background=bg, foreground=fg)
        self.status_sub_msg.grid(row=1, column=0)

        cur_geom = (self.prj_view.winfo_width(), self.prj_view.winfo_height(), self.prj_view.winfo_rootx(),
                    self.prj_view.winfo_rooty())
        x = cur_geom[2] + cur_geom[0] // 2 - width // 2
        y = cur_geom[3] + cur_geom[1] // 2 - height // 2
        new_geom = f'{width}x{height}+{x}+{y}'
        self.status_popup.geometry(new_geom)

        self.status_listener()

    def stop_status_popup(self):
        self.status_popup.destroy()
        self.status_popup = None
        self.status_msg = None
        self.status_sub_msg = None
        while not self.status_queue.empty():
            self.status_queue.get_nowait()

    def change_status_msg(self, msg=None, sub_msg=None):
        if self.status_popup is None:
            return
        if msg is not None and isinstance(msg, str):
            self.status_msg.configure(text=msg)
        if sub_msg is not None and isinstance(sub_msg, str):
            self.status_sub_msg.configure(text=sub_msg)

    def change_theme(self, theme):
        self.theme = theme
        for popup in self.popups.values():
            if popup is not None:
                popup.change_theme(theme)

    def status_listener(self):
        if not self.status_queue.empty():
            item = self.status_queue.get()
            if isinstance(item, dict):
                self.change_status_msg(**item)
        self.status_popup.after(20, self.status_listener)

    # ------------------ #
    # Goto String Helper #
    # ------------------ #

    def goto_string_helper(self, script, group, string, string_popup_up=False):
        if string_popup_up:
            self.popups['string'].goto_string(script, group, string)
        else:
            self.show_strings_popup()
            self.prj_view.after(10, self.goto_string_helper, script, group, string, True)
