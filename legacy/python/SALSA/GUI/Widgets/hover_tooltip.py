import tkinter as tk
from tkinter import ttk
from typing import Union

tooltip_offset_x = 12
tooltip_offset_y = 1


class HoverToolTip(tk.Toplevel):

    def __init__(self, master, tooltip, x, y, destroy_callback, min_time=None, position='', is_warning=False,
                 bbox=None, *args, **kwargs):
        super().__init__(master, *args, **kwargs)

        self.master = master
        self.overrideredirect(True)
        self.wm_focusmodel('active')
        self.columnconfigure(0, weight=1)
        self.rowconfigure(0, weight=1)
        self.destroy_callback = destroy_callback
        self.master_bbox = bbox if bbox is not None else (self.master.winfo_rootx(), self.master.winfo_rooty(),
                                                          self.master.winfo_width(), self.master.winfo_height())

        tooltip_frame = ttk.Frame(self, style='tooltip.TFrame')
        tooltip_frame.grid(row=0, column=0)
        tooltip = ttk.Label(tooltip_frame, text=tooltip)
        tooltip.grid(row=0, column=0, sticky='NSEW', padx=1, pady=1)
        if is_warning:
            tooltip.configure(style='warning.TLabel')

        self.after(10, self.move_into_position, x, y, position)

        if min_time is not None:
            time = min_time
        else:
            time = 100
        self.after(time, self.check_for_destroy)

    def check_for_destroy(self):
        rel_x = self.master.winfo_pointerx() - self.master_bbox[0]
        rel_y = self.master.winfo_pointery() - self.master_bbox[1]
        if rel_x < 0 or rel_x > self.master_bbox[2] or rel_y < 0 or rel_y > self.master_bbox[3]:
            return self.destroy()
        self.after(20, self.check_for_destroy)

    def move_into_position(self, x, y, position):
        if 'above' in position:
            y -= self.winfo_height()
        elif 'below' in position:
            y += self.winfo_height()
        else:
            y += tooltip_offset_y

        if 'left' in position:
            x = self.master_bbox[0]
        elif 'center' in position:
            c = self.master_bbox[0] + self.master_bbox[2]//2
            x = c - self.winfo_width()//2
        elif 'right' in position:
            r = self.master_bbox[0] + self.master_bbox[2]
            x = r - self.winfo_width()
        else:
            x += tooltip_offset_x

        self.geometry(f'+{x}+{y}')

    def destroy(self, super_destroy=False):
        if super_destroy:
            return super().destroy()
        self.destroy_callback(self)


tooltip_is_active = False
active_tooltip: Union[None, HoverToolTip] = None
tooltip_delay = 700
tooltip_hover_interval = 50


def schedule_tooltip(master, tooltip, delay=tooltip_delay, *args, **kwargs):
    global tooltip_is_active
    if tooltip_is_active:
        return
    tooltip_is_active = True
    if delay > 0:
        check_hover(master, tooltip, delay, args, kwargs)
    else:
        tooltip_generator(master, tooltip, master.winfo_pointerx(), master.winfo_pointery(), args, kwargs)


def check_hover(master, tooltip, total_delay, args, kwargs, cur_delay=0):
    x, y, w, h = kwargs['bbox'] if 'bbox' in kwargs else (master.winfo_rootx(), master.winfo_rooty(),
                                                          master.winfo_width(), master.winfo_height())

    rel_x = master.winfo_pointerx() - x
    rel_y = master.winfo_pointery() - y
    if rel_x < 0 or rel_x > w or rel_y < 0 or rel_y > h:
        global tooltip_is_active
        tooltip_is_active = False
        return
    if cur_delay < total_delay:
        cur_delay += tooltip_hover_interval
        master.after(20, check_hover, master, tooltip, total_delay, args, kwargs, cur_delay)
    else:
        tooltip_generator(master, tooltip, master.winfo_pointerx(), master.winfo_pointery(), args, kwargs)


def tooltip_generator(master, tooltip, x, y, args, kwargs):
    global active_tooltip
    if active_tooltip is not None:
        return
    active_tooltip = HoverToolTip(master, tooltip, x, y, destroy_tooltip, *args, **kwargs)
    active_tooltip.tkraise()


def destroy_tooltip(tooltip):
    global tooltip_is_active
    tooltip_is_active = False
    tooltip.destroy(super_destroy=True)
    global active_tooltip
    active_tooltip = None


if __name__ == '__main__':
    win = tk.Tk()
    win.geometry('+200+200')

    label = tk.Label(win, text='test label for tooltip')
    label.pack()
    label.bind('<Enter>', lambda e: schedule_tooltip(label, 'This is a tooltip', e))

    win.mainloop()
