#!/usr/bin/python -u
# -*- coding: utf-8 -*-
"""
Created on Wed Jan 14 15:18:39 2015

@author: Thorsten Gedicke
"""

import numpy as np
import os
import common

class SearchRunSeries:
    def __init__(self, trials):
        self.eTMove = np.array([t.eTMove for t in trials])
        self.eTCpu = np.array([t.eTCpu for t in trials])
        self.tPlanning = np.array([x for t in trials for x in t.tPlanning])
        self.tNbv = np.array([x for t in trials for x in t.tNbv])
        self.tLut = np.array([x for t in trials for x in t.tLut])

def iros0215(N=20):
    script = "iros0215"
    print "*** %s ***" % script
    greedy_trials = []
    planning_trials = []
    for n in range(N):
        logdir = "%s/greedy_%i" % (os.getcwd(), n)
        if not os.path.exists(logdir):
            print "WARNING: aborted at %s" % n
            break
        greedy = common.SearchRun(logdir)
        logdir = "%s/planning_%i" % (os.getcwd(), n)
        if not os.path.exists(logdir):
            print "WARNING: aborted at %s" % n
            break
        planning = common.SearchRun(logdir)
        greedy_trials.append(greedy)
        planning_trials.append(planning)

    greedySeries = SearchRunSeries(greedy_trials)
    planningSeries = SearchRunSeries(planning_trials)
    eTMoveDiff = greedySeries.eTMove - planningSeries.eTMove
    allTNbv = np.concatenate((greedySeries.tNbv, planningSeries.tNbv))
    allTLut = np.concatenate((greedySeries.tLut, planningSeries.tLut))

    print "# Results Greedy:"
    print ">    mean eTMove: %.2f (std: %.2f)" % (np.mean(greedySeries.eTMove), np.std(greedySeries.eTMove))
    print ">     mean eTCpu: %.2f (std: %.2f)" % (np.mean(greedySeries.eTCpu), np.std(greedySeries.eTCpu))
    print "> mean tPlanning: %.2f (std: %.2f)" % (np.mean(greedySeries.tPlanning), np.std(greedySeries.tPlanning))
    print "# Results Planning:"
    print ">    mean eTMove: %.2f (std: %.2f)" % (np.mean(planningSeries.eTMove), np.std(planningSeries.eTMove))
    print ">     mean eTCpu: %.2f (std: %.2f)" % (np.mean(planningSeries.eTCpu), np.std(planningSeries.eTCpu))
    print "> mean tPlanning: %.2f (std: %.2f)" % (np.mean(planningSeries.tPlanning), np.std(planningSeries.tPlanning))
    print "# Extreme diffs (greedy - planning):"
    print ">  max: %f at %i" % (np.max(eTMoveDiff), np.argmax(eTMoveDiff))
    print ">  min: %f at %i" % (np.min(eTMoveDiff), np.argmin(eTMoveDiff))
    print ">", np.argsort(eTMoveDiff)
    print "# Average times:"
    print "> NBV Sampling: %.2f (std: %.2f)" % (np.mean(allTNbv), np.std(allTNbv))
    print "> Trans. Times: %.2f (std: %.2f)" % (np.mean(allTLut), np.std(allTLut))

def main():
    iros0215()

if __name__=="__main__":
    main()
