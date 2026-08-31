# This file tests decoding and encoding of sct files.
from typing import Literal

# Requires a script directory to run
script_dir = './../../_script_files/_EU_compressed_scripts'
eu_validation = True if 'EU' in script_dir else False
endian: Literal['big', 'little'] = 'big'
if 'DC' in script_dir:
    endian = 'little'

diff_suffix = '_US'
if 'EU' in script_dir:
    diff_suffix = '_EU'
if 'JP' in script_dir:
    diff_suffix = '_JP'

if 'DC' in script_dir:
    diff_suffix += '_DC'
else:
    diff_suffix += '_GC'

# Determines whether compressed or decompressed scripts are checked
check_compressed = False

# All files between the first and last file names will be checked.
# If first_file == None, starts at beginning
# If last_file == None, goes till end

# first_file = None
first_file = 'ME017B.sct'
# last_file = None
last_file = 'ME017B.sct'

# Determines whether files are exported if there is a difference
file_out = True

hex_equivalencies = {
    '00000010': '00000006',
    '00000006': '00000010',
    '00000011': '00000007',
    '00000007': '00000011',
    '00000012': '0000000b',
    '0000000b': '00000012',
    '00000013': '0000000c',
    '0000000c': '00000013',
    '00000014': '0000000d',
    '0000000d': '00000014',
    '00000015': '0000000e',
    '0000000e': '00000015',
    '00000016': '0000000f',
    '0000000f': '00000016',
}


def compare_files(ba_1, ba_2, cur_endian='big'):
    d = {}
    if len(ba_1) != len(ba_2):
        d['length'] = f'Original is {len(ba_1)} bytes long, Encoded is {len(ba_2)} bytes long'
    else:
        if cur_endian == 'big':
            hex_eq = hex_equivalencies
        else:
            hex_eq = {}
            for k, v in hex_equivalencies.items():
                k = bytearray(reversed(bytearray.fromhex(k))).hex()
                v = bytearray(reversed(bytearray.fromhex(v))).hex()
                hex_eq[k] = v

        # Starts from index 2 to ignore the header which is probably versioning information
        for word in range(2, len(ba_1) // 4 + 1):
            original_word = ba_1[word * 4: word * 4 + 4]
            encoded_word = ba_2[word * 4: word * 4 + 4]

            # check that the words are the same
            if original_word.hex() == encoded_word.hex():
                continue

            # check that the endian hasn't changed
            if bytearray(reversed(original_word)).hex() == encoded_word.hex():
                continue

            # check that a different parameter code hasn't been used
            if original_word.hex() in hex_eq:
                if encoded_word.hex() == hex_eq[original_word.hex()]:
                    continue

            orig_hex = original_word.hex()
            enc_hex = encoded_word.hex()
            if 'DC' in script_dir:
                orig_hex += f' ({bytearray(reversed(original_word)).hex()})'
                enc_hex += f' ({bytearray(reversed(encoded_word)).hex()})'
            d[word * 4] = f'Original is {orig_hex}, Encoded has {enc_hex}'
    return d


def save_diffs(d_dict, path):
    out_str = '"Script Name","Difference Pos","Difference Hex","Difference"'
    for n, d in d_dict.items():
        for diff_name, diff_str in d.items():
            out_str += f'\n{n},{diff_name}'
            out_str += f',{hex(int(diff_name))}' if diff_name != 'length' else ','
            out_str += f',{diff_str}'

    with open(path, 'w') as fh:
        fh.write(out_str)


if __name__ == '__main__':
    import os
    cur_dir = os.path.dirname(__file__)
    os.chdir(cur_dir)
    os.chdir(os.path.pardir)
    os.chdir(os.path.pardir)
    os.chdir(os.path.pardir)
    from SALSA.FileModels.sct_model import SCTModel
    from SALSA.BaseInstructions.bi_facade import BaseInstLibFacade
    from SALSA.Scripts.script_decoder import SCTDecoder
    from SALSA.Scripts.script_encoder import SCTEncoder
    from SALSA.AKLZ.aklz import Aklz
    os.chdir(cur_dir)
    os.chdir(os.path.pardir)

    baseinsts = BaseInstLibFacade()

    sct_model = SCTModel()

    files = os.listdir(script_dir)

    differences = {}
    skip_till = first_file if first_file is not None else 'me002a.sct'
    last_file = last_file if last_file is not None else files[-1]
    skip = True
    for f in files:
        if f.lower() == skip_till.lower():
            skip = False

        if skip:
            continue

        file_dir = script_dir
        filepath = os.path.join(file_dir, f)
        name, original_ba = sct_model.read_sct_file(filepath=filepath)
        name = name.split(os.sep)[-1]

        print(f'Starting decode for {f.split(".")[0]}')
        script = SCTDecoder.decode_sct_from_file(f.split('.')[0], sct=original_ba, inst_lib=baseinsts, is_validation=True)

        print(f'Starting re-encode for {f.split(".")[0]}')
        encoded_ba = SCTEncoder.encode_sct_file_from_project_script(project_script=script, base_insts=baseinsts,
                                                                    use_garbage=True, combine_footer_links=False,
                                                                    add_spurious_refresh=True, validation=True,
                                                                    eu_validation=eu_validation, endian=endian)
        if check_compressed:
            aklz = Aklz()
            original_ba = aklz.compress(original_ba)
            aklz = Aklz()
            encoded_ba = aklz.compress(encoded_ba)

        diffs = compare_files(original_ba, encoded_ba, cur_endian=endian)
        if len(diffs) != 0:
            differences[name] = diffs

        if name in differences and file_out:
            filepath_enc = f'./test_files/re-encodes/{name}_out.sct'
            filepath_orig = f'./test_files/originals/{name}_out.sct'
            sct_model.save_sct_file(filepath=filepath_enc, sct_file=encoded_ba, compress=check_compressed)
            sct_model.save_sct_file(filepath=filepath_orig, sct_file=original_ba, compress=check_compressed)
            # break
        else:
            print(f'No Differences for {name}.sct')

        if f.lower() == last_file.lower():
            break

    out_path = f'./test_files/diffs{diff_suffix}.csv'
    save_diffs(differences, out_path)
    print(f'Differences saved to {out_path}')
