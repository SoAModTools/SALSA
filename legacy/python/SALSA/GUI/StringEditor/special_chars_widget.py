import os
import tkinter as tk
from tkinter import ttk
from typing import Dict, Callable, Literal

from SALSA.GUI.Widgets.widgets import ScrollCanvas

sp_chars = {
    'EU': '‘’“”‚„…€ƒ†‡ˆ‰Š‹ŒŽ•–—˜™š›œžŸ¡¢£¤¥¦§¨©ª¬®¯°±²³´µ¶·¸¹º«»¼½¾¿ÀÁÂÃÄÅÆÇÈÉÊËÌÍÎÏÐÑÒÓÔÕÖ×ØÙÚÛÜÝÞàáâãäåæçèéêëìíîïðñòóôõö÷øùúûüýþÿ',
    'US/JP': '、。，．・：；？！゛゜´｀¨＾￣＿ヽヾゝゞ〃仝々〆〇ー―‐／＼～∥｜…‥‘’“”（）〔〕［］｛｝〈〉《》「」『』【】＋－±×÷＝≠＜＞≦≧∞∴♂♀°′″℃￥＄￠￡％＃＆＊＠§☆★○●◎◇◆□■△▲▽▼※〒→←↑↓〓∈∋⊆⊇⊂⊃∪∩∧∨￢⇒⇔∀∃∠⊥⌒∂∇≡≒≪≫√∽∝∵∫∬Å‰♯♭♪†‡¶◯ΑΒΓΔΕΖΗΘΙΚΛΜΝΞΟΠΡΣΤΥΦΧΨΩαβγδεζηθικλμνξοπρστυφχψωАБВГДЕЁЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯабвгдеёжзийклмнопрстуфхцчшщъыьэюя│┌┐┘└├┬┤┴┼━┃┏┓┛┗┣┳┫┻╋┠┯┨┷┿┝┰┥┸╂①②③④⑤⑥⑦⑧⑨⑩⑪⑫⑬⑭⑮⑯⑰⑱⑲⑳ⅠⅡⅢⅣⅤⅥⅦⅧⅨⅩⅰⅱⅲⅳⅴⅵⅶⅷⅸⅹ㍉㌔㌢㍍㌘㌧㌃㌶㍑㍗㌍㌦㌣㌫㍊㌻㎜㎝㎞㎎㎏㏄㎡㍻〝〟№㏍℡㊤㊥㊦㊧㊨㈱㈲㈹㍾㍽㍼∮∑∟⊿￤＇＂'
}
button_width = 2
button_pad = 2
recent_num = 10
char_pad = 3

font = 'Arial 12'
sp_char_cols = 16
sp_char_rows_vis = 10

sel_halo = 2


class SpecialCharSelectWidget(ttk.Frame):

    def __init__(self, master, insert_callback: Callable, recents=None, theme=None, cur_enc=None, location=None, button_num=recent_num, *args, **kwargs):
        super().__init__(master, *args, **kwargs)

        self.theme = theme

        self.recents = recents if recents is not None else []
        if len(self.recents) >= button_num:
            self.recents = self.recents[:button_num]

        self.cur_enc: Literal['EU', 'US/JP'] = cur_enc if cur_enc is not None else 'US/JP'
        self.insert_callback = insert_callback

        self.expand_button = ttk.Button(self, text='Other chars...', command=lambda: self.toggle_widget(location=location))
        self.expand_button.grid(row=0, column=0, padx=button_pad)
        self.widget_is_expanded = False

        self.recent_buttons = []
        i = 0
        for char in self.recents:
            if i >= button_num:
                break
            b = ttk.Button(self, text=char, command=lambda ind=i: self.select_recent(ind), width=button_width)
            b.grid(row=0, column=i+1, padx=button_pad)
            self.recent_buttons.append(b)
            i += 1

        while i < button_num:
            b = ttk.Button(self, text='', state='disabled', width=button_width)
            b.grid(row=0, column=i+1, padx=button_pad)
            self.recent_buttons.append(b)
            i += 1

        self.char_widgets: Dict[str, ScrollCanvas] = {'EU': ScrollCanvas(master, size={'width': 0, 'height': 0}, theme=theme),
                                                      'US/JP': ScrollCanvas(master, size={'width': 0, 'height': 0}, theme=theme)}

        self.insert_callback = insert_callback

        self.populate_chars(is_init=True)
        for w in self.char_widgets.values():
            w.canvas.tag_bind('char', '<ButtonRelease-1>', self.select_char)

    def populate_chars(self, is_init=False):
        if not is_init:
            for w in self.char_widgets.values():
                w.delete_all()

        txt_fill = 'black' if self.theme is None else self.theme['TCanvasText']['configure']['fill']

        max_dim = 0
        char_ids = {}
        for key, chars in sp_chars.items():
            char_ids[key] = []
            i = 0
            while i < len(chars):
                new_id = self.char_widgets[key].canvas.create_text(0, 0, text=chars[i], font=font,
                                                                   tags=(chars[i], 'char'), fill=txt_fill)
                char_ids[key].append(new_id)
                bbox = self.char_widgets[key].canvas.bbox(new_id)
                max_dim = max(max_dim, bbox[2] - bbox[0], bbox[3] - bbox[1])
                i += 1

        max_dim += char_pad
        if max_dim % 2 == 0:
            max_dim += 1

        center_dim = max_dim // 2 + 1

        for key, txt_ids in char_ids.items():
            i = 0
            while i < len(txt_ids):
                self.char_widgets[key].canvas.moveto(txt_ids[i], max_dim * (i % sp_char_cols) + center_dim,
                                                     max_dim * (i // sp_char_cols) + center_dim)
                i += 1
            self.char_widgets[key].set_size(max_dim * sp_char_cols, max_dim * sp_char_rows_vis)

    def toggle_widget(self, location=None):
        if self.widget_is_expanded:
            self.contract_widget()
        else:
            self._expand_widget(location=location)

    def _expand_widget(self, location=None):
        x = self.winfo_x()
        y = self.winfo_y()+self.winfo_height()
        anchor = tk.NW
        if location is not None:
            if location == 'above':
                y -= self.winfo_height()
                anchor = tk.SW
        self.char_widgets[self.cur_enc].place(x=x, y=y, anchor=anchor)
        self.widget_is_expanded = True

    def contract_widget(self):
        if not self.widget_is_expanded:
            return
        self.char_widgets[self.cur_enc].place_forget()
        self.widget_is_expanded = False

    def select_char(self, e):
        y_canv = e.widget.canvasy(e.y)
        char_text = e.widget.find_closest(e.x, y_canv, halo=sel_halo)[0]
        char = e.widget.gettags(char_text)[0]
        self.insert_callback(char)
        self.contract_widget()
        self.update_recents(char)

    def update_recents(self, char=None):
        if char is not None:
            if char in self.recents:
                self.recents.remove(char)
            self.recents.insert(0, char)
            if len(self.recents) > len(self.recent_buttons):
                self.recents.pop(-1)
        for i, c in enumerate(self.recents):
            self.recent_buttons[i].configure(text=c)

    def select_recent(self, ind):
        self.insert_callback(self.recents[ind])

    def set_encoding(self, enc):
        self.cur_enc = enc

    def set_state(self, state):
        self.expand_button.configure(state=state)
        for i in range(len(self.recents)):
            self.recent_buttons[i].configure(state=state)

    def change_theme(self, theme):
        self.theme = theme
        for c in self.char_widgets.values():
            c.change_theme(theme)

        self.populate_chars()


if __name__ == '__main__':
    os.chdir('./../../../')
    w = tk.Tk()
    w.geometry('500x500')

    sp_char_widget1 = SpecialCharSelectWidget(w, lambda s: print(f'Selected {s}'), )
    sp_char_widget1.grid(row=0, column=0, sticky=tk.NW)
    sp_char_widget2 = SpecialCharSelectWidget(w, lambda s: print(f'Selected {s}'), location='above')
    sp_char_widget2.grid(row=1, column=0, sticky=tk.SW)
    w.bind('<Escape>', lambda e: sp_char_widget1.contract_widget())
    w.bind('<Escape>', lambda e: sp_char_widget2.contract_widget(), add='+')

    w.rowconfigure(1, weight=1)

    w.mainloop()
