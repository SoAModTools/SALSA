import math
import platform
import re
import tkinter as tk
from tkinter import ttk
from typing import Dict, List, Union

from SALSA.Common.constants import sep


# ------ #
# Mixins #
# ------ #


class ValidatedMixin:
    """Adds a validation functionality to an input widget"""

    def __init__(self, *args, error_var=None, **kwargs):
        self.error = error_var or tk.StringVar()
        super().__init__(*args, **kwargs)

        vcmd = self.register(self._validate)
        invcmd = self.register(self._invalid)

        self.valid = True

        self.config(
            validate='all',
            validatecommand=(vcmd, '%P', '%s', '%S', '%V', '%i', '%d'),
            invalidcommand=(invcmd, '%P', '%s', '%S', '%V', '%i', '%d')
        )

    def _toggle_error(self, on=False):
        # self.config(foreground=('red' if on else 'black'))
        pass

    def _validate(self, proposed, current, char, event, index, action):
        """The validation method.

        Don't override this, override _key_validate, and _focus_validate
        """
        self._toggle_error(False)
        self.error.set('')
        valid = True
        if event == 'focusout':
            valid = self._focusout_validate(event=event)
        elif event == 'key':
            valid = self._key_validate(
                proposed=proposed,
                current=current,
                char=char,
                event=event,
                index=index,
                action=action
            )
        self.valid = valid
        return valid

    def _focusout_validate(self, **kwargs):
        return True

    def _key_validate(self, **kwargs):
        return True

    def _invalid(self, proposed, current, char, event, index, action):
        if event == 'focusout':
            self._focusout_invalid(event=event)
        elif event == 'key':
            self._key_invalid(
                proposed=proposed,
                current=current,
                char=char,
                event=event,
                index=index,
                action=action
            )

    def _focusout_invalid(self, **kwargs):
        """Handle invalid data on a focus event"""
        self._toggle_error(True)
        pass

    def _key_invalid(self, **kwargs):
        """Handle invalid data on a key event.  By default we want to do nothing"""
        pass

    def trigger_focusout_validation(self):
        valid = self._validate('', '', '', 'focusout', '', '')
        if not valid:
            self._focusout_invalid(event='focusout')
        return valid


class RequiredEntryMixin:

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)

    def _focusout_validate(self, event):
        valid = True
        if self.get() == '':
            valid = False
            self.error.set('A value is required')
        return valid


# -------------- #
# Widget Classes #
# -------------- #

class LabelNameEntry(ValidatedMixin, ttk.Entry):
    def __init__(self, master, *args, **kwargs):
        super().__init__(master, *args, **kwargs)

    def _key_validate(self, char, index, current, proposed, action, **kwargs):
        valid = True

        if len(proposed) > 16:
            return False

        if not re.search('^[a-zA-Z0-9_]*$', proposed):
            valid = False

        return valid


class RequiredEntry(ValidatedMixin, RequiredEntryMixin, ttk.Entry):

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)


class HexEntry(ValidatedMixin, ttk.Entry):
    def __init__(self, *args, hex_max_length: int, hex_min_length: int, **kwargs):
        super().__init__(*args, **kwargs)

        self.pattern_internal = "[0-9,a-f]"
        self.prefix = "^"
        self.min_len = hex_min_length
        min_str = '0' * self.min_len
        self.insert(0, min_str)
        self.max_len = hex_max_length
        self.suffix = '{' + str(self.min_len) + ',' + str(self.max_len) + '}$'
        self._set_pattern()

    def _set_pattern(self):
        self.pattern = self.prefix + self.pattern_internal + self.suffix

    def _key_validate(self, char, index, current, proposed, action, **kwargs):
        valid = True

        if not re.search(self.pattern, proposed):
            valid = False
            error = ''
            if not self.min_len < len(proposed) < self.max_len:
                error = f'length of field should be between {self.min_len} and {self.max_len}'
            valid_chars = ['abcdefABCDEF1234567890']
            if char not in valid_chars:
                error = f'Invalid character: {char}, use one of these for hexadecimal: {"".join(valid_chars)}'
            self.error.set(error)

        return valid


class RequiredHexEntry(RequiredEntryMixin, HexEntry):

    def __init__(self, hex_max_length: int, *args, hex_min_length: int = 0, **kwargs):
        super().__init__(*args, hex_max_length=hex_max_length, hex_min_length=hex_min_length, **kwargs)

    def _focusout_validate(self, **kwargs):
        valid = super()

        if not re.search(self.pattern, self.get()):
            valid = False

        return valid


class RequiredAddrEntry(RequiredHexEntry):

    def __init__(self, *args, **kwargs):
        super().__init__(*args, hex_min_length=3, **kwargs)

        self.pattern_length = 8
        self.prefix = '^0x8'
        self.suffix = '{7}$'

        self.set('0x8')

    def _focusout_validate(self, **kwargs):
        valid = super()._focusout_validate(**kwargs)

        if not len(self.get()) == self.pattern_length:
            valid = False
            error = f'Final address should be the full '
            self.error.set(f'error')
        return valid


class RequiredIntEntry(RequiredEntry):
    def __init__(self, parent, signed=True, b_min=-math.inf, b_max=math.inf, *a, **kw):

        super().__init__(parent, *a, **kw)

        self.max = b_max
        self.min = b_min

        self.signed = signed

    def _focusout_validate(self, **kwargs):
        valid = super()._focusout_validate(**kwargs)

        if self.get() == '':
            return False

        return valid

    def _key_validate(self, char, index, current, proposed, action, **kwargs):
        valid = super()._key_validate(char=char, index=index, current=current, proposed=proposed, action=action, **kwargs)

        chars = '-1234567890'
        if self.signed:
            chars += '-'

        for c in char:
            if c not in chars:
                return False

        if '-' in proposed:
            if proposed.count('-') > 1:
                return False
            if not re.search('^-', proposed):
                return False

        if proposed in ('', '-'):
            return True

        if int(proposed) < self.min or int(proposed) > self.max:
            return False

        return valid

    def set_bounds(self, b_min=-math.inf, b_max=math.inf):
        self.min = b_min
        self.max = b_max


class RequiredFloatEntry(RequiredEntry):
    def __init__(self, parent, b_min=-math.inf, b_max=math.inf, *a, **kw):
        super().__init__(parent, *a, **kw)

        self.max = b_max
        self.min = b_min

    def _focusout_validate(self, **kwargs):
        valid = super()._focusout_validate(**kwargs)

        if self.get() == '':
            return False

        return valid

    def _key_validate(self, char, index, current, proposed, action, **kwargs):
        valid = super()._key_validate(char=char, index=index, current=current, proposed=proposed, action=action,
                                      **kwargs)

        for c in char:
            if c not in '-1234567890.':
                return False

        if proposed.count('-') > 1:
            return False

        if proposed.count('.') > 1:
            return False

        if '-' in proposed:
            if not re.search('^-', proposed):
                return False

        if proposed in ('', '-'):
            return True

        if float(proposed) < self.min or float(proposed) > self.max:
            return False

        return valid

    def _focusout_invalid(self, proposed, **kwargs):

        if not re.search('^-?[0-9]+[.][0-9]+', proposed):
            return False

    def set_bounds(self, b_min=-math.inf, b_max=math.inf):
        self.min = b_min
        self.max = b_max


# ------------- #
# Frame Classes #
# ------------- #


canvas_default_theme = {
    'TCanvas': {
        'configure': {
            'background': 'white'
        }
    }
}


class ScrollCanvas(ttk.Frame):

    def __init__(self, parent, size: dict, theme=None, *args, **kwargs):

        super().__init__(parent, *args, **kwargs)

        self.rowconfigure(0, weight=1)
        self.columnconfigure(0, weight=1)
        self.parent = parent

        theme = theme if theme is not None else canvas_default_theme
        self.canvas = tk.Canvas(self, width=size['width'], height=size['height'], **theme['TCanvas']['configure'])
        self.canvas.grid(row=0, column=0, sticky='NSEW')

        self.canvas_scrollbar = ttk.Scrollbar(self, orient="vertical", command=self.canvas.yview)
        self.canvas_scrollbar.grid(row=0, column=1, sticky=tk.N + tk.S)

        self.canvas.configure(yscrollcommand=self.canvas_scrollbar.set)
        self.canvas.config(scrollregion=self.canvas.bbox("all"))

        self.canvas.bind('<Enter>', self.onEnter)  # bind wheel events when the cursor enters the control
        self.canvas.bind('<Leave>', self.onLeave)  # unbind wheel events when the cursor leaves the control

    # cross platform scroll wheel event
    def onMouseWheel(self, event):
        if platform.system() == 'Windows':
            self.canvas.yview_scroll(int(-1 * (event.delta / 120)), "units")
        elif platform.system() == 'Darwin':
            self.canvas.yview_scroll(int(-1 * event.delta), "units")
        else:
            if event.num == 4:
                self.canvas.yview_scroll(-1, "units")
            elif event.num == 5:
                self.canvas.yview_scroll(1, "units")

    # bind wheel events when the cursor enters the control
    def onEnter(self, event):
        if platform.system() == 'Linux':
            self.canvas.bind_all("<Button-4>", self.onMouseWheel)
            self.canvas.bind_all("<Button-5>", self.onMouseWheel)
        else:
            self.canvas.bind_all("<MouseWheel>", self.onMouseWheel)

    # unbind wheel events when the cursor leaves the control
    def onLeave(self, event):
        if platform.system() == 'Linux':
            self.canvas.unbind_all("<Button-4>")
            self.canvas.unbind_all("<Button-5>")
        else:
            self.canvas.unbind_all("<MouseWheel>")

    def change_theme(self, theme):
        w = self.canvas['width']
        h = self.canvas['height']
        self.canvas.configure(**theme['TCanvas']['configure'], width=w, height=h)

    def set_size(self, w, h):
        self.canvas.config(scrollregion=self.canvas.bbox("all"))
        w -= self.canvas_scrollbar.winfo_width()
        self.canvas.configure(width=w, height=h)

    def delete_all(self):
        objs = self.canvas.find_all()
        for obj in objs:
            self.canvas.delete(obj)


class ScrollLabelCanvas(ttk.LabelFrame):

    def __init__(self, parent, size: dict, has_label=True, canvas_style=None, theme=None, *args, **kwargs):
        if not has_label:
            kwargs = kwargs
            kwargs['text'] = ''

        super().__init__(parent, *args, **kwargs)

        self.rowconfigure(0, weight=1)
        self.columnconfigure(0, weight=1)
        self.parent = parent
        self.canvas_style = 'TCanvas' if canvas_style is None else canvas_style

        theme = theme if theme is not None else canvas_default_theme
        self.canvas = tk.Canvas(self, width=size['width'], height=size['height'], **theme[self.canvas_style]['configure'])
        self.canvas.grid(row=0, column=0, sticky='NSEW')

        self.canvas_scrollbar = ttk.Scrollbar(self, orient="vertical", command=self.canvas.yview)
        self.canvas_scrollbar.grid(row=0, column=1, sticky=tk.N + tk.S)

        self.canvas.configure(yscrollcommand=self.canvas_scrollbar.set)
        self.canvas.config(scrollregion=self.canvas.bbox("all"))

        self.canvas.bind('<Enter>', self.onEnter)  # bind wheel events when the cursor enters the control
        self.canvas.bind('<Leave>', self.onLeave)  # unbind wheel events when the cursor leaves the control

    # cross platform scroll wheel event
    def onMouseWheel(self, event):
        if platform.system() == 'Windows':
            self.canvas.yview_scroll(int(-1 * (event.delta / 120)), "units")
        elif platform.system() == 'Darwin':
            self.canvas.yview_scroll(int(-1 * event.delta), "units")
        else:
            if event.num == 4:
                self.canvas.yview_scroll(-1, "units")
            elif event.num == 5:
                self.canvas.yview_scroll(1, "units")

    # bind wheel events when the cursor enters the control
    def onEnter(self, event):
        if platform.system() == 'Linux':
            self.canvas.bind_all("<Button-4>", self.onMouseWheel)
            self.canvas.bind_all("<Button-5>", self.onMouseWheel)
        else:
            self.canvas.bind_all("<MouseWheel>", self.onMouseWheel)

    # unbind wheel events when the cursor leaves the control
    def onLeave(self, event):
        if platform.system() == 'Linux':
            self.canvas.unbind_all("<Button-4>")
            self.canvas.unbind_all("<Button-5>")
        else:
            self.canvas.unbind_all("<MouseWheel>")

    def set_canvas_style(self, style):
        self.canvas_style = style

    def change_theme(self, theme):
        self.canvas.configure(**theme[self.canvas_style]['configure'])


text_default_theme = {
    'TCanvasText': {
        'configure': {
            'fill': 'black'
        }
    }
}


class TextScrollLabelCanvas(ScrollLabelCanvas):

    def __init__(self, parent, size: dict, canvas_text_offset: dict, canvas_text: str, theme=None, has_label=True, *args, **kwargs):
        super().__init__(parent, size, has_label=has_label, theme=theme, *args, **kwargs)

        theme = theme if theme is not None else text_default_theme

        fill = theme['TCanvasText']['configure']['fill']
        self.text = self.canvas.create_text((canvas_text_offset['x'], canvas_text_offset['y']),
                                            anchor=tk.NW, text=canvas_text, fill=fill,
                                            width=size['width'] - canvas_text_offset['x'])

    def change_theme(self, theme):
        super().change_theme(theme)
        fill = theme['TCanvasText']['configure']['fill']

        self.canvas.itemconfigure(self.text, fill=fill)


class ScrollFrame(ScrollCanvas):

    def __init__(self, parent, size=None, theme=None, *args, **kwargs):
        size = {'width': 100, 'height': 100} if size is None else size
        super().__init__(parent, size, theme=theme, *args, **kwargs)

        self.scroll_frame = ttk.Frame(self.canvas, style='canvas.TFrame')

        self.canvas_window = self.canvas.create_window(0, 0, window=self.scroll_frame, anchor='nw',
                                                       tags='self.viewport')

        # bind an event whenever the size of the viewPort frame changes.
        self.scroll_frame.bind("<Configure>", self.onCanvasContentChange)

        self.onCanvasContentChange(None)

        # whenever the size of the frame changes, alter the scroll region respectively.

    def onCanvasContentChange(self, event):
        """Reset the scroll region to encompass the canvas contents"""
        self.canvas.configure(scrollregion=self.canvas.bbox("all"))

    def change_theme(self, theme):
        super().change_theme(theme)


class ScrollLabelFrame(ScrollLabelCanvas):

    def __init__(self, parent, size=None, has_label=True, theme=None, canvas_style=None, *args, **kwargs):
        size = {'width': 100, 'height': 100} if size is None else size
        super().__init__(parent, size, has_label=has_label, theme=theme, *args, **kwargs)

        self.scroll_frame = ttk.Frame(self.canvas, style='canvas.TFrame')

        self.canvas_window = self.canvas.create_window(2, 2, window=self.scroll_frame, anchor='nw',
                                                       tags='self.viewport')

        if canvas_style is not None:
            self.set_canvas_style(canvas_style)

        # bind an event whenever the size of the viewPort frame changes.
        self.scroll_frame.bind("<Configure>", self.onCanvasContentChange)

        self.onCanvasContentChange(None)

    # whenever the size of the frame changes, alter the scroll region respectively.
    def onCanvasContentChange(self, event):
        """Reset the scroll region to encompass the canvas contents"""
        self.canvas.configure(scrollregion=self.canvas.bbox("all"))

    def change_theme(self, theme):
        super().change_theme(theme)


class ThemedScrolledText(tk.Text):

    def __init__(self, master=None, **kw):
        self.frame = ttk.Frame(master)
        self.vbar = ttk.Scrollbar(self.frame)
        self.vbar.pack(side=tk.RIGHT, fill=tk.Y)

        kw.update({'yscrollcommand': self.vbar.set})
        tk.Text.__init__(self, self.frame, **kw)
        self.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        self.vbar['command'] = self.yview

        # Copy geometry methods of self.frame without overriding Text
        # methods -- hack!
        text_meths = vars(tk.Text).keys()
        methods = vars(tk.Pack).keys() | vars(tk.Grid).keys() | vars(tk.Place).keys()
        methods = methods.difference(text_meths)

        for m in methods:
            if m[0] != '_' and m != 'config' and m != 'configure':
                setattr(self, m, getattr(self.frame, m))

    def __str__(self):
        return str(self.frame)

    def change_theme(self, theme):
        self.configure(**theme['text']['configure'])
