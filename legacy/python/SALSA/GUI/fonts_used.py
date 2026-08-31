
class SALSAFont:
    default_family = 'Helvetica'
    default_size = 10
    default_suffix = ''
    default_select_suffix = 'underline'
    default_font: str
    hover_font: str

    def __init__(self):
        self.set_font()

    def set_font(self, family=None, size=None, suffix=None, select_suffix=None):
        self.default_family = family if family is not None else self.default_family
        self.default_size = size if size is not None else self.default_size
        self.default_suffix = suffix if suffix is not None else self.default_suffix
        self.default_select_suffix = select_suffix if select_suffix is not None else self.default_select_suffix
        self.default_font = f'{self.default_family} {self.default_size} {self.default_suffix}'
        self.hover_font = f'{self.default_family} {self.default_size} {self.default_select_suffix}'

