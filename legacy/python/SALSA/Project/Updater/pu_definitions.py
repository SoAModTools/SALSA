from SALSA.Project.Updater.pu_constants import PP, UP

v7 = {
    PP.project: {
        1: {
            UP.callable: '_add_attr',
            UP.arguments: [('inst_id_colors', {})]
        }
    }
}

v6 = {
    PP.instruction: {
        1: {
            UP.callable: '_add_attr',
            UP.arguments: [('encode_inst', True)]
        }
    }
}

v5 = {
    PP.parameter: {
        1: {
            UP.callable: '_add_script_to_links',
            UP.up_args: ['cur_script']
        }
    }
}

v4 = {
    PP.script: {
        1: {
            UP.callable: '_del_attr',
            UP.arguments: ['links_to_sections', 'section_groups', 'section_group_keys']
        },
        2: {
            UP.callable: '_refactor_logical_sections'
        }
    }
}

v3 = {
    PP.section: {
        1: {
            UP.callable: '_move_sect_labels_v1',
        }
    }
}

v2 = {
    PP.project: {
        1: {
            UP.callable: '_change_attribute_names',
            UP.arguments: [
                ('scripts', 'scts'),
            ]
        },
    },
    PP.script: {
        1: {
            UP.callable: '_change_attribute_names',
            UP.arguments: [
                ('sections', 'sects'),
                ('sections_grouped', 'sect_tree'),
                ('sections_ungrouped', 'sect_list'),
                ('folded_sections', 'folded_sects')
            ]
        },
        2: {
            UP.callable: '_modify_section_groups_v1',
        },
        3: {
            UP.callable: '_move_string_garbage_v1',
        },
    },
    PP.section: {
        1: {
            UP.callable: '_change_attribute_names',
            UP.arguments: [
                ('instructions', 'insts'),
                ('instruction_ids_ungrouped', 'inst_list'),
                ('instruction_ids_grouped', 'inst_tree'),
                ('instructions_used', 'insts_used')
            ]
        },
        2: {
            UP.callable: '_move_sect_labels_v1',
        },
    },
    PP.instruction: {
        1: {
            UP.callable: '_change_attribute_names',
            UP.arguments: [
                ('instruction_id', 'base_id'),
                ('parameters', 'params'),
                ('loop_parameters', 'l_params'),
                ('frame_delay_param', 'delay_param')
            ]
        },
    },
    PP.parameter: {
        1: {
            UP.callable: '_retype_link_v1'
        },
        2: {
            UP.callable: '_del_attr',
            UP.arguments: ['link_value']
        }
    }
}

update_tasks = {
    1: v2,
    2: v3,
    3: v4,
    4: v5,
    5: v6,
    6: v7
}

p_max_depth = {
    1: PP.parameter,
    2: PP.instruction,
    3: PP.script,
    4: PP.parameter,
    5: PP.instruction,
    6: PP.project
}
