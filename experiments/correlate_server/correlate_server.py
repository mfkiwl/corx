#!/usr/bin/env python3

import fcntl
import os
import re
import select
import subprocess
import sys
import time
from collections import deque

assert sys.version_info > (3,0), "Please use Python 3"

# Constants
CORX_DIR = "/tmp/uploads/"
CORR_DIR = "/tmp/corr/"
CORRELATOR = "../../src/correlate.py"
NUM_WORKERS = 4
GROUP_SPAN = 10  # maximum difference in timestamp within a group (/2)
STALE_TIMEOUT = 40
# FILENAME_REGEX = r"^[^_]+_[^_]+_([^_]+)_rx([^_]+).corx$"

# Global state
poller = select.epoll()
work_queue = deque([])
running_procs = {}
groups = {}


def run_task(task):
    group_key, filename1, filename2 = task
    path1 = os.path.join(CORX_DIR, filename1)
    path2 = os.path.join(CORX_DIR, filename1)

    rxid1 = extract_rxid(filename1)
    rxid2 = extract_rxid(filename2)
    noise_type = extract_noise_type(filename1)
    output_filename = "corr_{}_{}_{}-{}.npz".format(group_key, noise_type,
                                                    rxid1, rxid2)
    output_path = os.path.join(CORR_DIR, output_filename)
    command = [CORRELATOR, path1, path2, "-o", output_path]
    # print(command)
    # command = ['sleep', '5']

    proc = subprocess.Popen(command, stdout=subprocess.PIPE)
    fd = proc.stdout.fileno()
    running_procs[fd] = (task, proc)
    poller.register(proc.stdout, select.EPOLLHUP)

    print("Executing task: ({}, {}, {}) [PID: {}]"
          .format(group_key, rxid1, rxid2, proc.pid))


def process_queue():
    while len(work_queue) > 0 and len(running_procs) < NUM_WORKERS:
        task = work_queue.popleft()
        run_task(task)


def add_corr_task(group_key, filename1, filename2):
    work_queue.append((group_key, filename1, filename2))


def get_group_key(path):
    mtime = int(os.path.getmtime(path))  # getmtime returns a float timestamp
    if len(groups) == 0:
        return mtime
    keys = list(groups.keys())
    diffs = [abs(group - mtime) for group in keys]
    nearest = min(range(len(groups)), key=diffs.__getitem__)
    if diffs[nearest] <= GROUP_SPAN:
        return keys[nearest]
    else:
        return mtime


def _extract_filename_field(filename, idx):
    fields_str = os.path.splitext(filename)[0]
    fields = fields_str.split('_')
    if len(fields) < 4:
        return None
    return fields[idx]


def extract_rxid(filename):
    return _extract_filename_field(filename, 3)


def extract_noise_type(filename):
    return _extract_filename_field(filename, 2)


def add_corx(filename):
    path = os.path.join(CORX_DIR, filename)
    if not os.path.isfile(path):
        print("Skipping {}: not a file or file does not exist".format(filename))
        return

    group_key = get_group_key(path)
    is_new_group = group_key not in groups
    if is_new_group:
        groups[group_key] = []

    rxid = extract_rxid(filename)
    if rxid is None:
        print("Skipping {}: invalid filename".format(filename))
        return

    print("Add {} to group {}{}".format(rxid, group_key,
                                        " (new group)" if is_new_group else ""))

    for filename2 in groups[group_key]:
        rxid2 = extract_rxid(filename2)
        print("New task: ({}, {}, {})".format(
              group_key, rxid, rxid2))
        add_corr_task(group_key, filename, filename2)

    groups[group_key].append(filename)
    process_queue()
    purge_stale(group_key)


def purge_stale(latest):
    work_queue_groups = [x[0] for x in work_queue]
    process_groups = [x[0][0] for x in running_procs.values()]

    deleted = []
    for group_key, filenames in groups.items():
        if group_key > latest - STALE_TIMEOUT:
            continue
        print("Purge group", group_key)
        if group_key in work_queue_groups:
            print(" ... cannot purge: group is in work queue")
            continue
        if group_key in process_groups:
            print(" ... cannot purge: group is in use by a running process")
            continue
        for filename in filenames:
            print("Delete", filename)
            path = os.path.join(CORX_DIR, filename)
            os.remove(path)
        deleted.append(group_key)
    for group_key in deleted:
        groups.pop(group_key)


def _main():
    # do not block stdin on read
    stdin_fd = sys.stdin.fileno()
    fcntl_flags = fcntl.fcntl(stdin_fd, fcntl.F_GETFL)
    fcntl.fcntl(stdin_fd, fcntl.F_SETFL, fcntl_flags | os.O_NONBLOCK)

    # register stdin
    poller.register(stdin_fd, select.EPOLLIN)

    # wait for stdin and child processes
    while True:
        for fd, _ in poller.poll():
            if fd == stdin_fd:
                print("Received new input")
                while True:
                    line = sys.stdin.readline()
                    if not line:
                        break
                    add_corx(line.strip('\n'))
                    # TODO: check EOF
            else:
                task, proc = running_procs[fd]
                poller.unregister(fd)
                running_procs.pop(fd)

                group_key, filename1, filename2 = task
                rxid1 = extract_rxid(filename1)
                rxid2 = extract_rxid(filename2)
                print("Task done: ({}, {}, {}) [PID: {}]"
                      .format(group_key, rxid1, rxid2, proc.pid))

                process_queue()
                print("Tasks: {} running; {} in queue".format(len(running_procs),
                                                              len(work_queue)))

        print("\nWaiting for new events...")


if __name__ == '__main__':
    _main()