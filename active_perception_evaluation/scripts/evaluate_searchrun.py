# -*- coding: utf-8 -*-
"""
Created on Wed Jan 14 15:18:39 2015

@author: Thorsten Gedicke
"""

import pylab
import numpy as np
import sys
import os

# These are the "Tableau 20" colors as RGB.
tableau20 = [(31, 119, 180), (174, 199, 232), (255, 127, 14), (255, 187, 120),
             (44, 160, 44), (152, 223, 138), (214, 39, 40), (255, 152, 150),
             (148, 103, 189), (197, 176, 213), (140, 86, 75), (196, 156, 148),
             (227, 119, 194), (247, 182, 210), (127, 127, 127), (199, 199, 199),
             (188, 189, 34), (219, 219, 141), (23, 190, 207), (158, 218, 229)]

# Scale the RGB values to the [0, 1] range, which is the format matplotlib accepts.
for i in range(len(tableau20)):
    r, g, b = tableau20[i]
    tableau20[i] = (r / 255., g / 255., b / 255.)

def readValsFile(d):
    fvals = open(d + "/vals.log")
    vals = []
    val_idx = -1
    for line in fvals:
        fields = line.split('\t')
        if len(fields) < 2:
            vals.append({})
            val_idx += 1
            if val_idx + 1 != int(line):
                print "WARNING: Corrupt vals.log; Got iteration", line, ", expected", val_idx + 1
        elif len(fields) == 2:
            if fields[0] in vals[val_idx].keys():
                print "WARNING: Corrupt vals.log; Duplicate values for", fields[0], "in iteration", val_idx + 1
            vals[val_idx][fields[0]] = float(fields[1])
        else:
            print "WARNING: Corrupt vals.log; Illegal line", line
    fvals.close()
    return vals

def readLabel(d):
    try:
        f = open(d + "/label")
        label = f.readline()[:-1]
        f.close()
    except:
        label = os.path.relpath(d, d+"/..")
    return label

def vallist(vals, key):
    return [d[key] for d in vals if key in d.keys()]

def main():
    if len(sys.argv) < 2:
        print "Need at least one log_dir as argument"
        sys.exit()

    print "Evaluating", sys.argv[1:]

    vals_batch = [readValsFile(log_dir) for log_dir in sys.argv[1:]]
    label_batch = [readLabel(log_dir) for log_dir in sys.argv[1:]]

    # Set up plot
    # Common sizes: (10, 7.5) and (12, 9)
    pylab.figure(facecolor="white", figsize=(12, 9))
    # Remove the plot frame lines. They are unnecessary chartjunk.
    ax = pylab.subplot(111)
    ax.spines["top"].set_visible(False)
    ax.spines["bottom"].set_visible(True)
    ax.spines["right"].set_visible(False)
    ax.spines["left"].set_visible(True)
    # Remove the tick marks; they are unnecessary with the tick lines we just plotted.
    pylab.tick_params(axis="both", which="both", bottom="on", top="off", labelbottom="on", left="off", right="off", labelleft="on")
    # Gridlines
    ax.yaxis.grid(b=True, which='both', color='black', alpha=0.3, ls='--')

    for trial_nr, vals in enumerate(vals_batch):
        label = label=label_batch[trial_nr]
        color = tableau20[trial_nr*20/len(vals_batch)]
        print "\nEvaluating trial: ", label

        # movement time from [i-1]th step to i-th step
        move_times = [0] + vallist(vals, "expected_move_time")
        # probability to see target at step i
        move_gains = [0] + vallist(vals, "gain")
        # clock at step i
        timeline = np.cumsum(move_times)
        # probability to see target at or before step i
        pdone = np.array([1 - np.prod([1 - move_gains[i] for i in range(k+1)]) for k in range(len(move_gains))])

        # compare gains calculated in runtime and by analysis of probability sum difference
        #print np.array([1 - (1-psums[i-1]) / (1-psums[i]) for i in range(1, len(timeline))]) - np.array(vallist(vals, "gain"))

        # plot pdone vs timeline
        pylab.step(timeline, pdone*100, "-", lw=2.0, color=color, label=label, where='post')

        etime = 0;
        for i in range(len(move_times)-1):
            etime += (1.0 - pdone[i]) * move_times[i+1]
        pylab.axvline(etime, ls="--", lw=2.0, color=color)
        print "etime %.2f" % etime

        nbv_sampling_times = [d["nbv_sampling_time"] for d in vals if "nbv_sampling_time" in d.keys()]
        print "nbv_sampling_time min %.2f max %.2f average %.2f" % (np.min(nbv_sampling_times), np.max(nbv_sampling_times), np.mean(nbv_sampling_times))

        path_planning_times = [d["initial_tt_lut_time"] + d["mutual_tt_lut_time"] for d in vals if "initial_tt_lut_time" in d.keys() and "mutual_tt_lut_time" in d.keys()]
        print "path_planning_time min %.2f max %.2f med %.2f" % (np.min(path_planning_times), np.max(path_planning_times), np.median(path_planning_times))

        planning_times = [d["planning_time"] for d in vals if "planning_time" in d.keys()]
        print "planning_time min %.2f max %.2f average %.2f" % (np.min(planning_times), np.max(planning_times), np.mean(planning_times))

    pylab.legend(loc="lower right")
    pylab.xlabel('time [s]', x=1, ha="right")
    pylab.ylabel('P [%]', y=1, va="top")
    pylab.title('Search progress over time', y=1.01, va="bottom")
    pylab.show()

if __name__=="__main__":
    main()