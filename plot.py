import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
import sys
from argparse import ArgumentParser

def plot_cwnd():
    """
    Plot the congestion window size over time.
    Identifies different congestion control phases (Slow Start, Congestion Avoidance, Fast Retransmit).
    """
    # Read CWND data
    cwnd_data = pd.read_csv('CWND.csv')
    
    # Create figure and axis
    fig, ax = plt.subplots(figsize=(10, 6))
    
    # Plot CWND over time
    ax.plot(cwnd_data['time'], cwnd_data['cwnd'], 'b-', linewidth=2, marker='o', markersize=3)
    
    # Detect potential congestion events (significant drops in CWND)
    cwnd_data['drop'] = cwnd_data['cwnd'].diff() < -0.5
    potential_drops = cwnd_data[cwnd_data['drop']].index.tolist()
    
    # Mark potential fast retransmit events
    for idx in potential_drops:
        if idx > 0:  # Skip the first point
            ax.axvline(x=cwnd_data.iloc[idx]['time'], color='r', linestyle='--', alpha=0.7)
            ax.annotate('Fast\nRetransmit', 
                        xy=(cwnd_data.iloc[idx]['time'], cwnd_data.iloc[idx]['cwnd']),
                        xytext=(cwnd_data.iloc[idx]['time']+0.5, cwnd_data.iloc[idx]['cwnd']+2),
                        arrowprops=dict(arrowstyle="->", color='r'))
    
    # Try to identify slow start and congestion avoidance phases
    cwnd_data['growth_rate'] = cwnd_data['cwnd'].diff() / cwnd_data['time'].diff()
    
    # Regions with high growth rate are likely in slow start
    slow_start_mask = (cwnd_data['growth_rate'] > 0.5) & (cwnd_data['cwnd'] < 10)
    cong_avoid_mask = (cwnd_data['growth_rate'] > 0) & (cwnd_data['growth_rate'] < 0.5) & (cwnd_data['cwnd'] > 2)
    
    # Color the background for different phases
    if len(cwnd_data[slow_start_mask]) > 0:
        slow_start_periods = []
        phase_start = None
        
        for i, is_ss in enumerate(slow_start_mask):
            if is_ss and phase_start is None:
                phase_start = i
            elif not is_ss and phase_start is not None:
                slow_start_periods.append((phase_start, i))
                phase_start = None
        
        # Add the last period if it's still open
        if phase_start is not None:
            slow_start_periods.append((phase_start, len(slow_start_mask)-1))
        
        # Highlight slow start periods
        for start, end in slow_start_periods:
            if end > start:  # Ensure valid range
                ax.axvspan(cwnd_data.iloc[start]['time'], cwnd_data.iloc[end]['time'], 
                           alpha=0.2, color='green')
                ax.annotate('Slow Start', 
                            xy=(cwnd_data.iloc[start]['time'], cwnd_data.iloc[start]['cwnd']),
                            xytext=(cwnd_data.iloc[start]['time'], cwnd_data.iloc[start]['cwnd']+1),
                            color='green', fontweight='bold')
    
    # Similarly mark congestion avoidance regions
    if len(cwnd_data[cong_avoid_mask]) > 0:
        cong_avoid_periods = []
        phase_start = None
        
        for i, is_ca in enumerate(cong_avoid_mask):
            if is_ca and phase_start is None:
                phase_start = i
            elif not is_ca and phase_start is not None:
                cong_avoid_periods.append((phase_start, i))
                phase_start = None
        
        # Add the last period if it's still open
        if phase_start is not None:
            cong_avoid_periods.append((phase_start, len(cong_avoid_mask)-1))
        
        # Highlight congestion avoidance periods
        for start, end in cong_avoid_periods:
            if end > start:  # Ensure valid range
                ax.axvspan(cwnd_data.iloc[start]['time'], cwnd_data.iloc[end]['time'], 
                           alpha=0.2, color='yellow')
                ax.annotate('Congestion Avoidance', 
                            xy=(cwnd_data.iloc[start]['time'], cwnd_data.iloc[start]['cwnd']),
                            xytext=(cwnd_data.iloc[start]['time'], cwnd_data.iloc[start]['cwnd']+0.5),
                            color='orange', fontweight='bold')
    
    # Set labels and title
    ax.set_xlabel('Time (seconds)')
    ax.set_ylabel('Congestion Window Size (packets)')
    ax.set_title('TCP Congestion Window Size over Time')
    
    # Add grid
    ax.grid(True, linestyle='--', alpha=0.7)
    
    # Add reference line for ssthresh if we can detect it
    if len(potential_drops) > 0:
        # After the first drop, ssthresh is likely set to cwnd/2
        for idx in potential_drops[:1]:  # Use only the first drop
            if idx > 0:
                ssthresh_val = cwnd_data.iloc[idx-1]['cwnd'] / 2
                ax.axhline(y=ssthresh_val, color='purple', linestyle='-.', alpha=0.7)
                ax.annotate(f'ssthresh ≈ {ssthresh_val:.1f}', 
                            xy=(cwnd_data.iloc[idx]['time']+1, ssthresh_val),
                            color='purple')
    
    # Save the figure
    plt.savefig('cwnd_plot.png', dpi=300, bbox_inches='tight')
    plt.show()

def plot_throughput(args):
    """Original throughput plotting function"""
    fig = plt.figure(figsize=(21,3), facecolor='w')
    ax = plt.gca()
    
    # plotting the trace file
    f1 = open(args.trace, "r")
    BW = []
    nextTime = 1000
    cnt = 0
    for line in f1:
        if int(line.strip()) > nextTime:
            BW.append(cnt*1492*8)
            cnt = 0
            nextTime += 1000
        else:
            cnt += 1
    f1.close()
    
    def scale(a):
        return a/1000000.0
    
    ax.fill_between(range(len(BW)), 0, list(map(scale, BW)), color='#D3D3D3')
    
    # plotting throughput
    throughputDL = []
    timeDL = []
    traceDL = open(args.dir+"/"+str(args.name), 'r')
    traceDL.readline()
    tmp = traceDL.readline().strip().split(",")
    bytes = int(tmp[1])
    startTime = float(tmp[0])
    stime = float(startTime)
    
    for time in traceDL:
        if (float(time.strip().split(",")[0]) - float(startTime)) <= 1.0:
            bytes += int(time.strip().split(",")[1])
        else:
            throughputDL.append(bytes*8/1000000.0)
            timeDL.append(float(startTime)-stime)
            bytes = int(time.strip().split(",")[1])
            startTime += 1.0
    
    print(timeDL)
    print(throughputDL)
    plt.plot(timeDL, throughputDL, lw=2, color='r')
    plt.ylabel("Throughput (Mbps)")
    plt.xlabel("Time (s)")
    plt.grid(True, which="both")
    plt.savefig(args.dir+'/throughput.pdf', dpi=1000, bbox_inches='tight')

if __name__ == "__main__":
    parser = ArgumentParser(description="TCP Congestion Control Visualization")
    parser.add_argument('--mode', '-m', 
                        help="Mode: 'cwnd' for congestion window or 'throughput' for throughput plotting",
                        default='cwnd')
    parser.add_argument('--dir', '-d',
                        help="Directory to store outputs",
                        default='.')
    parser.add_argument('--name', '-n',
                        help="name of the experiment",
                        default='experiment')
    parser.add_argument('--trace', '-tr',
                        help="name of the trace",
                        default='trace.txt')
    
    args = parser.parse_args()
    
    if args.mode == 'cwnd':
        plot_cwnd()
    else:
        plot_throughput(args)