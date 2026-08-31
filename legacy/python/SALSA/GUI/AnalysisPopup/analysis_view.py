import tkinter as tk
from tkinter import ttk

from SALSA.GUI.Widgets import widgets as w


class AnalysisView(tk.Toplevel):
    window_offset = {
        'x': 50,
        'y': 50
    }

    size = {
        'width': 400,
        'height': 400
    }

    text_offset = {
        'x': 10,
        'y': 10
    }

    def __init__(self, parent, title, position, callbacks, *args, **kwargs):
        super().__init__(parent, *args, **kwargs)

        self.callbacks = callbacks
        self.export_notebook = ttk.Notebook(self, width=self.size['width'], height=self.size['height'])
        self.export_notebook.grid(row=0, column=0)
        self.export_notebook.rowconfigure(0, weight=1)
        self.export_notebook.columnconfigure(0, weight=1)

        self.export_frame = tk.Frame(self)
        self.export_frame.grid(row=0, column=0, sticky='NSEW')
        # self.export_selector = w.LabelInput(self.export_frame, 'Name', field_spec=export_fields['types'])
        # self.export_selector.set(export_fields['types']['values'][0])
        # self.export_selector.grid(row=0, column=0)
        self.button_extract_data = tk.Button(self.export_frame, text='Extract Data', command=self.export_selected_data)
        self.button_extract_data.grid(row=1, column=0)

        button_frame = tk.Frame(self)
        button_frame.grid(row=1, column=0)
        self.quit = tk.Button(button_frame, text='Cancel', command=lambda: self.callbacks['on_close'](window='export'))
        self.quit.grid(row=0, column=1)
        self.export_all = tk.Button(button_frame, text='Write all to CSVs', command=lambda: self.write_csv('all'))
        progress_frame = tk.LabelFrame(self, text='Progress')
        progress_frame.grid(row=2, column=0)
        self.progress_text = tk.StringVar()
        self.progress_text.set('')
        progress_text_label = tk.Label(self, textvariable=self.progress_text)
        progress_text_label.grid(row=2, column=0, sticky=tk.W+tk.E)
        self.progress_bar = ttk.Progressbar(self, orient='horizontal', mode='determinate', length=100)
        self.progress_bar.grid(row=3, column=0, sticky=tk.W+tk.E)
        self.title(title)
        self.resizable(width=True, height=True)
        posX = position['x'] + self.window_offset['x']
        posY = position['y'] + self.window_offset['y']
        pos = f'+{posX}+{posY}'
        self.geometry(pos)

        self.export_values = {}

    def update_exports(self, exports):
        self.export_values = exports

        for tab in reversed(range(0, len(self.export_notebook.tabs()))):
            self.export_notebook.forget(tab)

        for i, (k, v) in enumerate(self.export_values.items()):
            tab_frame = tk.Frame(self.export_notebook)
            tab_frame.rowconfigure(0, weight=1)
            tab_frame.columnconfigure(0, weight=1)
            self.export_notebook.add(tab_frame)
            self.export_notebook.tab(i, text=k)

            tab_text = w.TextScrollLabelCanvas(tab_frame, size=self.size, canvas_text_offset=self.text_offset,
                                               canvas_text=v)
            tab_text.grid(row=0, column=0)

            button_frame = tk.Frame(tab_frame)
            button_frame.grid(row=1, column=0)
            tab_button = tk.Button(button_frame, text='Copy to clipboard', command=self.send_to_clipboard)
            tab_button.grid(row=0, column=0)
            tab_button = tk.Button(button_frame, text='Write tab to CSV', command=lambda: self.write_csv('tab'))
            tab_button.grid(row=0, column=1)

        self.export_all.grid(row=0, column=0)
        self.export_notebook.tkraise()

    def export_selected_data(self):
        print('export button pressed')
        self.button_extract_data['state'] = 'disabled'
        # export_type = self.export_selector.get()
        # if export_type == '':
        #     self.update_progress(0, 'No Export Value Selected...')
        #     return
        # self.callbacks['on_export'](export_type=export_type)

    def send_to_clipboard(self):
        key = self.export_notebook.tab(self.export_notebook.select(), 'text')
        export = self.export_values[key]
        self.clipboard_clear()
        self.clipboard_append(export)
        print(f'sent {key} to clipboard')

    def update_progress(self, percent, text):
        self.progress_text.set(text)
        self.progress_bar['value'] = percent

    def write_csv(self, number):
        if number == 'all':
            to_csv = self.export_values
        else:
            tab_name = self.export_notebook.tab(self.export_notebook.select(), 'text')
            to_csv = {tab_name:  self.export_values[tab_name]}

        self.callbacks['on_write_to_csv'](to_csv)
