import os
import time
import shutil
import argparse
import subprocess

# data_preparation_ngspice.py extract netlist files from raw data and create a seed netlist pool.
if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument("--raw_data_path", type=str, help="Input directory of data preparation",
                        default="./data/raw_data/ngspice (regression) test circuits")
    parser.add_argument("--seed_netlist_path", type=str, help="Output directory of data preparation",
                        default="./data/seed_netlist/ngspice (regression) test circuits")
    parser.add_argument('--maxtime', type=int, help="Maximum time to use subprocess",
                        default=60)
    args = parser.parse_args()
    if not os.path.exists(args.raw_data_path):
        print("Please check the raw_data_path!")
        exit(-1)
    if not os.path.exists(args.seed_netlist_path):
        os.makedirs(args.seed_netlist_path)
    workdir = "./work"
    if not os.path.exists(workdir):
        os.makedirs(workdir)
    for root, dirs, files in os.walk(args.raw_data_path):
        for filename in files:
            if filename.endswith(".cir") or filename.endswith(".net"):
                path = os.path.join(root, filename)
                new_path = os.path.join(workdir, filename)
                shutil.copy(str(path), new_path)
                flag = -1
                cmd = "ngspice -b -r " + filename[:-4] + ".raw -o " + filename[:-4] + ".log " + filename
                try:
                    result = subprocess.run(cmd, cwd=workdir, timeout=args.maxtime, shell=True)
                    if result.returncode == 0:
                        flag = 0
                    else:
                        flag = 1
                except subprocess.TimeoutExpired:
                    cmd = "TASKKILL /F /IM ngspice.exe /T"
                    subprocess.run(cmd, shell=True)
                    flag = 2
                if flag == 0 and os.path.exists(os.path.join(workdir, filename[:-4] + ".raw")):
                    filepath = os.path.join(workdir, filename)
                    with open(filepath, 'r') as f:
                        lines = f.readlines()
                    f.close()
                    new_lines = [lines[0].strip()]
                    for i in range(1, len(lines)):
                        line = lines[i].strip()
                        if len(line) == 0:
                            continue
                        if line.startswith("*"):
                            continue
                        index = line.find(";")
                        if index != -1:
                            line = line[:index].strip()
                        new_lines.append(line)
                    if len(new_lines) <= 100:
                        with open(filepath, 'w') as f:
                            for line in new_lines:
                                f.write(line + "\n")
                        f.close()
                        new_filename = time.strftime('%Y%m%d%H%M%S', time.localtime()) + ".cir"
                        shutil.copy(filepath, os.path.join(args.seed_netlist_path, new_filename))
    shutil.rmtree(workdir)
