string_replace_list = [
    ('\\h', ''),
    ('\\e', ''),
    ('\\c', ''),
    ('\\n', '\n'),
    ('', ' '),
    # ('　', ''),
]

blank_string = '\\h(　)\\e'


def SAstr_to_visible(string):
    for item in string_replace_list:
        string = string.replace(item[0], item[1])
    return string


def visible_to_SAstr(string):
    for item in string_replace_list:
        if item[1] == '':
            continue
        string = string.replace(item[1], item[0])
    return string


def SAstr_to_head_and_body(string):
    head_end = -1
    no_head = False
    if '\\h' in string:
        head_start = string.find('(') + 1
        head_end = string.find(')')
        temp_head = string[head_start:head_end]
        if len(temp_head) == 0:
            no_head = True
            head = temp_head
        else:
            no_head = False
            head = SAstr_to_visible(temp_head)
    else:
        head = None
    body = SAstr_to_visible(string[head_end+1:])
    return no_head, head, body


def head_and_body_to_SAstr(no_head, head, body):
    body = visible_to_SAstr(body)
    if no_head:
        head = ''
    elif head is not None:
        head = '　' if head == '' else visible_to_SAstr(head)
    else:
        return f'{body}\\e'
    return f'\\h({head}){body}\\e'


decoding_errors = {'cp1252': [('\\x81', ' ')],
                   'shiftjis': [('\\x85', '…')]}


def fix_string_decoding_errors(string, encoding):
    for error in decoding_errors[encoding]:
        string = string.replace(error[0], error[1])
    return string


encoding_errors = {'cp1252': [],
                   'shiftjis': [(b'\x81\x63', b'\x85')]}


def fix_string_encoding_errors(str_bytes: bytearray, encoding):
    for error in encoding_errors[encoding]:
        str_bytes = str_bytes.replace(error[0], error[1])
    return str_bytes
