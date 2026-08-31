import tkinter as tk

from SALSA.Common.containers import Vector2, Dimension


class HelpPopupView(tk.Toplevel):

    def __init__(self, parent, title, text, position: Vector2, size: Dimension, text_offset: Vector2,
                 window_offset: Vector2, close_callback, *args, **kwargs):
        super().__init__(parent, *args, **kwargs)

        self.close_callback = close_callback

        help_canvas = tk.Canvas(self, width=size.width, height=size.height)
        help_canvas.grid(row=0, column=0)
        help_canvas.create_text(text_offset.x, text_offset.y, anchor=tk.NW, text=text,
                                width=size.width - text_offset.x)
        x, y0, x, y1 = help_canvas.bbox('all')
        help_canvas.config(height=(y1-y0))
        self.quit = tk.Button(self, text='Cancel', command=self.close_callback)
        self.quit.grid(row=1, column=0)

        self.title(title)
        self.resizable(width=False, height=False)
        posX = position.x + window_offset.x
        posY = position.y + window_offset.y
        pos = f'+{posX}+{posY}'
        self.geometry(pos)


class AboutView(HelpPopupView):

    def __init__(self, parent, position, callback, *args, **kwargs):
        title = 'About'
        text = 'Skies of Arcadia Legends - Script Assistant\nby: Jahorta\n2021'
        size = Dimension(width=400, height=400)
        text_offset = Vector2(x=10, y=10)
        window_offset = Vector2(x=50, y=50)
        super().__init__(parent=parent, position=position, close_callback=callback,
                         size=size, text_offset=text_offset, window_offset=window_offset, *args, **kwargs)

    def change_theme(self, theme):
        pass