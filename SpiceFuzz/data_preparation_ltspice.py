import os
import time
import shutil
import argparse
import subprocess

# data_preparation_ltspice.py extract netlist files from raw data and create a seed netlist pool.
if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument("--raw_data_path", type=str, help="Raw data directory path",
                        default="./data/raw_data/ltspice_demo_circuit")
    parser.add_argument("--seed_netlist_path", type=str, help="Seed netlist directory path",
                        default="./data/seed_netlist/ltspice_demo_circuit")
    parser.add_argument('--maxtime', type=int, help="Maximum time during simulation",
                        default=60)
    args = parser.parse_args()
    if not os.path.exists(args.raw_data_path):
        print("Please check the raw_data_path!")
        exit(-1)
    if not os.path.exists(args.seed_netlist_path):
        os.makedirs(args.seed_netlist_path)
    workdir = "./workdir"
    for root, dirs, files in os.walk(args.raw_data_path):
        for filename in files:
            if filename.endswith(".asc"):
                if not os.path.exists(workdir):
                    os.makedirs(workdir)
                filepath = os.path.join(root, filename)
                filepath_new = os.path.join(workdir, filename)
                shutil.copy(str(filepath), filepath_new)
                cmd = "ltspice -netlist " + filename
                try:
                    result = subprocess.run(cmd, cwd=workdir, timeout=args.maxtime, shell=True)
                    if result.returncode == 0:
                        cmd = "ltspice -b " + filename[:-4] + ".net"
                        try:
                            result = subprocess.run(cmd, cwd=workdir, timeout=args.maxtime, shell=True)
                            if result.returncode == 0:
                                netlist_filepath = os.path.join(workdir, filename[:-4] + ".net")
                                with open(netlist_filepath, 'r') as f:
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
                                if len(new_lines) < 100:
                                    with open(netlist_filepath, 'w') as f:
                                        for line in new_lines:
                                            f.write(line + "\n")
                                    f.close()
                                    new_filename = time.strftime('%Y%m%d%H%M%S', time.localtime()) + ".net"
                                    shutil.copy(netlist_filepath, os.path.join(args.seed_netlist_path, new_filename))
                        except subprocess.TimeoutExpired:
                            cmd = "TASKKILL /F /IM ltspice.exe /T"
                            subprocess.run(cmd, shell=True)
                except subprocess.TimeoutExpired:
                    cmd = "TASKKILL /F /IM ltspice.exe /T"
                    subprocess.run(cmd, shell=True)
                time.sleep(60)
                shutil.rmtree(workdir)
