
def are_same(item1, item2):
    if not isinstance(item1, type(item2)):
        return False
    if not check_same_fxns[type(item1)](item1, item2):
        return False
    return True


def dicts_are_same(dict1, dict2):
    """Checks whether the entirety of dict1 and dict2 are the same. Only allows for base python classes"""
    if not isinstance(dict1, dict) or not isinstance(dict2, dict):
        return False
    dict1_keys = list(dict1.keys())
    dict2_keys = list(dict2.keys())
    if not check_same_fxns[list](dict1_keys, dict2_keys):
        return False
    for key in dict1_keys:
        if not isinstance(dict1[key], type(dict2[key])):
            return False
        if dict1[key] is None or dict2[key] is None:
            if not (dict1[key] is None and dict2[key] is None):
                return False
        elif not check_same_fxns[type(dict1[key])](dict1[key], dict2[key]):
            return False
    return True


def lists_are_same(list1, list2):
    """Checks whether the entirety of list1 and list2 are the same. Only allows for base python classes"""
    if not isinstance(list1, list) or not isinstance(list2, list) or len(list1) != len(list2):
        return False
    for i in range(len(list1)):
        if not isinstance(list1[i], type(list2[i])):
            return False
        if not check_same_fxns[type(list1[i])](list1[i], list2[i]):
            return False
    return True


def sets_are_same(set1, set2):
    if len(set1 - set2) > 0 or len(set2 - set1) > 0 or not isinstance(set1, set) or not isinstance(set2, set):
        return False
    for item in set1:
        if item not in set2:
            return False
    return True


def values_are_same(value1, value2):
    if not isinstance(value1, type(value2)):
        return False
    return value1 == value2


check_same_fxns = {
    dict: dicts_are_same,
    list: lists_are_same,
    str: values_are_same,
    int: values_are_same,
    float: values_are_same,
    tuple: lists_are_same,
    set: sets_are_same
}