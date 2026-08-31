import copy
import os
import queue
import random
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk
from tkinter.filedialog import asksaveasfilename
from typing import Union

from SALSA.Analysis.link_finder import LinkFinder
from SALSA.Analysis.var_usage import VarUsage
from SALSA.BaseInstructions.bi_facade import BaseInstLibFacade
from SALSA.Common.setting_class import settings
from SALSA.FileModels.project_model import ProjectModel
from SALSA.FileModels.sct_model import SCTModel
from SALSA.GUI.ProjectEditor.project_editor_controller import ProjectEditorController
from SALSA.GUI.ProjectEditor.project_editor_view import ProjectEditorView
from SALSA.GUI.gui_controller import GUIController
from SALSA.GUI import menus
from SALSA.GUI.themes import themes, theme_non_color_maps
from SALSA.Project.project_facade import SCTProjectFacade
from SALSA.AKLZ.aklz import set_aklz_slow

has_debugger = True
try:
    from SALSA.DolphinLink.dolphin_link_controller import DolphinLink
except ImportError as e:
    print(e)
    has_debugger = False

default_style = 'clam'


default_settings = {
    'darkmode': 'False'
}

icons = [
    ('./SALSA/GUI/Icons/icon-1.png', './SALSA/GUI/Icons/icon-1-16.png'),
    ('./SALSA/GUI/Icons/icon-2.png', './SALSA/GUI/Icons/icon-2-16.png'),
    ('./SALSA/GUI/Icons/icon-3.png', './SALSA/GUI/Icons/icon-3-16.png')
]


class Application(tk.Tk):
    """Controls the links between the data models and views"""
    test = True
    title_text = "Skies of Arcadia Legends Script Assistant"
    log_key = 'app'

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)

        if self.log_key not in settings:
            settings[self.log_key] = {}

        for s in default_settings:
            if settings[self.log_key].get(s, None) is None:
                settings.set_single(self.log_key, s, default_settings[s])

        # Load base instructions
        self.base_insts = BaseInstLibFacade()

        # Setup window parameters
        self.title(self.title_text)
        self.resizable(width=True, height=True)
        self.protocol('WM_DELETE_WINDOW', self.on_quit)

        icon_ind = random.randint(0, 2)
        large_icon = tk.PhotoImage(file=icons[icon_ind][0])
        small_icon = tk.PhotoImage(file=icons[icon_ind][1])
        self.iconphoto(True, large_icon, small_icon)

        self.is_darkmode = True if settings[self.log_key]['darkmode'] == 'True' else False
        self.style = ttk.Style()
        for theme, theme_settings in themes.items():
            self.style.theme_create(theme, parent=default_style, settings=theme_settings)

        for key, maps in theme_non_color_maps.items():
            for mk, mv in maps.items():
                self.style.map(key, **{mk: mv})

        theme_name = list(themes.keys())[0] if self.is_darkmode else list(themes.keys())[1]
        theme = themes[theme_name]
        self.style.theme_use(theme_name)

        # maps dynamic style attributes for the current theme
        self.map_current_theme(themes[theme_name])

        # Setup project details
        self.project = SCTProjectFacade(self.base_insts)
        self.project_filepath = ''

        # Setup script editor view and controller
        self.project_edit_view = ProjectEditorView(self, theme=theme)
        self.project_edit_view.grid(row=0, column=0, sticky='NSEW', pady=5, padx=5)
        self.rowconfigure(0, weight=1)
        self.columnconfigure(0, weight=1)

        project_controller_callbacks = {
            'save_project': self.on_save_project,
            'refresh_offsets': self.refresh_offsets,
            'show_section_links': self.show_section_links
        }
        self.project_edit_controller = ProjectEditorController(self, self.project_edit_view, self.project,
                                                               callbacks=project_controller_callbacks,
                                                               theme=theme)

        self.gui = GUIController(parent=self, project_editor_controller=self.project_edit_controller,
                                 project_facade=self.project, inst_lib_facade=self.base_insts, theme=theme)

        self.project_edit_controller.add_callback('toggle_frame_state', self.gui.toggle_frame_state)
        self.project_edit_controller.add_callback('refresh_popup', self.gui.refresh_popup)
        self.project_edit_view.add_callback('show_project_errors', self.gui.show_project_errors)
        self.project_edit_view.add_callback('show_search', self.gui.show_project_search_popup)

        # Create Models
        self.proj_model = ProjectModel()
        self.sct_model = SCTModel()
        recent_files = self.proj_model.get_recent_filenames()

        # Create Debugger
        if os.name == 'nt' and has_debugger:
            def inst_lib():
                return self.base_insts
            debug_callbacks = {
                'update_sct': self.update_script,
                'check_for_script': self.project.check_script_is_in_project,
                'sect_name_is_used': self.project.is_sect_name_used,
                'find_similar_inst': self.project.find_similar_inst,
                'get_inst_lib': inst_lib,
                'get_sel_inst_offset': self.get_selected_inst_offset,
                'get_sect_preditor': self.project.get_sect_preditor_offset
            }
            self.dolphin_debugger = DolphinLink(callbacks=debug_callbacks, tk_parent=self)

        # Implement Menu
        self.menu_callbacks = {
            'file->new_prj': self.on_new_project,
            'file->save_prj': self.on_save_project,
            'file->save_as_prj': self.on_save_as_project,
            'file->load_prj': self.on_load_project,
            'file->load_recent': self.on_load_recent_project,
            'file->settings': self.gui.show_settings,
            'file->quit': self.on_quit,
            'prj->add_script': self.add_script,
            'prj->export_script': self.gui.show_sct_export_popup,
            'prj->variable': self.gui.show_variables_popup,
            'prj->string': self.gui.show_strings_popup,
            'prj->refresh_pos': self.refresh_offsets,
            'prj->repair->textbox': self.textbox_fadeout_repair,
            'prj->search': self.gui.show_project_search_popup,
            'prj->aklz_style': self.set_aklz_decoder,
            'analysis->var_usage': self.analyze_var_usage,
            'analysis->insts_to_clipboard': self.insts_used_to_clipboard,
            # 'analysis->export': self.gui.show_analysis_view,
            'view->inst': self.gui.show_instruction_view,
            'view->theme': self.change_theme,
            'help->help': self.gui.show_help,
            'help->about': self.gui.show_about,
        }

        if os.name == 'nt':
            self.menu_callbacks |= {
                'view->debug': lambda: self.gui.show_sct_debugger_popup(callbacks=self.dolphin_debugger.view_callbacks)
            }

        # Implement Menu
        self.menu = menus.MainMenu(parent=self, callbacks=self.menu_callbacks, recent_files=recent_files,
                                   dark_mode=self.is_darkmode, can_debug=has_debugger)
        self.config(menu=self.menu)

        # Connect recent files in proj_model to menu
        self.proj_model.add_callback('menu->update_recents', self.menu.update_recents)

        def return_darkmode():
            return self.is_darkmode

        # Set menu options available only when in script mode
        self.gui.add_callbacks({
            'script': self.menu.enable_script_commands,
            'no_script': self.menu.disable_script_commands,
            'export_sct': self.on_export_scts,
            'is_darkmode': return_darkmode,
        })

        self.gui.disable_project_view()

        # self.bind('<<ChangeTheme>>', self.propagate_change_theme)
        self.change_theme(self.is_darkmode)

    def on_new_project(self):
        self.project.create_new_project()
        self.gui.enable_project_view()
        self.gui.toggle_frame_state(self.project_edit_view.inst_frame, 'disabled')
        self.project_edit_controller.update_tree(None, None)

    def on_save_as_project(self):
        kwargs = {'filetypes': (('Project File', '*.prj'),), 'title': 'Save a project file'}
        start_dir = self.proj_model.get_project_directory()
        if start_dir != '' and start_dir is not None:
            kwargs['initialdir'] = start_dir
        filepath = filedialog.asksaveasfilename(**kwargs)
        if filepath == '' or filepath is None:
            return
        if filepath[-4:] != '.prj':
            filepath += '.prj'
        self.project.set_filepath(filepath=filepath)
        self.on_save_project()

    def on_save_project(self, quit_program=False):
        filepath = self.project.get_filepath()
        if filepath == '' or filepath is None:
            self.on_save_as_project()
            return

        self.gui.show_status_popup(title='Saving Project', msg=f'Saving Project ({os.path.basename(filepath)})')
        self.gui.disable_project_view()

        quit_queue = queue.SimpleQueue()
        save_thread = threading.Thread(target=self._save_project, args=(filepath, quit_program, quit_queue))
        save_thread.start()

        if quit_program:
            self.quit_await(quit_queue)

    def _save_project(self, filepath, quit_program, quit_queue):
        self.proj_model.save_project(proj=self.project.project, filepath=filepath)
        self.proj_model.add_recent_file(filepath=filepath)
        self.project_edit_controller.save_complete()
        self.gui.stop_status_popup()
        self.gui.enable_project_view()
        if quit_program:
            quit_queue.put('quit')

    def on_load_recent_project(self, index):
        self.on_load_project(self.proj_model.get_recent_filepath(index=index))

    def on_load_project(self, filepath=''):
        if filepath == '' or filepath is None:
            default_dir = self.proj_model.get_project_directory()
            kwargs = {'filetypes': (('Project File', '*.prj'),), 'title': 'Open a project file'}
            if default_dir is not None:
                kwargs['initialdir'] = default_dir
            filepath = filedialog.askopenfilename(**kwargs)
            if filepath == '' or filepath is None:
                print('no file selected')
                return

        self.gui.show_status_popup(title='Loading Project', msg=f'Loading Project ({os.path.basename(filepath)})')
        load_queue = queue.SimpleQueue()
        load_thread = threading.Thread(target=self._load_project, args=(filepath, load_queue))
        load_thread.start()
        self.after(10, self._load_project_listener, load_queue)

    def _load_project(self, filepath, load_queue):
        prj = self.proj_model.load_project(filepath=filepath)
        self.proj_model.add_recent_file(filepath=filepath)
        success = self.project.load_project(prj)
        load_queue.put(success)

    def _load_project_listener(self, listen_queue):
        if listen_queue.empty():
            return self.after(20, self._load_project_listener, listen_queue)
        self._finish_load_project(listen_queue.get())

    def _finish_load_project(self, success):
        if not success:
            self.gui.change_status_msg(msg=f'Project load failed :(')
            self.after(1000, self.gui.stop_status_popup)
            return
        self.menu.update_recents(self.proj_model.get_recent_filenames())
        self.project_edit_controller.load_project()
        self.gui.stop_status_popup()
        self.gui.enable_project_view()
        self.gui.toggle_frame_state(self.project_edit_view.inst_frame, 'disabled')

    def on_print_debug(self):
        print(f'\nWindow Dimensions:\nHeight: {self.winfo_height()}\nWidth: {self.winfo_width()}')

    # --------------------------- #
    # Adding scripts to a project #
    # --------------------------- #

    def add_script(self):
        kwargs = {'filetypes': (('Script File', '*.sct'),), 'title': 'Select script(s) to add to the current project'}
        base_dir = self.sct_model.get_default_directory()
        if base_dir != '':
            kwargs['initialdir'] = base_dir
        script_paths = filedialog.askopenfilenames(**kwargs)
        if script_paths == '' or script_paths is None:
            return
        self.sct_model.set_default_directory(os.path.dirname(script_paths[0]))

        self.gui.show_status_popup('Adding Script(s)', 'Adding Script:')

        script_decode_queue = queue.Queue()
        script_thread = threading.Thread(target=self._threaded_script_decoder,
                                         args=(list(script_paths), script_decode_queue, self.gui.status_queue))
        script_thread.start()
        self._script_add_listener(script_decode_queue)

    def _script_add_listener(self, decode_queue, scripts: Union[None, dict] = None):
        if scripts is None:
            scripts = {}
        if not decode_queue.empty():
            item = decode_queue.get()
            decode_queue.task_done()
            if isinstance(item, str):
                if item == 'stop':
                    return self.finish_add_script(scripts)
            else:
                scripts |= item
        self.after(20, self._script_add_listener, decode_queue, scripts)

    def _threaded_script_decoder(self, script_paths, script_decode_out_queue: queue.Queue, status_queue: queue.SimpleQueue):
        for filepath in script_paths:
            name, script = self.sct_model.load_sct(self.base_insts, file=filepath, status=status_queue)
            script_decode_out_queue.put({name: script})
        script_decode_out_queue.put('stop')

    def finish_add_script(self, scripts):
        self.project.add_scripts_to_project(scripts)
        self.gui.stop_status_popup()

    def on_quit(self):
        if self.project_edit_controller.has_changes:
            # dialog to save changes or not
            save = tk.messagebox.askyesno(title='Unsaved Changes',
                                          message='There are unsaved changes remaining.\nWould you like to save them?')
            if save:
                return self.on_save_project(quit_program=True)
        if has_debugger and os.name == 'nt':
            self.dolphin_debugger.release_handle()
        self.destroy()

    def quit_await(self, quit_queue):
        if not quit_queue.empty():
            self.on_quit()
        self.after(20, self.quit_await, quit_queue)

    # -------------------------------- #
    # Exporting scripts from a project #
    # -------------------------------- #

    def on_export_scts(self, directory, scripts, options):
        compress = options['compress_aklz']
        options.pop('compress_aklz')

        script_dict = {}
        for script in scripts:
            script_file = script
            if script_file[-4:] != '.sct':
                script_file += '.sct'
            filepath = os.path.join(directory, script_file)
            script_dict[script] = filepath

        self.gui.show_status_popup(title='Exporting Script(s)', msg=f'Exporting Script:')

        finish_queue = queue.SimpleQueue()
        script_thread = threading.Thread(target=self._threaded_script_exporter,
                                         args=(script_dict, options, compress, finish_queue, self.gui.status_queue))
        script_thread.start()
        self._script_export_listener(finish_queue)

    def _threaded_script_exporter(self, scripts, options, compress, finish_queue, status_queue: queue.SimpleQueue):
        for name, filepath in scripts.items():
            if name in self.project_edit_controller.encoding_errors:
                self.project_edit_controller.encoding_errors.remove(name)
            if name in self.project_edit_controller.script_refresh_offset_queue:
                self.project_edit_controller.script_refresh_offset_queue.remove(name)
            status_queue.put({'msg': f'Encoding {name}.sct'})
            script = self.project.get_project_script_by_name(name)
            self.sct_model.export_script_as_sct(filepath=filepath, script=script, base_insts=self.base_insts,
                                                options=options, compress=compress)
            for error in self.project.get_project_script_by_name(name).errors:
                if 'Encoding' in error:
                    self.project_edit_controller.encoding_errors.append(name)
                    self.project_edit_controller.script_refresh_offset_queue.append(name)
        finish_queue.put('stop')

    def _script_export_listener(self, decode_queue):
        if not decode_queue.empty():
            item = decode_queue.get()
            if isinstance(item, str):
                if item == 'stop':
                    self.project_edit_controller.check_encoding_errors()
                    self.project_edit_controller.refresh_all_trees()
                    return self.gui.stop_status_popup()
        self.after(20, self._script_export_listener, decode_queue)

    # ------------------------- #
    # Threaded textbox repairer #
    # ------------------------- #

    def textbox_fadeout_repair(self):
        self.gui.show_status_popup('Texbox Repair', 'Repairing script: ')
        done_queue = queue.SimpleQueue()
        thread = threading.Thread(target=self._threaded_textbox_fadeout_repair, args=(self.sct_model, self.gui.status_queue, done_queue))
        thread.start()
        self.after(20, self._textbox_fadeout_repair_listener, done_queue)

    def _threaded_textbox_fadeout_repair(self, model, sq, dq):
        self.project.repair_text_box_fade(model, sq)
        dq.put('done')

    def _textbox_fadeout_repair_listener(self, dq: queue.SimpleQueue):
        if dq.empty():
            return self.after(20, self._textbox_fadeout_repair_listener, dq)
        self.gui.stop_status_popup()

    # -------------------- #
    # Threaded Refresh Pos #
    # -------------------- #

    def refresh_offsets(self):
        scts = copy.deepcopy(self.project_edit_controller.script_refresh_offset_queue)
        self.project_edit_controller.script_refresh_offset_queue = []
        done_queue = queue.SimpleQueue()
        self.gui.show_status_popup('Refresh Absolute Positions', 'Refreshing positions:')
        thread = threading.Thread(target=self._threaded_refresh_poses, args=(scts, self.gui.status_queue, done_queue))
        thread.start()
        self.after(20, self._refresh_pos_listener, done_queue)

    def _threaded_refresh_poses(self, scripts, message_queue, done_queue):
        errored_scts = self.project.refresh_abs_poses(scripts, message_queue)
        self.project_edit_controller.encoding_errors = errored_scts
        self.project_edit_controller.script_refresh_offset_queue = [*errored_scts]
        done_queue.put('done')

    def _refresh_pos_listener(self, dq: queue.SimpleQueue):
        if dq.empty():
            return self.after(20, self._refresh_pos_listener, dq)
        self.gui.stop_status_popup()
        self.project_edit_controller.check_encoding_errors()
        self.project_edit_controller.refresh_all_trees()

    # --------------------- #
    # Update SCT in Dolphin #
    # --------------------- #

    def update_script(self, sct_name):
        done_queue = queue.SimpleQueue()
        self.gui.show_status_popup(f'Update SCT: {sct_name}', 'Refreshing positions:')
        thread = threading.Thread(target=self._threaded_update_script, args=(sct_name, self.gui.status_queue, done_queue))
        thread.start()
        self.after(20, self._update_script_listener, done_queue)

        if sct_name in self.project_edit_controller.script_refresh_offset_queue:
            self.project_edit_controller.script_refresh_offset_queue.remove(sct_name)

    def _threaded_update_script(self, script, message_queue, done_queue):
        result = self.project.get_script(script, message_queue)
        if isinstance(result, str):
            self.project_edit_controller.encoding_errors = [result]
        done_queue.put(result)

    def _update_script_listener(self, dq: queue.SimpleQueue):
        if dq.empty():
            return self.after(20, self._update_script_listener, dq)
        result = dq.get()
        self.gui.stop_status_popup()
        self.project_edit_controller.check_encoding_errors()
        self.project_edit_controller.refresh_all_trees()
        self.dolphin_debugger.write_sct_to_dolphin(result)

    # ------------- #
    # Other methods #
    # ------------- #

    def change_theme(self, dark_mode=True):
        self.is_darkmode = dark_mode
        settings.set_single(self.log_key, 'darkmode', str(dark_mode))
        theme_name = list(themes.keys())[0] if dark_mode else list(themes.keys())[1]
        theme = themes[theme_name]

        self.style.theme_use(theme_name)

        self.map_current_theme(theme)

        self.configure(bg=theme['.']['configure']['background'])

        self.project_edit_view.change_theme(theme)
        self.gui.change_theme(theme)

    def map_current_theme(self, theme):
        for item_key, arg_dict in theme.items():
            if 'map' not in arg_dict:
                continue
            self.style.map(item_key, **arg_dict['map'])

    def get_selected_inst_offset(self):
        if self.project_edit_controller.current['instruction'] is None:
            return None
        return self.project.get_inst_offset(**self.project_edit_controller.current)

    def set_aklz_decoder(self):
        set_aklz_slow(self.menu.aklz_var.get() == 1)

    def insts_used_to_clipboard(self):
        used_inst_strings = [inst[0] for inst in self.project.get_used_insts()]

        self.clipboard_clear()
        self.clipboard_append("\n".join(used_inst_strings))
        self.update()

    def analyze_var_usage(self):
        filename = asksaveasfilename(parent=self, title='Save Variable Usage CSV', filetypes=[("CSV file", '*.csv')],
                                     confirmoverwrite=True, defaultextension=".csv")
        if filename is None or filename == "":
            return
        use_verbose = False
        # use_verbose = askyesno(title='Export all usages?', message='Export all usage locations for variables? (Verbose export).\nIf "No" will just export if variable is used or not')
        print('Analyzing variable usage')
        csv_out = VarUsage.record_usage(self.project.project).get_usage_csv(use_verbose)

        with open(filename, 'w') as fh:
            fh.write(csv_out)

        print('Variable usage written to ' + filename)

    def show_section_links(self, sct, section):
        link_finder = LinkFinder.find_links(self.project.project, sct, section, self.base_insts)
        self.gui.show_links_popup(link_finder)
