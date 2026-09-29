import os
import numpy
import shutil
import subprocess
from spicelib import RawRead
import math

from torch.utils.hipify.hipify_python import value


def simulate_on_ltspice_norm(filepath, filename, maxtime):
    test_path = "./test/norm"
    if not os.path.exists(test_path):
        os.makedirs(test_path)
    new_path = os.path.join(test_path, filename)
    shutil.copy(filepath, new_path)
    cmd = "ltspice -norm -b " + filename
    try:
        result = subprocess.run(cmd, cwd=test_path, timeout=maxtime, shell=True)
        if result.returncode == 0:
            return 0
        else:
            return 1
    except subprocess.TimeoutExpired:
        cmd = "TASKKILL /F /IM Ltspice.exe /T"
        subprocess.run(cmd, shell=True)
        return 2


def simulate_on_ltspice_alt(filepath, filename, maxtime):
    test_path = "./test/alt"
    if not os.path.exists(test_path):
        os.makedirs(test_path)
    new_path = os.path.join(test_path, filename)
    shutil.copy(filepath, new_path)
    cmd = "ltspice -alt -b " + filename
    try:
        result = subprocess.run(cmd, cwd=test_path, timeout=maxtime, shell=True)
        if result.returncode == 0:
            return 0
        else:
            return 1
    except subprocess.TimeoutExpired:
        cmd = "TASKKILL /F /IM Ltspice.exe /T"
        subprocess.run(cmd, shell=True)
        return 2


def get_same_time_points(path1,path2):
    raw_file1 = RawRead(path1)
    raw_file2 = RawRead(path2)
    trace_names1 = raw_file1.get_trace_names()
    if "time" not in trace_names1:
        return -1,[],[]
    trace_time1 = raw_file1.get_trace("time")
    trace_time2 = raw_file2.get_trace("time")
    wave_time1 = trace_time1.get_wave(0)
    wave_time2 = trace_time2.get_wave(0)
    points_1 = []
    points_2 = []
    point_1 = 0
    point_2 = 0
    while point_1 < len(wave_time1) and point_2 < len(wave_time2):
        if wave_time1[point_1] == wave_time2[point_2]:
            points_1.append(point_1)
            points_2.append(point_2)
            point_1 += 1
            point_2 += 1
        elif wave_time1[point_1] < wave_time2[point_2]:
            point_1 += 1
        else:
            point_2 += 1
    return 0,points_1,points_2


def compare_ltspice_raw_file(path1, path2, error):
    state, points1,points2 = get_same_time_points(path1,path2)
    raw_file1 = RawRead(path1)
    raw_file2 = RawRead(path2)
    trace_names1 = raw_file1.get_trace_names()
    trace_names2 = raw_file2.get_trace_names()
    for i in range(len(trace_names1)):
        trace_name1 = trace_names1[i]
        trace_name2 = trace_names2[i]
        if trace_name1 == "time":
            continue
        trace1 = raw_file1.get_trace(trace_name1)
        trace2 = raw_file2.get_trace(trace_name2)
        wave1 = trace1.get_wave(0)
        wave2 = trace2.get_wave(0)
        if state == -1:
            if len(wave1) != len(wave2):
                return 1
            for k in range(len(wave1)):
                value1 = wave1[k]
                value2 = wave2[k]
                min_value = min(math.fabs(value1), math.fabs(value2))
                if math.fabs(value1 - value2) > numpy.float32(error) * min_value:
                    return 1
                # if not ((value1 - numpy.float32(error)) <= value2 <= (value1 + numpy.float32(error))):
                #     return 1
            return 0
        elif state == 0:
            for k in range(len(points1)):
                value1= wave1[points1[k]]
                value2= wave2[points2[k]]
                min_value = min(math.fabs(value1), math.fabs(value2))
                if math.fabs(value1 - value2) > numpy.float32(error) * min_value:
                    return 1
                # if not ((value1-numpy.float32(error))<=value2<=(value1+numpy.float32(error))):
                #     return 1
            return 0


def differential_testing(filepath, filename, maxtime, error):
    code1 = simulate_on_ltspice_norm(filepath, filename, maxtime)
    print("norm code:" + str(code1))
    code2 = simulate_on_ltspice_alt(filepath, filename, maxtime)
    print("alt code:" + str(code2))
    compare_code = -1
    raw_file_path1 = "./test/norm/" + filename[:-4] + ".raw"
    raw_file_path2 = "./test/alt/" + filename[:-4] + ".raw"
    if code1 == 0 and code2 == 0:
        if os.path.exists(raw_file_path1) and os.path.exists(raw_file_path2):
            compare_code = compare_ltspice_raw_file(raw_file_path1, raw_file_path2, error)
        else:
            compare_code = 2
    print("compare code:" + str(compare_code))
    with open("./test/output.txt", "w") as f:
        f.write("code1:" + str(code1) + "\n")
        f.write("code2:" + str(code2) + "\n")
        f.write("code3:" + str(compare_code) + "\n")
    f.close()
    return code1, code2, compare_code
