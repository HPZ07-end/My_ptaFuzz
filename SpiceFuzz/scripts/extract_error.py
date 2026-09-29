def extract_error_msg(log_path):
    with open(log_path, 'r') as f:
        log_lines = f.readlines()
    f.close()
    error_msg = ""
    for i in range(len(log_lines)):
        if "error" in log_lines[i]:
            index = log_lines[i].find("error")
            new_index = log_lines[i].find(";")
            if new_index != -1:
                error_msg = log_lines[i][index:new_index]
            else:
                error_msg = log_lines[i][index:]
            break
    return error_msg
