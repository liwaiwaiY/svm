import socket, struct

s = socket.socket()
s.connect(("127.0.0.1", 8080))

out_data=struct.pack("<II", 0x1000,4)
header=struct.pack("!IIII", 0, 0, len(out_data), 0)
s.sendall(header + out_data)

out_data=struct.pack("<II", 0x1000,4)
header=struct.pack("!IIII", 0, 0, len(out_data), 0)
s.sendall(header + out_data)

s.close()