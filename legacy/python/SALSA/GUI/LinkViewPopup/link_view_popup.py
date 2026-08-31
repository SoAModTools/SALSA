import tkinter as tk
from tkinter import ttk

from SALSA.GUI.fonts_used import SALSAFont
from SALSA.Analysis.link_finder import LinkFinder
from SALSA.GUI.Widgets.widgets import ScrollFrame


class LinkViewPopup(tk.Toplevel):

    t = 'SALSA - Section Link Viewer'
    log_key = 'SectLinkPopup'
    w = 600
    h = 500

    def __init__(self, parent, name, callbacks, link_registry, theme, *args, **kwargs):
        super().__init__(parent, *args, **kwargs)

        self.parent: tk.Tk = parent
        self.callbacks = callbacks
        self.name = name
        self.protocol('WM_DELETE_WINDOW', self.close)
        self.theme = theme
        self.configure(**self.theme['Ttoplevel']['configure'])
        self.link_registry: LinkFinder = link_registry

        self.title(self.t)

        posX = self.parent.winfo_x() + (self.parent.winfo_width() - self.w)//2
        posY = self.parent.winfo_y() + (self.parent.winfo_height() - self.h)//2
        pos = f'{self.w}x{self.h}+{posX}+{posY}'
        self.geometry(pos)

        self.columnconfigure(0, weight=1)
        self.rowconfigure(0, weight=1)
        self.rowconfigure(1, weight=1)

        self.link_in_rows = []
        self.link_in_links = []

        link_target_string = self.link_registry.target_sct + ': ' + self.link_registry.target_sect

        links_in_frame = tk.LabelFrame(self, text='Links In to ' + link_target_string)
        links_in_frame.grid(row=0, column=0, sticky='NSEW')
        links_in_frame.columnconfigure(0, weight=1)
        links_in_frame.rowconfigure(0, weight=1)
        self.links_in_frame = ScrollFrame(links_in_frame, theme=self.theme)
        self.links_in_frame.grid(row=0, column=0, sticky='NSEW')

        self.link_out_rows = []
        self.link_out_links = []

        links_out_frame = tk.LabelFrame(self, text='Links out from ' + link_target_string)
        links_out_frame.grid(row=1, column=0, sticky='NSEW')
        links_out_frame.columnconfigure(0, weight=1)
        links_out_frame.rowconfigure(0, weight=1)
        self.links_out_frame = ScrollFrame(links_out_frame, theme=self.theme)
        self.links_out_frame.grid(row=0, column=0, sticky='NSEW')

        self.link_font = SALSAFont()

        self.update_link_widgets()

    def update_link_widgets(self, new_registry=None):
        if new_registry is not None:
            self.link_registry = new_registry
        links_in = self.link_registry.links_in.get_rows()
        for r, row in enumerate(links_in):
            row_frame = tk.Frame(self.links_in_frame.scroll_frame)
            row_frame.grid(row=r, column=0, sticky=tk.E + tk.W, pady='5 0', padx=5)
            i = 0
            for part in row:
                if i > 0:
                    part_label_prefix = ttk.Label(row_frame, text='->', style='canvas.TLabel')
                    part_label_prefix.grid(row=0, column=i, sticky='NSEW')
                    i += 1

                part_frame = tk.Frame(row_frame)
                part_frame.grid(row=len(self.link_in_rows), column=i, sticky=tk.E + tk.W)
                part_frame.columnconfigure(0, weight=1)

                part_label = ttk.Label(part_frame, text=part[0],
                                      font=self.link_font.default_font, style='canvas.TLabel')
                part_label.grid(row=0, column=1, sticky=tk.E + tk.W)

                if len(part) == 2:
                    part_label.bind('<ButtonRelease-1>', lambda e, idx=len(self.link_in_links): self.goto('in', idx, e))
                    part_frame.bind('<Enter>', self.handle_link_font)
                    part_frame.bind('<Leave>', self.handle_link_font)
                    self.link_in_links.append(part[1])

                i += 1

            last_label = ttk.Label(row_frame, text='', style='canvas.TLabel')
            last_label.grid(row=0, column=i, sticky='NSEW')
            row_frame.columnconfigure(i, weight=1)

        links_out = self.link_registry.links_out.get_rows()
        for r, row in enumerate(links_out):
            row_frame = tk.Frame(self.links_out_frame.scroll_frame)
            row_frame.grid(row=r, column=0, sticky=tk.E + tk.W, pady='5 0', padx=5)
            i = 0
            for part in row:
                if i > 0:
                    part_label_prefix = ttk.Label(row_frame, text='->', style='canvas.TLabel')
                    part_label_prefix.grid(row=0, column=i, sticky='NSEW')
                    i += 1

                part_frame = tk.Frame(row_frame)
                part_frame.grid(row=len(self.link_out_rows), column=i, sticky=tk.E + tk.W)
                part_frame.columnconfigure(0, weight=1)

                part_label = ttk.Label(part_frame, text=part[0],
                                      font=self.link_font.default_font, style='canvas.TLabel')
                part_label.grid(row=0, column=0, sticky=tk.E + tk.W)

                if len(part) == 2:
                    part_label.bind('<ButtonRelease-1>', lambda e, i=len(self.link_out_links): self.goto('out', i, e))
                    part_frame.bind('<Enter>', self.handle_link_font)
                    part_frame.bind('<Leave>', self.handle_link_font)
                    self.link_out_links.append(part[1])

                i += 1
            last_label = ttk.Label(row_frame, text='', style='canvas.TLabel')
            last_label.grid(row=0, column=i, sticky='NSEW')
            row_frame.columnconfigure(i, weight=1)

    def close(self):
        self.callbacks['close'](self.name, self)


    def change_theme(self, theme):
        self.theme = theme
        self.configure(**self.theme['Ttoplevel']['configure'])
        self.links_in_frame.change_theme(theme)
        self.links_out_frame.change_theme(theme)

    def handle_link_font(self, e):
        font = self.link_font.default_font if e.type == tk.EventType.Leave else self.link_font.hover_font
        style = 'canvas.TLabel' if e.type == tk.EventType.Leave else 'link.TLabel'
        for child in e.widget.winfo_children():
            child.configure(font=font, style=style)

    def goto(self, direction, i, *args):
        if direction == 'in':
            row_trace = self.link_in_links[i]
        else:
            row_trace = self.link_out_links[i]
        self.callbacks['goto'](self.link_registry.target_sct, row_trace[0], row_trace[1])