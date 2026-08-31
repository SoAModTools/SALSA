class HelpStrings:
    instruction_descriptions = "Instruction Descriptions\n" \
                           "------------------------\n\n" \
                           "Instruction descriptions are flexible, in that certain modifiers can be " \
                           "used to display the result of parameters from the instruction, or even " \
                           "perform basic manipulation of those parameter values.\n" \
                           "\n" \
                           "* Displaying a parameter value -\n" \
                           "By enclosing a parameter name with greater and less " \
                           "than signs, e.g. <offset> for the parameter named offset, the text will " \
                           "be replaced by the result of the parameter. This is performed first, and " \
                           "its result can be used in other operations\n" \
                           "\n" \
                           "* Basic Operations -\n" \
                           "Certain basic operations are available: add, subtract, multiply, " \
                           "and link. These operations all use a similar syntax and take in two " \
                           "parameters and can read hex, float, or ints. The format of the first " \
                           "parameter determines the format of the output.\n" \
                           "\n" \
                           "* add - *add[(1),(2)]* - the value of (2) will be added to (1). " \
                           "The resultant number will be in the style of (1). All floats will be " \
                           "converted to ints before use.\n" \
                           "  * e.g. *add[1,2]* -> 3\n" \
                           "\n" \
                           "* subtract - *sub[(1),(2)]* - the value of (2) will be subtracted from (1). " \
                           "The resultant number will be in the style of (1). " \
                           "All floats will be converted to ints before use.\n" \
                           "  * e.g. *sub[5,2]* -> 3\n" \
                           "\n" \
                           "* multiply - *mul[(1),(2)]* - the value of (2) will be multiplied to (1). " \
                           "The resultant number will be in the style of (1). " \
                           "Multiplication of floats is allowed and will output a float.\n" \
                           "  * e.g. *mul[1,2]* -> 2\n" \
                           "\n" \
                           "* link - *lnk[(1),(2)]* - Link determines the target of an offset. " \
                           "(1) is the name of the parameter to use as the start position. " \
                           "(2) is the offset. With instructions which link to other positions in the " \
                           "script file, a parameter is used  to determine the jump offset to the " \
                           "linked data. The position of the offset parameter is used as the start point, " \
                           "with the value of the parameter being the offset itself\n" \
                           "  * e.g. *lnk[offset,<offset>]* -> Target instruction/string.\n" \
                           "\n\nOther Features:\n\n" \
                           "* Prefixing an instruction name with '??' will add the instruction ID to the end of the" \
                           " instruction in the script view instruction tree" \


    script_notes = "Script Features\n" \
                   "---------------\n\n" \
                   "* Setting a memory position for the script\n" \
                   "The in-game position of each script instruction can be updated by entering the start position for" \
                   " the script. This start position is the pointer value at 0x8030CEA0."

    other_notes = "Other Notes\n" \
                  "-----------\n\n" \
                  "Hardcoded param 1 and the param1 referred to in some of the instruction notes are " \
                  "different parameters. Since instructions are able to take place over multiple frames, " \
                  "Param1 appears to be sent as a sort of indicator on the state of the instruction so that " \
                  "the desired instruction can continue where it left off. Bits within param1 are used to " \
                  "control this state:\n" \
                  "\n" \
                  "* param1 & 1: initialize instruction\n" \
                  "* param1 & 2: run instruction\n" \
                  "* param1 & 8: terminate instruction"

    def get_all(self):
        return {
            'Scripts': self.script_notes,
            'Other': self.other_notes
        }
