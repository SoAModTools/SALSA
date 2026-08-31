script_dir1 = './../../_script_files/_DC_US_compressed_scripts'
script_dir2 = './../../_script_files/_DC_JP_compressed_scripts'

if __name__ == '__main__':
    import os
    cur_dir = os.path.dirname(__file__)
    os.chdir(cur_dir)
    os.chdir(os.path.pardir)
    os.chdir(os.path.pardir)
    os.chdir(os.path.pardir)
    from SALSA.FileModels.sct_model import SCTModel
    from SALSA.BaseInstructions.bi_facade import BaseInstLibFacade
    os.chdir(cur_dir)
    os.chdir(os.path.pardir)

    baseinsts = BaseInstLibFacade()

    sct_model = SCTModel()

    files = os.listdir(script_dir1)
    headers = {}

    # num = 5
    # i = 0
    for f in files:
        # if i >= num:
        #     continue
        # i += 1
        print(f'working on {f}')

        file_dir = script_dir1
        filepath = os.path.join(file_dir, f)
        name, original_ba = sct_model.read_sct_file(filepath=filepath)
        name = name.split(os.sep)[-1]
        ba: bytearray = original_ba
        h1 = ba[:8].hex(sep=' ', bytes_per_sep=2)

        file_dir = script_dir2
        filepath = os.path.join(file_dir, f)
        if not os.path.exists(filepath):
            filepath = os.path.join(file_dir, f.lower())
        if not os.path.exists(filepath):
            continue
        name, original_ba = sct_model.read_sct_file(filepath=filepath)
        name = name.split(os.sep)[-1]
        ba: bytearray = original_ba
        h2 = ba[:8].hex(sep=' ', bytes_per_sep=2)

        headers[name] = f'"{h1}","{h2}"'

    out_path = f'./test_files/headers.csv'
    out_str = '"Script Name","DC Header","GC Header"'
    for n, d in headers.items():
        out_str += f'\n{n},{d}'

    with open(out_path, 'w') as fh:
        fh.write(out_str)
