import math
import os
import numpy
import shutil
import subprocess


def simulate_on_ngspice_sparse(filepath, filename, maxtime):
    test_path = "./test/sparse"
    if not os.path.exists(test_path):
        os.makedirs(test_path)
    new_path = os.path.join(test_path, filename)
    shutil.copy(filepath, new_path)
    with open(new_path, "r") as f:
        lines = f.readlines()
    f.close()
    with open(new_path, "w") as f:
        f.write(lines[0].strip() + "\n")
        f.write(".options sparse\n")
        for i in range(1, len(lines)):
            f.write(lines[i].strip() + "\n")
    f.close()
    cmd = "ngspice -b -r " + filename[:-4] + ".raw -o " + filename[:-4] + ".log " + filename
    try:
        result = subprocess.run(cmd, cwd=test_path, timeout=maxtime, shell=True)
        if result.returncode == 0:
            return 0
        else:
            return 1
    except subprocess.TimeoutExpired:
        cmd = "TASKKILL /F /IM ngspice.exe /T"
        subprocess.run(cmd, shell=True)
        return 2


def simulate_on_ngspice_klu(filepath, filename, maxtime):
    test_path = "./test/klu"
    if not os.path.exists(test_path):
        os.makedirs(test_path)
    new_path = os.path.join(test_path, filename)
    shutil.copy(filepath, new_path)
    with open(new_path, "r") as f:
        lines = f.readlines()
    f.close()
    with open(new_path, "w") as f:
        f.write(lines[0].strip() + "\n")
        f.write(".options klu\n")
        for i in range(1, len(lines)):
            f.write(lines[i].strip() + "\n")
    f.close()
    cmd = "ngspice -b -r " + filename[:-4] + ".raw -o " + filename[:-4] + ".log " + filename
    try:
        result = subprocess.run(cmd, cwd=test_path, timeout=maxtime, shell=True)
        if result.returncode == 0:
            return 0
        else:
            return 1
    except subprocess.TimeoutExpired:
        cmd = "TASKKILL /F /IM ngspice.exe /T"
        subprocess.run(cmd, shell=True)
        return 2


def compare_ngspice_raw_file(path1, path2, error):
    with open(path1, "r") as f:
        lines1 = f.readlines()
    f.close()
    with open(path2, "r") as f:
        lines2 = f.readlines()
    f.close()
    variable_num = int(lines1[4].split()[-1])
    new_variable_num = int(lines2[4].split()[-1])
    if new_variable_num != variable_num:
        return 1
    point_num = int(lines1[5].split()[-1])
    new_point_num = int(lines2[5].split()[-1])
    if point_num != new_point_num and point_num == 0:
        return 1
    value_line_no = variable_num + 8
    tokens = lines1[value_line_no].split()[-1].split(",")
    for i in range(variable_num):
        for j in range(point_num):
            for k in range(len(tokens)):
                value1 = numpy.float32(lines1[value_line_no + j * variable_num + i].split()[-1].split(",")[k])
                value2 = numpy.float32(lines2[value_line_no + j * variable_num + i].split()[-1].split(",")[k])
                min_value = min(math.fabs(value1), math.fabs(value2))
                if math.fabs(value1 - value2) > numpy.float32(error) * min_value:
                    return 1
    return 0


def differential_testing(filepath, filename, maxtime, error):
    code1 = simulate_on_ngspice_sparse(filepath, filename, maxtime)
    print("sparse code:" + str(code1))
    code2 = simulate_on_ngspice_klu(filepath, filename, maxtime)
    print("klu code:" + str(code2))
    compare_code = -1
    raw_file_path1 = "./test/sparse/" + filename[:-4] + ".raw"
    raw_file_path2 = "./test/klu/" + filename[:-4] + ".raw"
    if code1 == 0 and code2 == 0:
        if os.path.exists(raw_file_path1) and os.path.exists(raw_file_path2):
            compare_code = compare_ngspice_raw_file(raw_file_path1, raw_file_path2, error)
        else:
            compare_code = 2
    print("compare code:" + str(compare_code))
    with open("./test/output.txt", "w") as f:
        f.write("code1:" + str(code1) + "\n")
        f.write("code2:" + str(code2) + "\n")
        f.write("code3:" + str(compare_code) + "\n")
    f.close()
    return code1, code2, compare_code
