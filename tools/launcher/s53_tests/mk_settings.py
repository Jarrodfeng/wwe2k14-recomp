import json, sys
R = r'C:\Users\Jarrod\wwe2k14-recomp'
base = {"game_data_dir": R + r'\game_data\extracted', "user_data_dir": R + r'\out\s53\userdata_test',
        "first_run_done": True, "check_updates": False}
json.dump(base, open(R + r'\out\s53\set_data.json', 'w'), indent=2)
json.dump(dict(base, quick_start=True), open(R + r'\out\s53\set_quick.json', 'w'), indent=2)
json.dump({"first_run_done": True, "check_updates": False}, open(R + r'\out\s53\set_nodata.json', 'w'), indent=2)
print(open(R + r'\out\s53\set_data.json').read())
