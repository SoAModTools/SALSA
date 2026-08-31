import colorsys
import math
import tkinter as tk
from copy import copy
from typing import Literal, Tuple, List

text_pad = 10
w_pad = 4
slider_pad = 1
slider_animation_frames = 10
slider_animation_duration = 75

default_theme = {
    "ToggleButton.Canvas": {
        "configure": {
            "background": 'SystemButtonFace'
        }
    },
    "ToggleButton.Background": {
        "configure": {
            "fill": "gray90",
            'outline': "blue",
            'width': 2
        }
    },
    "ToggleButton.Text": {
        "configure": {
            "fill": "black",
            "font": 'Helvetica 50'
        }
    },
    "ToggleButton.Slider": {
        "configure": {
            "fill": "gray60"
        }
    }
}


class ToggleButton(tk.Canvas):

    def __init__(self, master, on_text, off_text, *args, on_color=None, off_color=None, theme=None, command=None,
                 start_state: Literal['on', 'off'] = 'on', **kwargs):
        super().__init__(master, *args, borderwidth=0, **kwargs)
        self.bg_color = self['bg']
        self.toggle_callback = command

        self.text_values = {'on': on_text, 'off': off_text}
        self.state_keys = {v: k for k, v in self.text_values.items()}

        self.theme = theme
        if theme is None:
            self.theme = default_theme

        self.on_color = on_color
        self.off_color = off_color

        self.colors = {'on': self.theme["ToggleButton.Background"]["configure"]["fill"],
                       'off': self.theme["ToggleButton.Background"]["configure"]["fill"]}
        if self.on_color is not None:
            self.on_color = self.colors['on'] = on_color
        if self.off_color is not None:
            self.off_color = self.colors['off'] = off_color

        self.on_text = self.create_text(w_pad + text_pad, w_pad, text=on_text, anchor=tk.NW,
                                        **self.theme['ToggleButton.Text']['configure'])
        self.off_text = self.create_text(w_pad + text_pad + self.bbox(self.on_text)[2], w_pad,
                                         text=off_text, anchor=tk.NW, **self.theme['ToggleButton.Text']['configure'])

        self.w_height = self.bbox(self.off_text)[3] - w_pad
        self.w_width = self.bbox(self.off_text)[2] + text_pad - w_pad
        bg_pts = get_stretched_circle_polygon_points(w_pad, w_pad, self.w_height, self.w_width)
        self.bg = self.create_polygon(*bg_pts, **self.theme['ToggleButton.Background']['configure'])
        self.itemconfigure(self.bg, fill=self.colors[start_state])

        self.tag_raise(self.on_text)
        self.tag_raise(self.off_text)

        rgb: Tuple[int, int, int] = self.winfo_rgb(self.bg_color)
        rgb = (rgb[0] >> 8, rgb[1] >> 8, rgb[2] >> 8)

        # slider_background = '#'+''.join([hex(c)[2:] if len(hex(c)[2:]) == 2 else '0'+hex(c)[2:]
        #                                  for c in get_lighter_color(rgb, lightness_increment=.05)])
        self.slider_state: Literal['on', 'off'] = start_state
        slider_start_ind = 0 if start_state == 'off' else -1
        self.slider_polygons = get_slider_polygons(self.bbox(self.on_text), self.bbox(self.off_text),
                                                   self.w_height, slider_animation_frames, y_pad=slider_pad, x_pad=-5)

        self.slider_anim_inds = {
            'on': [i for i in range(1, slider_animation_frames)],
            'off': [i for i in reversed(range(0, slider_animation_frames - 1))]
        }
        self._in_toggle = False

        self.slider = self.create_polygon(*self.slider_polygons[slider_start_ind], **self.theme['ToggleButton.Slider']['configure'])

        self.addtag_all('toggle')

        self.tag_bind('toggle', sequence='<ButtonRelease-1>', func=self.start_toggle)
        self._widget_state = 'normal'

        canvas_size = self.bbox('all')
        self.configure(width=canvas_size[2]-1, height=canvas_size[3]-1, **self.theme['ToggleButton.Canvas']['configure'])

    def set_widget_state(self, state):
        if state == self._widget_state:
            return
        self._widget_state = state
        if state == 'normal':
            self.tag_bind('toggle', sequence='<ButtonRelease-1>', func=self.start_toggle)
        if state == 'disabled':
            self.tag_unbind('toggle', sequence='<ButtonRelease-1>')


    def start_toggle(self, e):
        if self._in_toggle:
            return
        self.slider_state = 'off' if self.slider_state == 'on' else 'on'
        if self.toggle_callback is not None:
            self.toggle_callback(self.text_values[self.slider_state])
        self.itemconfigure(self.bg, fill=self.colors[self.slider_state])
        self._toggle_step(copy(self.slider_anim_inds[self.slider_state]), slider_animation_duration//slider_animation_frames)

    def _toggle_step(self, inds_remaining, wait_time):
        cur_ind = inds_remaining.pop(0)
        self.delete(self.slider)
        self.slider = self.create_polygon(*self.slider_polygons[cur_ind], **self.theme['ToggleButton.Slider']['configure'])
        if len(inds_remaining) > 0:
            self.after(wait_time, self._toggle_step, inds_remaining, wait_time)
        else:
            self._in_toggle = False
            self.addtag_withtag('toggle', self.slider)

    def set_state(self, state: Literal['on', 'off']):
        self._toggle_step([self.slider_anim_inds[state][-1]], 0)
        self.slider_state = state

    def set_state_by_value(self, key: str):
        if key not in self.state_keys:
            return
        self.set_state(self.state_keys[key])

    def get(self):
        return 0 if self.slider_state == 'on' else 1

    def change_theme(self, new_theme):
        self.theme = new_theme
        self.itemconfigure(self.on_text, **self.theme['ToggleButton.Text']['configure'])
        self.itemconfigure(self.off_text, **self.theme['ToggleButton.Text']['configure'])
        self.itemconfigure(self.bg, **self.theme['ToggleButton.Background']['configure'])
        self.itemconfigure(self.slider, **self.theme['ToggleButton.Slider']['configure'])
        self.configure(**self.theme['ToggleButton.Canvas']['configure'])
        if self.on_color is None:
            self.colors['on'] = self.theme["ToggleButton.Background"]["configure"]["fill"]
        if self.off_color is None:
            self.colors['off'] = self.theme["ToggleButton.Background"]["configure"]["fill"]


def get_lighter_color(rgb: Tuple[int, int, int], lightness_increment=0.1) -> Tuple[int, int, int]:
    hsv = colorsys.rgb_to_hsv(rgb[0]/255, rgb[1]/255, rgb[2]/255)
    rgb = colorsys.hsv_to_rgb(hsv[0], hsv[1], min(hsv[2] + lightness_increment, 1))
    return math.floor(rgb[0]*255), math.floor(rgb[1]*255), math.floor(rgb[2]*255)


def get_color_str_from_tuple(rgb: Tuple[int, int, int], bit_depth_reduction=0) -> str:
    rgb = [hex(c >> bit_depth_reduction)[2:]
           if len(hex(c >> bit_depth_reduction)[2:]) == 2
           else '0'+hex(c >> bit_depth_reduction)[2:]
           for c in rgb]
    return '#' + ''.join(rgb)


def get_rgb_from_str(rgb_str: str) -> [Tuple[int, int, int], int]:
    color_size = (len(rgb_str)-1)/3
    if int(color_size) != color_size or rgb_str[0] != '#':
        raise ValueError(f'Incorrect format: should be a string with prefix # and r, g, and b should be of equal size. {rgb_str} was given')
    color_size = int(color_size)
    rgb_str = rgb_str[1:]
    return (int(rgb_str[:color_size], 16), int(rgb_str[color_size:color_size * 2], 16), int(rgb_str[color_size * 2:color_size * 3], 16)), color_size*4


def create_color_animation_frames(start_color: str, end_color: str, frames: int) -> List[str]:
    c1, depth1 = get_rgb_from_str(start_color)
    c2, depth2 = get_rgb_from_str(end_color)
    if depth1 != depth2:
        raise ValueError(f'Color 1 and 2 should be the same bit depth: d1: {depth1}, d2: {depth2}')
    color_size = 2**depth1-1
    c1 = [c/color_size for c in c1]
    c2 = [c/color_size for c in c2]
    c1 = colorsys.rgb_to_hsv(*c1)
    c2 = colorsys.rgb_to_hsv(*c2)

    color_anim = [start_color]
    for i in range(1, frames):
        new_hsv = [c1[j] + ((c2[j] - c1[j])*(i/frames)) for j in range(len(c1))]
        new_rgb_floats = colorsys.hsv_to_rgb(*new_hsv)
        new_rgb = (int(new_rgb_floats[0]*color_size),
                   int(new_rgb_floats[1]*color_size),
                   int(new_rgb_floats[2]*color_size))
        color_anim.append(get_color_str_from_tuple(new_rgb))
    color_anim.append(end_color)
    return color_anim


def get_stretched_circle_polygon_points(origin_x, origin_y, height, width):
    polygon_pts = []

    c1_x = int(origin_x+height/2)
    c1_y = int(origin_y+height/2)
    c_r = height/2
    if int(c_r) != c_r:
        c1_x += 1
        c1_y += 1

    # Generate first arc
    for d in range(90, 271):
        cur_xy = (int(c_r*math.cos(d*(math.pi/180))+c1_x), int(c_r*math.sin(d*(math.pi/180))+c1_y))
        if cur_xy not in polygon_pts:
            polygon_pts.append(cur_xy)

    c2_x = int(origin_x + width - height/2)
    c2_y = int(origin_y + height/2)
    if int(c_r) != c_r:
        c2_x += 1
        c2_y += 1

    # Generate second arc
    for d in range(270, 450):
        if d > 360:
            d -= 360
        cur_xy = (int(c_r*math.cos(d*(math.pi/180))+c2_x), int(c_r*math.sin(d*(math.pi/180))+c2_y))
        if cur_xy not in polygon_pts:
            polygon_pts.append(cur_xy)

    return polygon_pts


def get_slider_polygons(bbox1, bbox2, height, frames, x_pad=-5, y_pad=2):

    width1 = bbox1[2] - bbox1[0]
    width2 = bbox2[2] - bbox2[0]
    x1 = bbox1[0]
    x2 = bbox2[0]
    y = bbox1[1]

    # Adjust for padding, add width, subtract height
    height -= 2*y_pad
    y += y_pad
    width1 -= 2*x_pad
    width2 -= 2*x_pad
    x1 += x_pad
    x2 += x_pad

    sliders = []
    for i in range(frames):
        cur_x = x1 + int((x2 - x1)*(i/(frames-1)))
        cur_width = width1 + int((width2 - width1)*(i/(frames-1)))
        sliders.append(get_stretched_circle_polygon_points(cur_x, y, height, cur_width))

    return sliders


if __name__ == '__main__':
    window = tk.Tk()
    toggle_frame = tk.Frame(window, highlightbackground='black', highlightthickness=1)
    toggle_frame.pack()
    toggle_button = ToggleButton(toggle_frame, 'US/JP', 'EU', on_color='#22cc00', off_color='#cc2200',
                                 start_state='on')
    toggle_button.pack()

    state_button_on = tk.Button(window, text='Set On', command=lambda: toggle_button.set_state('on'))
    state_button_on.pack()
    state_button_off = tk.Button(window, text='Set Off', command=lambda: toggle_button.set_state('off'))
    state_button_off.pack()

    window.mainloop()
