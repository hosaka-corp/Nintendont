#!/usr/bin/python3

log = []
with open("/tmp/log", "r") as f:
    for line in f:
        if ("want" in line):
            x = line.rstrip("\n").split(" ")

            measured = int(x[0][:-3])
            target = int(x[2][:-2])
            log.append((measured, target))
            print(measured, target)


total_variance = 0
var_log = []
for meas in log:
    variance = meas[0] - meas[1]
    var_log.append(variance)
    total_variance += variance

avg_variance = total_variance / len(log)
print("avg_variance={:2f}us, max_variance={}us over {} sampled disc reads".format(avg_variance, max(var_log), len(log)))
