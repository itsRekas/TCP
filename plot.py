import matplotlib.pyplot as plt

x = []
y1 = []  # CWND values
y2 = []  # ss_thresh values

with open("CWND.csv", "r") as fp:
    # Skip header line
    fp.readline()
    for line in fp:
        data = line.strip().split(",")
        x.append(float(data[0]))  # Time
        y1.append(float(data[1]))  # CWND
        y2.append(int(data[2]))    # ss_thresh

# Create figure with two y-axes
fig, ax1 = plt.subplots(figsize=(10, 6))
ax2 = ax1.twinx()

# Plot CWND on left y-axis (green line)
ax1.plot(x, y1, "g-", linewidth=2, label="CWND")
ax1.set_ylim([0, max(y1) * 1.1])  # Add 10% padding

# Plot ss_thresh on right y-axis (red dashed line)
ax2.step(x, y2, "r--", linewidth=2, label="ss_thresh")
ax2.set_ylim([0, max(y2) * 1.1])  # Set based on max ss_thresh

# Set labels and colors
ax1.set_ylabel("Window Size (num. packets)", color="g")
ax1.set_xlabel("Time (seconds)")
ax2.set_ylabel("ss_thresh (num. packets)", color="r")

# Color the tick labels
ax1.tick_params(axis='y', colors='g')
ax2.tick_params(axis='y', colors='r')

# Add grid
ax1.grid(True, alpha=0.3)

# Add title
plt.title("Congestion Window (CWND) and Slow-start Threshold Over Time")

# Add legend
lines1, labels1 = ax1.get_legend_handles_labels()
lines2, labels2 = ax2.get_legend_handles_labels()
ax1.legend(lines1 + lines2, labels1 + labels2, loc='upper right')

# Save figure
plt.savefig("cwnd.pdf", bbox_inches='tight')

# Show plot (optional - comment out if you just want to save without displaying)
plt.show()