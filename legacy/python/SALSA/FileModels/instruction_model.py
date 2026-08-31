import json
import os


class InstructionModel:

    def __init__(self):

        self.user_filename = "./UserSettings/UserInstructions.json"
        self.default_filename = "./SALSA/BaseInstructions/DefaultInstructions.json"

    def load_instructions(self, inst_type):
        filename = self.user_filename if inst_type == 'user' else self.default_filename
        if not os.path.exists(filename):
            insts = {}
            for i in range(0, 266):
                insts[str(i)] = {'Instruction ID': str(i), 'Parameters': {}}
            return insts

        with open(filename, 'r') as fh:
            instructions = json.loads(fh.read())

        return instructions

    def save_instructions(self, inst_dict, inst_type):
        """Function to save instructions to file"""
        print("saving to file")
        filename = self.user_filename if inst_type == 'user' else self.default_filename
        with open(filename, 'w') as fh:
            fh.write(json.dumps(inst_dict, indent=2))
