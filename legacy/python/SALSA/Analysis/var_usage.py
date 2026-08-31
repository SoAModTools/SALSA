from SALSA.Project.project_container import SCTProject


class VarUsage:
    bits_used = {}
    bytes_used = {}
    ints_used = {}
    floats_used = {}

    def get_container_by_type_string(self, var_type):
        if var_type == 'BitVar':
            return self.bits_used
        elif var_type == "ByteVar":
            return self.bytes_used
        elif var_type == "IntVar":
            return self.ints_used
        else:
            return self.floats_used

    @classmethod
    def record_usage(cls, prj: SCTProject):
        usage = cls()

        for var_type, var_dict in prj.global_variables.items():
            var_container = usage.get_container_by_type_string(var_type)
            for var_id in var_dict:
                if var_id not in var_container:
                    var_container[var_id] = []

        for sct_name, sct in prj.scts.items():
            for var_type, var_dict in sct.variables.items():
                var_container = usage.get_container_by_type_string(var_type)
                for var_id, locs in var_dict.items():
                    if var_id not in var_container:
                        var_container[var_id] = []
                    for use in locs['usage']:
                        new_use = tuple([sct_name, *use])
                        var_container[var_id].append(new_use)

        return usage

    def get_usage_csv(self, verbose) -> str:
        column2 = 'Is Used' if not verbose else 'Uses'
        csv_header_parts = ['BitVar', '', '', 'ByteVar', '', '', 'IntVar', '', '', 'FloatVar', '',]
        csv_header_parts_2 = ['VarID', column2, '', 'VarID', column2, '', 'VarID', column2, '', 'VarID', column2]
        csv_header = ("This worksheet shows whether a script variable is used or not. "
                      "Does not print out all possible variables, only up to the max variable used for each type"
                      " since the storage size for variables could be different in GC vs. DC versions of the game"
                      if not verbose else
                      "This worksheet prints out all usages of script variables in the loaded project"
                      "Does not print out all possible variables, only up to the max variable used for each type"
                      " since the storage size for variables could be different in GC vs. DC versions of the game")
        csv_lines = [
            csv_header + ',,,,,,,,,,',
            ','.join(csv_header_parts),
            ','.join(csv_header_parts_2)
        ]
        max_bit = max(self.bits_used.keys()) if len(self.bits_used) > 0 else -1
        max_byte = max(self.bytes_used.keys()) if len(self.bytes_used) > 0 else -1
        max_int = max(self.ints_used.keys()) if len(self.ints_used) > 0 else -1
        max_float = max(self.floats_used.keys()) if len(self.floats_used) > 0 else -1
        max_max = max(max_int, max_float, max_byte, max_bit)
        print(f'maxes: bit:{max_bit}, byte:{max_byte}, int:{max_int}, float:{max_float}')

        i = 0
        while i <= max_max:
            csv_line_parts = []

            if i <= max_bit:
                csv_line_parts.append(str(i))
                if i in self.bits_used.keys():
                    if verbose:
                        csv_line_parts.append("\""+'\n'.join([":".join(i).replace(',','-') for i in self.bits_used[i]])+"\"")
                    else:
                        csv_line_parts.append('Yes')
                else:
                    csv_line_parts.append("Not Used")
            else:
                csv_line_parts.append("")
                csv_line_parts.append("")
            csv_line_parts.append("")

            if i <= max_byte:
                csv_line_parts.append(str(i))
                if i in self.bytes_used.keys():
                    if verbose:
                        csv_line_parts.append("\""+'\n'.join([":".join(i).replace(',','-') for i in self.bytes_used[i]])+"\"")
                    else:
                        csv_line_parts.append('Yes')
                else:
                    csv_line_parts.append("Not Used")
            else:
                csv_line_parts.append("")
                csv_line_parts.append("")
            csv_line_parts.append("")

            if i <= max_int:
                csv_line_parts.append(str(i))
                if i in self.ints_used.keys():
                    if verbose:
                        csv_line_parts.append("\""+'\n'.join([":".join(i).replace(',','-') for i in self.ints_used[i]])+"\"")
                    else:
                        csv_line_parts.append('Yes')
                else:
                    csv_line_parts.append("Not Used")
            else:
                csv_line_parts.append("")
                csv_line_parts.append("")
            csv_line_parts.append("")

            if i <= max_float:
                csv_line_parts.append(str(i))
                if i in self.floats_used.keys():
                    if verbose:
                        csv_line_parts.append("\""+'\n'.join([":".join(i).replace(',','-') for i in self.floats_used[i]])+"\"")
                    else:
                        csv_line_parts.append('Yes')
                else:
                    csv_line_parts.append("Not Used")
            else:
                csv_line_parts.append("")
                csv_line_parts.append("")

            csv_lines.append(','.join(csv_line_parts))

            i += 1

        return '\n'.join(csv_lines)