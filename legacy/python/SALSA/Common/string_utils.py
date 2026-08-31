def get_padding_for_number(i: int, length: int, pad_char: str):
    i_str = str(i)
    if len(i_str) >= length:
        return i_str
    return pad_char * (length - len(i_str))
