import dataclasses
import tkinter


@dataclasses.dataclass
class Colors:
    bg: str
    bg_highlight: str
    bg_internal: str
    text: str
    selector: str
    bright_selector: str
    selector_text: str
    insert_indicator: str
    border: str
    button: str
    error_button: str
    disabled_button: str
    link: str
    warning_text: str
    disabled_text: str
    success_text: str
    dim_success_text: str


def theme_generator(colors: Colors):
    return {
        ".": {
            "configure": {
                "background": colors.bg,
                "foreground": colors.text
            }
        },
        "tooltip.TFrame": {
            "configure": {
                "background": colors.bg_highlight,
            }
        },
        "TLabelframe": {
            "configure": {
                "highlightbackground": colors.bg_highlight,
                "highlightcolor": colors.bg_highlight,
                "highlightthickness": 2,
                "background": colors.bg,
                "bordercolor": colors.bg_highlight,
                "lightcolor": colors.bg_highlight,
                "darkcolor": colors.bg_internal
            }
        },
        "flash.TLabelframe": {
            "configure": {
                "highlightbackground": colors.bg_highlight,
                "highlightcolor": colors.bg_highlight,
                "highlightthickness": 2,
                "background": colors.bg_highlight,
                "bordercolor": colors.bg_highlight,
                "lightcolor": colors.bg_highlight,
                "darkcolor": colors.bg_internal
            }
        },
        "TLabel": {
            "configure": {
                "background": colors.bg,
                "foreground": colors.text
            }
        },
        "flash.TLabel": {
            "configure": {
                "background": colors.bg_highlight,
                "foreground": colors.text
            }
        },
        "canvas.TLabel": {
            "configure": {
                "background": colors.bg_internal,
                "foreground": colors.text
            }
        },
        "link.TLabel": {
            "configure": {
                "background": colors.bg_internal,
                "foreground": colors.link
            }
        },
        "warning.TLabel": {
            "configure": {
                "background": colors.bg,
                "foreground": colors.warning_text
            }
        },
        "success.TLabel": {
            "configure": {
                "background": colors.bg,
                "foreground": colors.success_text
            }
        },
        "dim_success.TLabel": {
            "configure": {
                "background": colors.bg,
                "foreground": colors.dim_success_text
            }
        },
        "TButton": {
            "configure": {
                "background": colors.button,
                "foreground": colors.text
            },
            "map": {
                "background": [('disabled', colors.disabled_button)],
                "foreground": [('disabled', colors.disabled_text)]
            }
        },
        "error.TButton": {
            "configure": {
                "background": colors.error_button,
                "foreground": colors.text
            }
        },
        "TEntry": {
            "configure": {
                "background": colors.bg_internal,
                "foreground": colors.text,
                "highlightbackground": colors.bg,
                "highlightcolor": colors.bg,
                "fieldbackground": colors.bg_internal,
                "lightcolor": colors.border,
                "selectbackground": colors.bright_selector,
                "selectforeground": colors.selector_text,
                "insertcolor": colors.insert_indicator,
            },
        },
        "tooltip.TEntry": {
            "configure": {
                "background": colors.bg_internal,
                "foreground": colors.disabled_text,
                "fieldbackground": colors.bg_internal,
                "lightcolor": colors.border,
            },
        },
        "TCheckbutton": {
            "configure": {
                "background": colors.bg,
                "foreground": colors.text,  # White text
                "indicatorbackground": colors.bg_internal,
                "indicatorforeground": colors.text,
            },
            "map": {
                "background": [('disabled', colors.bg)],
                "foreground": [('disabled', colors.disabled_text)],
                "indicatorbackground": [('disabled', colors.bg_internal)],
                "indicatorforeground": [('disabled', colors.disabled_text)]
            }
        },
        "TCombobox": {
            "configure": {
                "background": colors.bg,  # Dark grey background
                "foreground": colors.text,  # White text
                "fieldbackground": colors.bg_highlight,
                "insertcolor": colors.text,
                "bordercolor": colors.border,
                "lightcolor": colors.bg_highlight,
                "darkcolor": colors.bg_internal,
                "arrowcolor": colors.text
            },
        },
        "TSpinbox": {
            "configure": {
                "background": colors.bg,  # Dark grey background
                "foreground": colors.text,  # White text
                "fieldbackground": colors.bg_highlight,
                "insertcolor": colors.text,
                "bordercolor": colors.border,
                "lightcolor": colors.bg_highlight,
                "darkcolor": colors.bg_internal,
                "arrowcolor": colors.text
            },
        },
        # Treeview configuration for the next three items
        "Treeview": {
            "configure": {
                "background": colors.bg_internal,
                "foreground": colors.text,
                "fieldbackground": colors.bg_internal,
                "lightcolor": colors.border,
                "darkcolor": colors.bg_internal
            },
            "map": {
                "background": [('selected', colors.selector)],
                "foreground": [('selected', colors.selector_text)]
            }
        },
        "Heading": {
            "configure": {
                "background": colors.bg_highlight,
                "foreground": colors.text,
                "borderwidth": 3
            }
        },
        "Item": {
            "configure": {
                "foreground": colors.link,
                "focuscolor": colors.selector
            }
        },
        "text": {
            "configure": {
                "background": colors.bg_internal,
                "foreground": colors.text,
                "insertbackground": colors.insert_indicator,
                "selectbackground": colors.selector,
                "selectforeground": colors.selector_text
            }
        },
        # panedwindow configurations
        "Sash": {
            "configure": {
                "background": colors.bg,
                "lightcolor": colors.bg_highlight,
                "bordercolor": colors.bg_highlight,
                "sashthickness": 8,
                "handlesize": 10
            }
        },
        "TScrollbar": {
            "configure": {
                "background": colors.bg_highlight,
                "troughcolor": colors.bg,
                "arrowcolor": colors.text,
                "darkcolor": colors.bg_internal,
                "lightcolor": colors.bg_highlight
            }
        },
        "SALSAMenu": {
            'configure': {
                "background": colors.bg,
            }
        },
        "SALSAMenuBar.TFrame": {
            'configure': {
                "background": colors.bg,
            }
        },
        "SALSAMenu.TLabel": {
            'configure': {
                "background": colors.bg,
                "foreground": colors.text,
                "activebackground": colors.selector,
                "activeforeground": colors.text,
            }
        },
        "SALSAMenu.TCheckbutton": {
            'configure': {
                "background": colors.bg,
                "foreground": colors.text,
                "activebackground": colors.selector,
                "activeforeground": colors.text,
            }
        },
        "SALSAMenuBar.TLabel": {
            'configure': {
                "background": colors.bg_highlight,
                "foreground": colors.text,
                "activebackground": colors.selector,
                "activeforeground": colors.text,
            }
        },
        "canvas.TFrame": {
            'configure': {
                "background": colors.bg_internal,
            }
        },
        "light.TCanvas": {
            'configure': {
                "background": colors.bg,
            }
        },
        "TCanvas": {
            'configure': {
                "background": colors.bg_internal,
                "highlightbackground": colors.border,
                "highlightcolor": colors.border
            }
        },
        "Tmessage": {
            'configure': {
                "background": colors.bg_internal,
                "foreground": colors.text,
                "highlightbackground": colors.border,
                "highlightcolor": colors.border
            }
        },
        "TCanvasText": {
            'configure': {
                "fill": colors.text
            }
        },
        "Ttoplevel": {
            "configure": {
                "background": colors.bg,
                "highlightbackground": colors.bg_highlight,
                "highlightcolor": colors.bg_highlight,
                "highlightthickness": 2,
            }
        },
        "drag.Ttoplevel": {
            "configure": {
                "background": colors.bg,
                "highlightbackground": colors.bg,
                "highlightcolor": colors.bg,
                "highlightthickness": 0,
            }
        },
        # Treeview configuration for the next three items
        "drag.Treeview": {
            "configure": {
                "background": colors.bg_internal,
                "foreground": colors.text,
                "fieldbackground": colors.bg_internal,
                "lightcolor": colors.bg_internal,
                "darkcolor": colors.bg_internal
            },
        },
        "ToggleButton.Canvas": {
            "configure": {
                "background": colors.bg
            }
        },
        "ToggleButton.Background": {
            "configure": {
                "fill": colors.selector,
                'outline': colors.border,
                'width': 2
            }
        },
        "ToggleButton.Text": {
            "configure": {
                "fill": colors.selector_text
            }
        },
        "ToggleButton.Slider": {
            "configure": {
                "fill": colors.bg_highlight
            }
        }
    }


themes = {
    'dark': theme_generator(Colors(
        bg="#2d2d2d",
        bg_internal='#202020',
        bg_highlight="#4d4d4d",
        text="#bbbbbb",
        selector='#333366',
        bright_selector='#6666cc',
        selector_text="#cccccc",
        insert_indicator='#cccccc',
        border='#5d5d5d',
        button="#3c4f4f",
        error_button="#5c2f2f",
        disabled_button="#3c3c3c",
        link='#5588ff',
        warning_text='#ee4444',
        disabled_text='#777777',
        success_text='#39bb39',
        dim_success_text='#339933'
    )),
    'light': theme_generator(Colors(
        bg='#eeeeee',
        bg_internal='white',
        bg_highlight='#dddddd',
        text='#000000',
        selector='#4444dd',
        bright_selector='#3333bb',
        selector_text='white',
        insert_indicator='black',
        border='#888888',
        button="#acafd1",
        error_button="#dcafa1",
        disabled_button="#9c9c9c",
        link='#0055ee',
        warning_text='#dd1122',
        disabled_text='#eeeeee',
        success_text='#11dd22',
        dim_success_text='#11aa22'
    )),
}

dark_theme = themes['dark']
light_theme = themes['light']

theme_non_color_maps = {
    'Heading': {
        "relief": [('active', 'groove'), ('pressed', 'sunken')]
    }
}
