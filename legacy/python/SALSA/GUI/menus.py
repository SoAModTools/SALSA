import tkinter as tk


class MainMenu(tk.Menu):
    """The application's main menu"""

    def __init__(self, parent, callbacks, recent_files, dark_mode, can_debug, **kwargs):
        super().__init__(parent, **kwargs)

        self.recent_files = recent_files
        self.no_recents = True if len(self.recent_files) == 0 else False
        self.recent_files = ['No Recent Files'] if len(self.recent_files) == 0 else recent_files

        self.callbacks = callbacks
        self.can_debug = can_debug

        self.parent = parent
        self.file_menu = tk.Menu(self, tearoff=False)
        self.file_menu.add_command(label='New Project', command=callbacks['file->new_prj'])
        self.file_menu.add_command(label='Save Project', command=callbacks['file->save_prj'])
        self.file_menu.add_command(label='Save Project as', command=callbacks['file->save_as_prj'])
        self.file_menu.add_command(label='Load Project', command=callbacks['file->load_prj'])

        self.recent_menu = tk.Menu(self.file_menu, tearoff=False)
        for i, file in enumerate(self.recent_files):
            self.recent_menu.add_command(label=file, command=lambda x=i: self.callbacks['file->load_recent'](x))
        if self.no_recents:
            self.recent_menu.entryconfig(self.recent_files[0], state='disabled')
        self.file_menu.add_cascade(label='Recent Projects...', menu=self.recent_menu)

        self.file_menu.add_command(label='Quit', command=callbacks['file->quit'])
        self.add_cascade(label='File', menu=self.file_menu)

        self.project_menu = tk.Menu(self, tearoff=False)
        self.project_menu.add_command(label='Import SCT file(s)', command=callbacks['prj->add_script'])
        self.project_menu.add_command(label='Export SCT file(s)', command=callbacks['prj->export_script'])
        self.project_menu.add_separator()
        self.project_menu.add_command(label='Variable alias editor', command=callbacks['prj->variable'])
        self.project_menu.add_command(label='String editor', command=callbacks['prj->string'])
        self.project_menu.add_command(label='Project Search', command=callbacks['prj->search'])
        self.project_menu.add_separator()
        self.project_menu.add_command(label='Refresh Sect/Inst Offsets', command=callbacks['prj->refresh_pos'])
        self.p_repair_menu = tk.Menu(self.project_menu, tearoff=False)
        self.p_repair_menu.add_command(label='Repair Textbox Fadeout', command=callbacks['prj->repair->textbox'])
        self.project_menu.add_cascade(label='Repair', menu=self.p_repair_menu)
        self.aklz_var = tk.IntVar(master=self, value=0)
        self.project_menu.add_checkbutton(label='Use Legacy AKLZ', variable=self.aklz_var,
                                          onvalue=1, offvalue=0, command=callbacks['prj->aklz_style'])
        self.add_cascade(label='Project', menu=self.project_menu)

        analysis_menu = tk.Menu(self, tearoff=False)
        # analysis_menu.add_command(label='Export Data', command=callbacks['analysis->export'])
        analysis_menu.add_command(label='Export Project Var Usage', command=callbacks['analysis->var_usage'])
        analysis_menu.add_command(label='Copy Used Insts to Clipboard', command=callbacks['analysis->insts_to_clipboard'])
        self.add_cascade(label='Analysis', menu=analysis_menu)

        self.view_menu = tk.Menu(self, tearoff=False)
        self.view_menu.add_command(label='Instruction editor', command=callbacks['view->inst'])
        self.has_debug = False
        if 'view->debug' in callbacks:
            self.view_menu.add_command(label='Dolphin Link', command=callbacks['view->debug'])
            self.has_debug = True
        dark_mode = 1 if dark_mode else 0
        self.dark_mode_var = tk.IntVar(self, dark_mode)
        self.view_menu.add_checkbutton(label='Use Dark Mode', variable=self.dark_mode_var,
                                       onvalue=1, offvalue=0, command=self.toggle_dark_mode)
        self.add_cascade(label='View', menu=self.view_menu)

        help_menu = tk.Menu(self, tearoff=False)
        help_menu.add_command(label='Help', command=callbacks['help->help'])
        help_menu.add_command(label='About', command=callbacks['help->about'])
        self.add_cascade(label='Help', menu=help_menu)

        self.all_menus = [
            self.file_menu,
            self.recent_menu,
            self.project_menu,
            # analysis_menu,
            self.view_menu,
            help_menu]

        self.toggle_dark_mode()

    def disable_script_commands(self):
        self.project_menu.entryconfig('Import SCT file(s)', state='disabled')
        self.project_menu.entryconfig('Export SCT file(s)', state='disabled')
        self.project_menu.entryconfig('Variable alias editor', state='disabled')
        self.project_menu.entryconfig('String editor', state='disabled')
        self.project_menu.entryconfig('Refresh Sect/Inst Offsets', state='disabled')
        self.project_menu.entryconfig('Project Search', state='disabled')
        self.project_menu.entryconfig('Repair', state='disabled')
        self.project_menu.entryconfig('Use Legacy AKLZ', state='disabled')
        self.file_menu.entryconfig('Save Project', state='disabled')
        self.file_menu.entryconfig('Save Project as', state='disabled')
        if self.has_debug:
            self.view_menu.entryconfig('Dolphin Link', state='disabled')

    def enable_script_commands(self):
        self.project_menu.entryconfig('Import SCT file(s)', state='normal')
        self.project_menu.entryconfig('Export SCT file(s)', state='normal')
        self.project_menu.entryconfig('Variable alias editor', state='normal')
        self.project_menu.entryconfig('String editor', state='normal')
        self.project_menu.entryconfig('Refresh Sect/Inst Offsets', state='normal')
        self.project_menu.entryconfig('Project Search', state='normal')
        self.project_menu.entryconfig('Repair', state='normal')
        self.project_menu.entryconfig('Use Legacy AKLZ', state='normal')
        self.file_menu.entryconfig('Save Project', state='normal')
        self.file_menu.entryconfig('Save Project as', state='normal')
        if self.has_debug and self.can_debug:
            self.view_menu.entryconfig('Dolphin Link', state='normal')

    def update_recents(self, recent_files):
        self.no_recents = True if len(self.recent_files) == 0 else False
        self.recent_files = ['No Recent Files'] if len(self.recent_files) == 0 else recent_files

        self.recent_menu.delete(0, 'end')
        for i, file in enumerate(self.recent_files):
            self.recent_menu.add_command(label=file, command=lambda x=i: self.callbacks['file->load_recent'](x))
        if self.no_recents:
            self.recent_menu.entryconfig('No Recent Files', state='disabled')

    def toggle_dark_mode(self):
        dark_mode = True if self.dark_mode_var.get() == 1 else False
        self.callbacks['view->theme'](dark_mode)
