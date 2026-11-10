import sys, pathlib
sys.path.insert(0, 'port/tools')
import ps3
o, err = ps3.compile_one(pathlib.Path('port/src/gs/gs_core.c'), route='port')
print('result:', o, err)
