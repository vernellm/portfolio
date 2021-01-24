#!/usr/bin/python3
# Autolab autograder for CSCI 2467 (M. Toups / Fall 2018)

import subprocess # to launch tests
import re # regular expressions please
import difflib # to show differences
import signal # to make sure grading script doesn't get stopped by SIGTSTP
import sys

# list of trace files to test on
tracefiles =  ["trace01", "trace02", "trace03",
               "trace04", "trace05", "trace06",
               "trace07", "trace08", "trace09",
               "trace10", "trace11", "trace12",
               "trace13", "trace14", "trace15",
               "trace16", "trace17", "trace18",
               "trace19", "trace20"]
grades = {} # use a dict for results
test_max_time = 25 # maximum number of seconds for each test before timeout

signal.signal(signal.SIGTSTP, signal.SIG_IGN) # ignore SIGTSTP (does not affect children, but helps if their buggy code sends us a SIGTSTP)

for trace in tracefiles:
    try:
        studentoutput = subprocess.check_output(['./sdriver.pl -t ' + str(trace) + ".txt -s ./tsh -a '-p' "], timeout=test_max_time, shell=True)
        refoutput = subprocess.check_output(['./sdriver.pl -t ' + str(trace) + ".txt -s ./tshref -a '-p' "], timeout=test_max_time, shell=True)

        sout_mod = re.sub("[\(].*?[\)]", "(PID)", studentoutput.decode("utf-8")) # get rid of PIDS
        sout_mod = re.sub(r'\s+$', '', sout_mod, flags=re.M) # remove any trailing whitespace
        rout_mod = re.sub("[\(].*?[\)]", "(PID)", refoutput.decode("utf-8")) # get rid of PIDS
        rout_mod = re.sub(r'\s+$', '', rout_mod, flags=re.M) # remove any trailing whitespace

        if trace == "trace12" or trace == "trace13" or trace == "trace14": # for these tests, remove PIDs from ps output
            sout_mod = re.sub("(.\d+\s+\d+)\s+(\w.+\s+\w.\s+.*)", r" PID PID \2 ", sout_mod, flags=re.M) # add a dot in case autograder user gets truncated with a +
            rout_mod = re.sub("(.\d+\s+\d+)\s+(\w.+\s+\w.\s+.*)", r" PID PID \2 ", rout_mod, flags=re.M) 
        if trace == "trace15" or trace == "trace16": # ignore trailing period or trailing space
            sout_mod = re.sub("\.*\s*$","",sout_mod,flags=re.M)
            rout_mod = re.sub("\.*\s*$","",rout_mod,flags=re.M)

        if sout_mod.lower() == rout_mod.lower(): # compare them case-insensitively
            print(trace + " outputs matched (2 points)") # SUCCESS
            grades[trace] = 2
        else:
            diffobj = difflib.Differ() # make a nice diff-style output for students to see what didn't match
            diff = diffobj.compare(sout_mod.splitlines(keepends=True), rout_mod.splitlines(keepends=True))
            grades[trace] = 0
            print("------------ ERROR: " + trace + " outputs DIFFER (0 points) ------------------------")
            sys.stdout.writelines(list(diff))
            print("\n------------ end differences for " + trace + " ------------------------------------")
    except subprocess.CalledProcessError as e:
        grades[trace] = 0
        print('Failed test: ' + str(trace))
        print(e.output.decode("utf-8"))
    except subprocess.TimeoutExpired:
        grades[trace] = 0
        print('Failed test: ' + str(trace) + ' (Test timed out)')

print("\n\n====  SHELL LAB SCORE SUMMARY  ====\n")
for trace in tracefiles:
    if grades[trace] == 2:
        print("[*PASSED*] (2) " + trace)
    else:
        print("[ FAILED ] (0) " + trace)

print("\n\n=== SHELL LAB SCORE: " + str(sum(grades.values())) + " ===\n")
