# Reliable File Transfer System (Custom TCP over UDP)

## Overview
This project implements a **Reliable File Transfer System** using a custom **TCP-like protocol** built on top of **UDP**. It ensures reliable data transfer by handling packet loss, retransmissions, and congestion control. The protocol was tested in real-world network conditions using **Mahimahi**, a network emulation tool.

## Features
- **Reliable Data Transfer:** Implements a **Sliding Window Protocol** for efficient packet management.
- **Cumulative Acknowledgments:** Ensures optimal data transmission and minimizes retransmissions.
- **Fast Retransmit Mechanism:** Handles packet loss efficiently using **duplicate ACK detection**.
- **Congestion Control:** Inspired by **TCP Tahoe**, adapting transmission rates based on network conditions.
- **Mahimahi Testing:** Simulated real-world network conditions to measure protocol performance.

## Technologies Used
- **C**: Low-level implementation for performance optimization.
- **UDP**: Used as the transport layer protocol.
- **Mahimahi**: Network emulation for testing different packet loss and delay scenarios.
- **Linux Sockets API**: Used for handling network communication.

## Project Structure
```
├── rdt_sender.c        # Implements the sender logic
├── rdt_receiver.c      # Implements the receiver logic
├── packet.h            # Defines the packet structure
├── common.h            # Shared constants and utilities
├── Makefile            # Build automation script
└── README.md           # Project documentation
```

## How It Works
1. **Sender:** Reads a file, breaks it into packets, and transmits them over **UDP**.
2. **Receiver:** Receives packets, buffers out-of-order packets, and acknowledges received data.
3. **Sliding Window:** Ensures efficient data transmission and reordering.
4. **Retransmission Mechanism:** If packets are lost, they are **re-sent** based on timeout or **duplicate ACKs**.

## Installation & Usage
### 1. Clone the Repository
```sh
git clone https://github.com/itsRekas/TCP.git
cd TCP
```

### 2. Compile the Code
```sh
make
```

### 3. Run the Receiver (on one terminal)
```sh
./rdt_receiver <port> <output_file>
```
Example:
```sh
./rdt_receiver 8080 received_file.txt
```

### 4. Run the Sender (on another terminal)
```sh
./rdt_sender <hostname> <port> <input_file>
```
Example:
```sh
./rdt_sender 127.0.0.1 8080 sample.txt
```

### 5. Testing with Mahimahi
To simulate real-world network conditions:
```sh
mm-delay 50 mm-loss 1 ./rdt_sender 127.0.0.1 8080 sample.txt
```

## Performance Evaluation
- Successfully transmitted large files over **UDP with zero data corruption**.
- **Reduced packet loss** by implementing an efficient retransmission strategy.
- **Optimized congestion control** through adaptive window sizing.

## Future Improvements
- Implement **Selective Acknowledgment (SACK)** for better loss recovery.
- Enhance congestion control with **TCP Reno** or **Cubic TCP**.
- Add **multi-threading** support for handling multiple connections.

## Authors
**Reginald Kotey Appiah Sekyere** – [GitHub Profile](https://github.com/itsRekas).
**Khaleeqa Aasiyah** – [GitHub Profile]([https://github.com/itsRekas](https://github.com/Khaleeks)).
**

## License
This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

