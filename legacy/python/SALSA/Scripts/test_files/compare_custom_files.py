# This file allows for tests between two specific files in the same directory
# Set file names and directory below

first_file = 'me002a.sct'
second_file = 'me002a.out.sct'
directory = 'D:\\SALSA projects\\exported'

if __name__ == '__main__':
    import os
    cur_dir = os.path.dirname(__file__)
    os.chdir(os.path.pardir)
    os.chdir(os.path.pardir)
    os.chdir(os.path.pardir)
    from SALSA.FileModels.sct_model import SCTModel
    from SALSA.BaseInstructions.bi_facade import BaseInstLibFacade
    from SALSA.Scripts.test_files.decode_encode_test import compare_files, save_diffs
    os.chdir(cur_dir)
    os.chdir(os.path.pardir)

    baseinsts = BaseInstLibFacade()

    sct_model = SCTModel()

    filepath = os.path.join(directory, first_file)
    name, ba_1 = sct_model.read_sct_file(filepath=filepath)

    filepath = os.path.join(directory, second_file)
    name2, ba_2 = sct_model.read_sct_file(filepath=filepath)

    differences = {first_file: compare_files(ba_1, ba_2)}

    out_name = f'{first_file}-{second_file}diffs.csv'
    out_path = os.path.join(directory, out_name)
    save_diffs(differences, out_path)
