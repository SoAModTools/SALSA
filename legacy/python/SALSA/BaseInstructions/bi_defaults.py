loop_count_name = 'iteration'

inst_defaults = {
    0: {
        "Location": "0x801f6044",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00000000,
        "Link": 1,
        "Link Type": "Jump",
        "Parameters": {
            0: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None,
                "Default": None
            },
            1: {
                "Type": "int|jump",
                "Signed": True,
                "Mask": None,
                "Default": None
            }
        }
    },
    1: {
        "Location": "0x801f60f0",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00000000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {}
    },
    2: {
        "Location": "0x801f60f0",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00000000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {}
    },
    3: {
        "Location": "0x801f5f50",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00000000,
        "Link": 3,
        "Link Type": "Switch",
        "Loop": [2, 3],
        "Loop Iterations": 1,
        "Switch Entry": [2, 3],
        "Parameters": {
            0: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None,
                "Default": None
            },
            1: {
                "Type": f"int|{loop_count_name}",
                "Signed": False,
                "Mask": None,
                "Default": 0
            },
            2: {
                "Type": "int",
                "Signed": True,
                "Mask": None
            },
            3: {
                "Type": "int|jump",
                "Signed": False,
                "Mask": None,
                "Default": None
            }
        }
    },
    4: {
        "Location": "0x801f60f0",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00000000,
        "Parameters": {}
    },
    5: {
        "Location": "0x801f5eb4",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00000000,
        "Parameters": {
            0: {
                "Type": "int|var|byte",
                "Mask": 0x0000ffff,
                "Signed": False,
                "Default": None
            },
            1: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            }
        }
    },
    6: {
        "Location": "0x801f5d7c",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00000000,
        "Parameters": {
            0: {
                "Type": "int|var|int",
                "Mask": 0x0000ffff,
                "Signed": False,
                "Default": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    7: {
        "Location": "0x801f5d08",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00000000,
        "Parameters": {
            0: {
                "Type": "int|var|float",
                "Mask": 0x0000ffff,
                "Signed": False,
                "Default": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            }
        }
    },
    8: {
        "Location": "0x801f60f0",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00000000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {}
    },
    9: {
        "Location": "0x801f60c4",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00000000,
        "Parameters": {
            0: {
                "Type": "scpt|skip",
                "Signed": False,
                "Mask": None
            }
        }
    },
    10: {
        "Link": 0,
        "Link Type": "Jump",
        "Location": "0x801f5ccc",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00000000,
        "Parameters": {
            0: {
                "Type": "int|jump",
                "Mask": 0xfffffffc,
                "Signed": True,
                "Default": None
            }
        }
    },
    11: {
        "Location": "0x801f5c30",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00000000,
        "Link": 0,
        "Link Type": "Jump",
        "Parameters": {
            0: {
                "Type": "int|subscript",
                "Signed": True,
                "Mask": None,
                "Default": None
            }
        }
    },
    12: {
        "Location": "0x801f5bd0",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00000000,
        "Parameters": {}
    },
    13: {
        "Location": "0x801f60f0",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00000000,
        "Parameters": {}
    },
    14: {
        "Location": "0x801f60f0",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00000000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {}
    },
    15: {
        "Location": "0x801f5bb4",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": True,
        "Hard parameter two": 0x00000000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {}
    },
    16: {
        "Location": "0x801f570c",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00000000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    17: {
        "Location": "0x801f5b14",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00000000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None,
                "Default": None
            }
        }
    },
    18: {
        "Location": "0x801f5a74",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00000000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None,
                "Default": None
            }
        }
    },
    19: {
        "Location": "0x801f59c0",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00000000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None,
                "Default": None
            }
        }
    },
    20: {
        "Location": "0x801f593c",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00000000,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            }
        }
    },
    21: {
        "Location": "0x801f58b8",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00000000,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            }
        }
    },
    22: {
        "Location": "0x801f58b0",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00000000,
        "Warning": "May cause infinite loop in script system, Instruction not used in the game, may be buggy",
        "Parameters": {}
    },
    23: {
        "Location": "0x801ff13c",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00000000,
        "Link": 0,
        "Link Type": "String",
        "Parameters": {
            0: {
                "Type": "int|footer",
                "Signed": True,
                "Mask": None,
                "Default": None
            }
        }
    },
    24: {
        "Location": "0x8020a938",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Link": 0,
        "Link Type": "String",
        "Parameters": {
            0: {
                "Type": "int|footer|string",
                "Signed": True,
                "Mask": None,
                "Default": None
            }
        }
    },
    25: {
        "Location": "0x8020a580",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Link": 1,
        "Link Type": "String",
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "int|footer|string",
                "Signed": False,
                "Mask": None,
                "Default": None
            }
        }
    },
    26: {
        "Location": "0x802088d8",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    27: {
        "Location": "0x80208774",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            }
        }
    },
    28: {
        "Location": "0x801fbba8",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00020000,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            }
        }
    },
    29: {
        "Location": "0x801fbf90",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00020000,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            }
        }
    },
    30: {
        "Location": "0x802085cc",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x0002000,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    31: {
        "Location": "0x80208234",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00050000,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    32: {
        "Location": "0x80208044",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00050000,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    33: {
        "Location": "0x80203ffc",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0xfffe0000,
        "Loop": [1, 2, 3, 4, 5, 6, 7],
        "Loop Iterations": 0,
        "Parameters": {
            0: {
                "Name": "Iterations",
                "Type": f"int|{loop_count_name}",
                "Signed": False,
                "Mask": None,
                "Default": 0
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
        }
    },
    34: {
        "Location": "0x801fc68c",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00050000,
        "Loop": [2, 3, 4, 5, 6, 7, 8, 9, 10],
        "Loop Iterations": 1,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Name": "Iterations",
                "Type": f"int|{loop_count_name}",
                "Signed": False,
                "Mask": None,
                "Default": 0
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            8: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            9: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            10: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    35: {
        "Location": "0x801fcaac",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00050000,
        "Loop": [2, 3, 4, 5, 6, 7, 8, 9, 10],
        "Loop Iterations": 1,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Name": "Iterations",
                "Type": f"int|{loop_count_name}",
                "Signed": False,
                "Mask": None,
                "Default": 0
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            8: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            9: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            10: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    36: {
        "Location": "0x801fc408",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00050000,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            }
        }
    },
    37: {
        "Location": "0x801fc184",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00050000,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            }
        }
    },
    38: {
        "Location": "0x80207cd0",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00020000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            }
        }
    },
    39: {
        "Location": "0x80207ed0",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00020000,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            }
        }
    },
    40: {
        "Location": "0x80206ea8",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            }
        }
    },
    41: {
        "Location": "0x80207a40",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    42: {
        "Location": "0x80207830",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Loop": [2],
        "Loop Iterations": 1,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Name": "Iterations",
                "Type": f"int|{loop_count_name}",
                "Signed": False,
                "Mask": None,
                "Default": 0
            },
            2: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            }
        }
    },
    43: {
        "Location": "0x802073b4",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00000000,
        "Link": 0,
        "Link Type": 'String',
        "Warning": "This instruction will exit the current script and load a new one",
        "Parameters": {
            0: {
                "Type": "int|footer",
                "Signed": False,
                "Mask": None
            }
        }
    },
    44: {
        "Location": "0x801fedc8",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00060000,
        "Warning": "May lead to an infinite loop, Instruction not used in the game, may be buggy",
        "Parameters": {}
    },
    45: {
        "Location": "0x8020726c",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {}
    },
    46: {
        "Location": "0x8020549c",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0xfffe0000,
        "Parameters": {
            0: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    47: {
        "Location": "0x80204f8c",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0xfffe0000,
        "Parameters": {
            0: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    48: {
        "Location": "0x802051d0",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    49: {
        "Location": "0x80204f04",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0xfffe0000,
        "Parameters": {
            0: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None,
            }
        }
    },
    50: {
        "Location": "0x80204cd0",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0xfffe0000,
        "Parameters": {
            0: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            }
        }
    },
    51: {
        "Location": "0x801fa060",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            }
        }
    },
    52: {
        "Location": "0x80204be8",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0xfffe0000,
        "Parameters": {
            0: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            }
        }
    },
    53: {
        "Location": "0x801fa630",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00050000,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            8: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            9: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            10: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            11: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            12: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            13: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            14: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            15: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            16: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            17: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            18: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            19: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    54: {
        "Location": "0x801fa4c0",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Link": 1,
        "Link Type": "String",
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "int|footer",
                "Signed": True,
                "Mask": None,
                "Default": None
            }
        }
    },
    55: {
        "Location": "0x801fa398",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    56: {
        "Location": "0x801fa290",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            }
        }
    },
    57: {
        "Location": "0x801fef18",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {}
    },
    58: {
        "Location": "0x801feee8",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {}
    },
    59: {
        "Location": "0x8020170c",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    60: {
        "Location": "0x802015ac",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    61: {
        "Location": "0x802021b8",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00040000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    62: {
        "Location": "0x80202004",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00040000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    63: {
        "Location": "0x80201e54",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00040000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    64: {
        "Location": "0x80201ce0",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00040000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    65: {
        "Location": "0x802019f8",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00040000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    66: {
        "Location": "0x801f9708",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            }
        }
    },
    67: {
        "Location": "0x801ff1e0",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {}
    },
    68: {
        "Location": "0x8020a2c8",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {}
    },
    69: {
        "Location": "0x801fa1d0",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Link": 0,
        "Link Type": "String",
        "Parameters": {
            0: {
                "Type": "int|footer",
                "Signed": True,
                "Mask": None,
                "Default": None
            }
        }
    },
    70: {
        "Location": "0x80207130",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    71: {
        "Location": "0x80206ff4",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    72: {
        "Location": "0x80206cbc",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            8: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    73: {
        "Location": "0x80206ab0",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            8: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            9: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            }
        }
    },
    74: {
        "Location": "0x80206904",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    75: {
        "Location": "0x8020660c",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            8: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            9: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            10: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            11: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    76: {
        "Location": "0x802056a8",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": True,
        "Force Frame Refresh Condition": "If 0x80346c3c == 2 and 80302f18 == 2, force frame refresh",
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    77: {
        "Location": "0x8020a048",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            }
        }
    },
    78: {
        "Location": "0x801fbd9c",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00020000,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            }
        }
    },
    79: {
        "Location": "0x80201228",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    80: {
        "Location": "0x80200f88",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {}
    },
    81: {
        "Location": "0x80200df4",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            8: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            }
        }
    },
    82: {
        "Location": "0x80200ce4",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            }
        }
    },
    83: {
        "Location": "0x80200b90",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            }
        }
    },
    84: {
        "Location": "0x802009c4",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            8: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            9: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            }
        }
    },
    85: {
        "Location": "0x8020d848",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Loop": [2],
        "Loop Iterations": 1,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Name": "Iterations",
                "Type": f"int|{loop_count_name}",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            }
        }
    },
    86: {
        "Location": "0x8020d668",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Loop": [2],
        "Loop Iterations": 1,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Name": "Iterations",
                "Type": f"int|{loop_count_name}",
                "Signed": False,
                "Mask": None,
                "Default": 0
            },
            2: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            }
        }
    },
    87: {
        "Location": "0x8020d488",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Loop": [2],
        "Loop Iterations": 1,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Name": "Iterations",
                "Type": f"int|{loop_count_name}",
                "Signed": False,
                "Mask": None,
                "Default": 0
            },
            2: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            }
        }
    },
    88: {
        "Location": "0x8020d2a8",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Loop": [2],
        "Loop Iterations": 1,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Name": "Iterations",
                "Type": f"int|{loop_count_name}",
                "Signed": False,
                "Mask": None,
                "Default": 0
            },
            2: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            }
        }
    },
    89: {
        "Location": "0x801fb664",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00050000,
        "Loop": [2, 3, 4, 5, 6, 7, 8, 9, 10],
        "Loop Iterations": 1,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Name": "Iterations",
                "Type": f"int|{loop_count_name}",
                "Signed": False,
                "Mask": None,
                "Default": 0
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            8: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            9: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            10: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    90: {
        "Location": "0x801fb120",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00050000,
        "Loop": [2, 3, 4, 5, 6, 7, 8, 9, 10],
        "Loop Iterations": 1,
        "Implement": True,
        "Parameter num": 13,
        "Notes": f"Only runs if param1 & 1 == 1:\n\n1: scpt|short\n2: int\n\nloop ((2)) 'iterations'):\n\n3: scpt|float\n4: scpt|float\n5: scpt|float\n6: scpt|float\n7: scpt|int\n8: scpt|int\n9: scpt|float\n10: scpt|int\n11: scpt|int",
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Name": "Iterations",
                "Type": f"int|{loop_count_name}",
                "Signed": False,
                "Mask": None,
                "Default": 0
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            8: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            9: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            10: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    91: {
        "Location": "0x8020d1c0",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    92: {
        "Location": "0x801ffef0",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    93: {
        "Location": "0x801ffe3c",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    94: {
        "Location": "0x801ffdb4",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    95: {
        "Location": "0x80206424",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {
            0: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    96: {
        "Location": "0x8020623c",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    97: {
        "Location": "0x80208a20",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    98: {
        "Location": "0x801ffc20",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Loop": [1],
        "Loop Iterations": 0,
        "Parameters": {
            0: {
                "Name": "Iterations",
                "Type": f"int|{loop_count_name}",
                "Signed": False,
                "Mask": None,
                "Default": 0
            },
            1: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            }
        }
    },
    99: {
        "Location": "0x801ffa60",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Loop": [1],
        "Loop Iterations": 0,
        "Parameters": {
            0: {
                "Name": "Iterations",
                "Type": f"int|{loop_count_name}",
                "Signed": False,
                "Mask": None,
                "Default": 0
            },
            1: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            }
        }
    },
    100: {
        "Location": "0x80208c08",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            }
        }
    },
    101: {
        "Location": "0x801f984c",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    102: {
        "Location": "0x801f9668",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    103: {
        "Location": "0x801f9614",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {}
    },
    104: {
        "Location": "0x801f9454",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0xfffe0000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    105: {
        "Location": "0x801f934c",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0xfffe0000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    106: {
        "Location": "0x801f9120",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            8: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            }
        }
    },
    107: {
        "Location": "0x801f8f7c",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            }
        }
    },
    108: {
        "Location": "0x801fabdc",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00050000,
        "Loop": [2, 3, 4, 5, 6, 7, 8, 9, 10],
        "Loop Iterations": 1,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Name": "Iterations",
                "Type": f"int|{loop_count_name}",
                "Signed": False,
                "Mask": None,
                "Default": 0
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            8: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            9: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            10: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    109: {
        "Location": "0x80200fc4",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            8: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            }
        }
    },
    110: {
        "Location": "0x801ff0f8",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Link": 0,
        "Link Type": "String",
        "Parameters": {
            0: {
                "Type": "int|footer",
                "Signed": False,
                "Mask": None,
                "Default": None
            }
        }
    },
    111: {
        "Location": "0x802060d4",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0xfffe0000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    112: {
        "Location": "0x801ff210",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    113: {
        "Location": "0x801ff05c",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Link": 0,
        "Link Type": "String",
        "Parameters": {
            0: {
                "Type": "int|footer",
                "Signed": False,
                "Mask": None,
                "Default": None
            }
        }
    },
    114: {
        "Location": "0x801fef80",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            }
        }
    },
    115: {
        "Location": "0x801fef48",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {}
    },
    116: {
        "Location": "0x801f9ea8",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            }
        }
    },
    117: {
        "Location": "0x80208d9c",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    118: {
        "Location": "0x80209330",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Loop": [2],
        "Loop Iterations": 1,
        "Loop Break Condition": {
            "Parameter": 1,
            "Test": "==",
            "Value": 0x10000,
            "Location": 'External'
        },
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Name": "Iterations",
                "Type": f"int|{loop_count_name}",
                "Signed": False,
                "Mask": None,
                "Default": 0
            },
            2: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            }
        }
    },
    119: {
        "Location": "0x80209028",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Loop": [2],
        "Loop Iterations": 1,
        "Loop Break Condition": {
            "Parameter": 1,
            "Test": "==",
            "Value": 0x10000,
            "Location": 'External'
        },
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Name": "Iterations",
                "Type": f"int|{loop_count_name}",
                "Signed": False,
                "Mask": None,
                "Default": 0
            },
            2: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            }
        }
    },
    120: {
        "Location": "0x801f8608",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00020000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    121: {
        "Location": "0x802034b0",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0xfffe0000,
        "Loop": [1, 2, 3, 4, 5, 6, 7],
        "Loop Iterations": 0,
        "Parameters": {
            0: {
                "Name": "Iterations",
                "Type": f"int|{loop_count_name}",
                "Signed": False,
                "Mask": None,
                "Default": 0
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
        }
    },
    122: {
        "Location": "0x80205f14",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    123: {
        "Location": "0x8020527c",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0xfffe0000,
        "Parameters": {
            0: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    124: {
        "Location": "0x80204e28",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0xfffe0000,
        "Parameters": {
            0: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None,
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    125: {
        "Location": "0x8020731c",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0xfffe0000,
        "Parameters": {}
    },
    126: {
        "Location": "0x80207554",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Loop": [1],
        "Loop Iterations": 0,
        "Parameters": {
            0: {
                "Name": "Iterations",
                "Type": f"int|{loop_count_name}",
                "Signed": False,
                "Mask": None,
                "Default": 0
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    127: {
        "Location": "0x80207630",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Loop": [1],
        "Loop Iterations": 0,
        "Parameters": {
            0: {
                "Name": "Iterations",
                "Type": f"int|{loop_count_name}",
                "Signed": False,
                "Mask": None,
                "Default": 0
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    128: {
        "Location": "0x801f8d0c",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00020000,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    129: {
        "Location": "0x801f57f8",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": f"int|{loop_count_name}",
                "Signed": False,
                "Mask": None
            }
        }
    },
    130: {
        "Location": "0x801f88d4",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00050000,
        "Loop": [2, 3, 4, 5, 6, 7, 8],
        "Loop Iterations": 1,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Name": "Iterations",
                "Type": f"int|{loop_count_name}",
                "Signed": False,
                "Mask": None,
                "Default": 0
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            8: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            }
        }
    },
    131: {
        "Location": "0x80200634",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Loop": [1, 2, 3, 4],
        "Loop Iterations": 0,
        "Parameters": {
            0: {
                "Name": "Iterations",
                "Type": f"int|{loop_count_name}",
                "Mask": 0x0000ffff,
                "Signed": False,
                "Default": 0
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    132: {
        "Location": "0x80200460",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Loop": [1, 2, 3, 4, 5, 6, 7],
        "Loop Iterations": 0,
        "Parameters": {
            0: {
                "Name": "Iterations",
                "Type": f"int|{loop_count_name}",
                "Mask": 0x0000ffff,
                "Signed": False,
                "Default": 0
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    133: {
        "Location": "0x802001f8",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Loop": [2, 3, 4, 5, 6, 7, 8, 9, 10],
        "Loop Iterations": 0,
        "Parameters": {
            0: {
                "Name": "Iterations",
                "Type": f"int|{loop_count_name}",
                "Signed": False,
                "Mask": None,
                "Default": 0
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            8: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            9: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            10: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    134: {
        "Location": "0x80209548",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00020000,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    135: {
        "Location": "0x80201458",
        "Skip Frame Refresh": "0x00000000",
        "Force Frame Refresh": False,
        "Hard parameter two": "0x00010000",
        "Loop": [2],
        "Loop Iterations": 1,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Name": "Iterations",
                "Type": f"int|{loop_count_name}",
                "Signed": False,
                "Mask": None,
                "Default": 0
            },
            2: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            }
        }
    },
    136: {
        "Location": "0x80209b94",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00050000,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    137: {
        "Location": "0x80207c2c",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00050000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    138: {
        "Location": "0x801fed58",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {}
    },
    139: {
        "Location": "0x80202dec",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0xfffe0000,
        "Loop": [1, 2, 3, 4, 5, 6, 7],
        "Loop Iterations": 0,
        "Parameters": {
            0: {
                "Name": "Iterations",
                "Type": f"int|{loop_count_name}",
                "Signed": False,
                "Mask": None,
                "Default": 0
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    140: {
        "Location": "0x801fe960",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    141: {
        "Location": "0x801fe7a0",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    142: {
        "Location": "0x801fe560",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    143: {
        "Location": "0x8020a72c",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Warning": "May cause an infinite loop. Instruction not used in the game, may be buggy",
        "Parameters": {}
    },
    144: {
        "Location": "0x8020a734",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Link": 0,
        "Link Type": "String",
        "Parameters": {
            0: {
                "Type": "int|string",
                "Signed": True,
                "Mask": None,
                "Default": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    145: {
        "Location": "0x801fe21c",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    146: {
        "Location": "0x801fe06c",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    147: {
        "Location": "0x801f9c18",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            8: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            9: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    148: {
        "Location": "0x802007c0",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Loop": [1, 2, 3, 4, 5, 6, 7, 8, 9],
        "Loop Iterations": 0,
        "Parameters": {
            0: {
                "Name": "Iterations",
                "Type": f"int|{loop_count_name}",
                "Signed": False,
                "Mask": None,
                "Default": 0
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            8: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            9: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    149: {
        "Location": "0x801ff604",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    150: {
        "Location": "0x801ff470",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    151: {
        "Location": "0x801ff380",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    152: {
        "Location": "0x8020ceb4",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Loop": [1],
        "Loop Iterations": 0,
        "Parameters": {
            0: {
                "Name": "Iterations",
                "Type": f"int|{loop_count_name}",
                "Signed": False,
                "Mask": None,
                "Default": 0
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    153: {
        "Location": "0x801fd264",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Loop": [1, 2, 3],
        "Loop Iterations": 0,
        "Loop Break Condition": {
            "Parameter": 3,
            "Test": "==",
            "Value": 0,
            "Location": 'Internal'
        },
        "Parameters": {
            0: {
                "Name": "Iterations",
                "Type": f"int|{loop_count_name}",
                "Signed": False,
                "Mask": None,
                "Default": 0
            },
            1: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            }
        }
    },
    154: {
        "Location": "0x8020c390",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    155: {
        "Location": "0x8020a338",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Link": 1,
        "Link Type": "String",
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "int|string",
                "Signed": True,
                "Mask": None
            },
            2: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    156: {
        "Location": "0x801fed04",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {}
    },
    157: {
        "Location": "0x801fec80",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    158: {
        "Location": "0x801febfc",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    159: {
        "Location": "0x802023bc",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0xfffe0000,
        "Loop": [1, 2, 3, 4, 5, 6, 7, 8],
        "Loop Iterations": 0,
        "Parameters": {
            0: {
                "Name": "Iterations",
                "Type": f"int|{loop_count_name}",
                "Signed": False,
                "Mask": None,
                "Default": 0
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            8: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            }
        }
    },
    160: {
        "Location": "0x8020a168",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            }
        }
    },
    161: {
        "Location": "0x801f8158",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00050000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Loop": [2, 3, 4, 5, 6, 7, 8, 9, 10],
        "Loop Iterations": 1,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Name": "Iterations",
                "Type": f"int|{loop_count_name}",
                "Signed": False,
                "Mask": None,
                "Default": 0
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            8: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            9: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            10: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
        }
    },
    162: {
        "Location": "0x80205e48",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    163: {
        "Location": "0x80205c24",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            8: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    164: {
        "Location": "0x8020184c",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    165: {
        "Location": "0x80215590",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            8: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            9: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            10: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            11: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            12: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            13: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            14: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            15: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            16: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            17: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            18: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    166: {
        "Location": "0x80215368",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    167: {
        "Location": "0x802150bc",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {}
    },
    168: {
        "Location": "0x801fdf0c",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    169: {
        "Location": "0x801fde70",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            }
        }
    },
    170: {
        "Location": "0x8021507c",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {}
    },
    171: {
        "Location": "0x8021503c",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {}
    },
    172: {
        "Location": "0x80214fac",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    173: {
        "Location": "0x80214f1c",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    174: {
        "Location": "0x80214e8c",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    175: {
        "Location": "0x801f5468",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            }
        }
    },
    176: {
        "Location": "0x80209974",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    177: {
        "Location": "0x8020dd1c",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            8: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            9: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            10: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            11: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            12: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            }
        }
    },
    178: {
        "Location": "0x802049bc",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    179: {
        "Location": "0x802058f8",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    180: {
        "Location": "0x80201b6c",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    181: {
        "Location": "0x801fddb0",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    182: {
        "Location": "0x801f60f0",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Warning": "This instruction does nothing. Instruction not used in the game, may be buggy",
        "Parameters": {}
    },
    183: {
        "Location": "0x801fdfb8",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            }
        }
    },
    184: {
        "Location": "0x802057e0",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    185: {
        "Location": "0x8020bcf4",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    186: {
        "Location": "0x8020bc38",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    187: {
        "Location": "0x8020bb8c",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    188: {
        "Location": "0x8020bae0",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    189: {
        "Location": "0x801f60f0",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {}
    },
    190: {
        "Location": "0x802097c0",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            }
        }
    },
    191: {
        "Location": "0x80214dfc",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    192: {
        "Location": "0x80214d6c",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    193: {
        "Location": "0x80214cdc",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    194: {
        "Location": "0x80214c04",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    195: {
        "Location": "0x801fdab8",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    196: {
        "Location": "0x80207368",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {}
    },
    197: {
        "Location": "0x8021528c",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    198: {
        "Location": "0x80209ed8",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            }
        }
    },
    199: {
        "Location": "0x80209d74",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            }
        }
    },
    200: {
        "Location": "0x801f60f0",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {}
    },
    201: {
        "Location": "0x801fd9b0",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {}
    },
    202: {
        "Location": "0x801fd8f0",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {}
    },
    203: {
        "Location": "0x801f98e8",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    204: {
        "Location": "0x80214b28",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    205: {
        "Location": "0x80214aa0",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    206: {
        "Location": "0x80214a60",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {}
    },
    207: {
        "Location": "0x8020bdb0",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            }
        }
    },
    208: {
        "Location": "0x80214a20",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {}
    },
    209: {
        "Location": "0x802149e0",
        "Skip Frame Refresh": "0x00000001",
        "Hard parameter two": "0x00010000",
        "Parameters": {}
    },
    210: {
        "Location": "0x80214994",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Link": 0,
        "Link Type": "String",
        "Parameters": {
            0: {
                "Type": "int|footer",
                "Signed": False,
                "Mask": None,
                "Default": None
            }
        }
    },
    211: {
        "Location": "0x80214954",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {}
    },
    212: {
        "Location": "0x802150fc",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    213: {
        "Location": "0x80215444",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            8: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            9: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            10: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            11: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            12: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            13: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            14: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            15: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            16: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            }
        }
    },
    214: {
        "Location": "0x80207428",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Link": 0,
        "Link Type": "String",
        "Parameters": {
            0: {
                "Type": "int|footer",
                "Signed": False,
                "Mask": None,
                "Default": None
            }
        }
    },
    215: {
        "Location": "0x801fee54",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Link": 1,
        "Link Type": "String",
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "int|footer",
                "Signed": False,
                "Mask": None,
                "Default": None
            }
        }
    },
    216: {
        "Location": "0x801fedd0",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    217: {
        "Location": "0x8020b9ec",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    218: {
        "Location": "0x8020b94c",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    219: {
        "Location": "0x801fd850",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    220: {
        "Location": "0x801fd668",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            8: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            }
        }
    },
    221: {
        "Location": "0x801fd584",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {
            0: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    222: {
        "Location": "0x8020b8ac",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    223: {
        "Location": "0x80214270",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            8: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            9: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            10: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            11: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            12: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            13: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            14: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            15: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            16: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            17: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            18: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            19: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            20: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            21: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            22: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            23: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            24: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    224: {
        "Location": "0x80213fe8",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            8: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            9: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            10: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            11: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            12: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            13: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            14: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            15: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            16: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            17: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            18: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            19: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            20: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            21: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            22: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            23: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            24: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            }
        }
    },
    225: {
        "Location": "0x80213dd0",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    226: {
        "Location": "0x80213920",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    227: {
        "Location": "0x801feb78",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    228: {
        "Location": "0x801feb40",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {}
    },
    229: {
        "Location": "0x801feb08",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {}
    },
    230: {
        "Location": "0x80205a3c",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    231: {
        "Location": "0x801fdcdc",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    232: {
        "Location": "0x802139f8",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            8: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            9: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            10: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            11: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            12: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            13: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    233: {
        "Location": "0x8020cba4",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    234: {
        "Location": "0x801fed3c",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Warning": "This instruction does nothing and returns zero. Instruction not used in the game, may be buggy",
        "Parameters": {}
    },
    235: {
        "Location": "0x8020b80c",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    236: {
        "Location": "0x8020b670",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {
            0: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    237: {
        "Location": "0x801fd54c",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {}
    },
    238: {
        "Location": "0x802074b0",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            }
        }
    },
    239: {
        "Location": "0x801fd1d0",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    240: {
        "Location": "0x801f7d2c",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    241: {
        "Location": "0x801f7cd8",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {}
    },
    242: {
        "Location": "0x801fcff0",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    243: {
        "Location": "0x801f9988",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            8: {
                "Type": "scpt|byte",
                "Signed": False,
                "Mask": None
            },
            9: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    244: {
        "Location": "0x8020bee8",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    245: {
        "Location": "0x801ff8d4",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Loop": [1],
        "Loop Iterations": 0,
        "Parameters": {
            0: {
                "Name": "Iterations",
                "Type": f"int|{loop_count_name}",
                "Signed": False,
                "Mask": None,
                "Default": 0
            },
            1: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            }
        }
    },
    246: {
        "Location": "0x801ff714",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Loop": [1],
        "Loop Iterations": 0,
        "Parameters": {
            0: {
                "Name": "Iterations",
                "Type": f"int|{loop_count_name}",
                "Signed": False,
                "Mask": None,
                "Default": 0
            },
            1: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            }
        }
    },
    247: {
        "Location": "0x801f95c0",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {}
    },
    248: {
        "Location": "0x801f7c18",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Link": 0,
        "Link Type": "String",
        "Parameters": {
            0: {
                "Type": "int|footer",
                "Signed": False,
                "Mask": None,
                "Default": None
            }
        }
    },
    249: {
        "Location": "0x80208424",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    250: {
        "Location": "0x801f7b14",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Link": 0,
        "Link Type": "String",
        "Parameters": {
            0: {
                "Type": "int|footer",
                "Signed": False,
                "Mask": None,
                "Default": None
            }
        }
    },
    251: {
        "Location": "0x8020014c",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            }
        }
    },
    252: {
        "Location": "0x802000a0",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            }
        }
    },
    253: {
        "Location": "0x801fffa4",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    254: {
        "Location": "0x802138e0",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {}
    },
    255: {
        "Location": "0x802138a0",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Parameters": {}
    },
    256: {
        "Location": "0x801fdc70",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Warning": "Instruction not used in the game, may be buggy",
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    257: {
        "Location": "0x80214908",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Link": 0,
        "Link Type": "String",
        "Parameters": {
            0: {
                "Type": "int|footer",
                "Signed": False,
                "Mask": None,
                "Default": None
            }
        }
    },
    258: {
        "Location": "0x801fd44c",
        "Skip Frame Refresh": True,
        "Hard parameter two": 0x00010000,
        "Loop": [1],
        "Loop Iterations": 0,
        "Parameters": {
            0: {
                "Name": "Iterations",
                "Type": f"int|{loop_count_name}",
                "Signed": False,
                "Mask": None,
                "Default": 0
            },
            1: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    259: {
        "Location": "0x8020b4ac",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    260: {
        "Location": "0x801fda70",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {}
    },
    261: {
        "Location": "0x8020ae04",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {}
    },
    262: {
        "Location": "0x801fe310",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    263: {
        "Location": "0x802137d0",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Loop": [1],
        "Loop Iterations": 0,
        "Parameters": {
            0: {
                "Name": "Iterations",
                "Type": f"int|{loop_count_name}",
                "Signed": False,
                "Mask": None,
                "Default": 0
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            }
        }
    },
    264: {
        "Location": "0x801f7f98",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00050000,
        "Parameters": {
            0: {
                "Type": "scpt|short",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            2: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            3: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            4: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            5: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            6: {
                "Type": "scpt|float",
                "Signed": False,
                "Mask": None
            },
            7: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            }
        }
    },
    265: {
        "Location": "0x8020aaa8",
        "Skip Frame Refresh": False,
        "Force Frame Refresh": False,
        "Hard parameter two": 0x00010000,
        "Link": 1,
        "Link Type": "String",
        "Parameters": {
            0: {
                "Type": "scpt|int",
                "Signed": False,
                "Mask": None
            },
            1: {
                "Type": "int|string",
                "Signed": True,
                "Mask": None,
                "Default": None
            }
        }
    }
}
