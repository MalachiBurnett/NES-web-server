import os
import io
import mimetypes
from http.server import BaseHTTPRequestHandler, HTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

class HuffmanNode:
    def __init__(self, char, freq):
        self.char = char
        self.freq = freq
        self.left = None
        self.right = None

def unpack_7bit_string(packed_bytes, length):
    bit_str = "".join(format(b, '08b') for b in packed_bytes)
    chars = []
    for i in range(length):
        chars.append(chr(int(bit_str[i*7:(i+1)*7], 2)))
    return "".join(chars)

def deserialize_tree(bits, index):
    if index[0] >= len(bits): return None
    if bits[index[0]] == '1':
        index[0] += 1
        char_bits = bits[index[0]:index[0]+8]
        index[0] += 8
        return HuffmanNode(int(char_bits, 2), 0)
    else:
        index[0] += 1
        node = HuffmanNode(None, 0)
        node.left = deserialize_tree(bits, index)
        node.right = deserialize_tree(bits, index)
        return node

def decompress_packet(data_bytes):
    # data_bytes should start exactly at NHF1
    if data_bytes[0:4] != b"NHF1":
        raise ValueError("Invalid magic bytes")
    
    idx = 4
    num_tokens = data_bytes[idx]
    idx += 1
    
    tokens = []
    for _ in range(num_tokens):
        t_len = data_bytes[idx]
        idx += 1
        packed_len = (t_len * 7 + 7) // 8
        packed_bytes = data_bytes[idx:idx+packed_len]
        idx += packed_len
        tokens.append(unpack_7bit_string(packed_bytes, t_len))
        
    tree_bits_len = int.from_bytes(data_bytes[idx:idx+2], 'little')
    idx += 2
    
    tree_bytes_len = (tree_bits_len + 7) // 8
    tree_bytes = data_bytes[idx:idx+tree_bytes_len]
    idx += tree_bytes_len
    
    tree_bits_str = "".join(format(b, '08b') for b in tree_bytes)[:tree_bits_len]
    
    ptr = [0]
    root = deserialize_tree(tree_bits_str, ptr)
    
    orig_len = int.from_bytes(data_bytes[idx:idx+2], 'little')
    idx += 2
    
    payload_bit_len = int.from_bytes(data_bytes[idx:idx+4], 'little')
    idx += 4
    
    payload_bytes_len = (payload_bit_len + 7) // 8
    payload_bytes = data_bytes[idx:idx+payload_bytes_len]
    idx += payload_bytes_len
    
    payload_bits_str = "".join(format(b, '08b') for b in payload_bytes)[:payload_bit_len]
    
    # Huffman decode
    decoded_bytes = []
    curr = root
    for bit in payload_bits_str:
        if bit == '0':
            curr = curr.left
        else:
            curr = curr.right
        
        if curr.char is not None:
            decoded_bytes.append(curr.char)
            curr = root
            
    if len(decoded_bytes) != orig_len:
        print(f"Warning: Decoded length {len(decoded_bytes)} does not match expected {orig_len}")
        
    # Detokenize
    final_chars = []
    for b in decoded_bytes:
        if 128 <= b < 128 + len(tokens):
            final_chars.append(tokens[b - 128])
        else:
            final_chars.append(chr(b))
            
    return "".join(final_chars), idx

def extract_files_from_rom(rom_path):
    with open(rom_path, 'rb') as f:
        rom_data = f.read()
        
    files = {}
    file_names = ["index.html", "style.css"]
    
    # Find all NHF1 headers
    offsets = []
    start = 0
    while True:
        pos = rom_data.find(b"NHF1", start)
        if pos == -1: break
        offsets.append(pos)
        start = pos + 4
        
    if len(offsets) < len(file_names):
        print("Warning: Did not find enough files in ROM.")
        
    for i, offset in enumerate(offsets):
        if i >= len(file_names): break
        name = file_names[i]
        try:
            content, packet_len = decompress_packet(rom_data[offset:])
            files[name] = content
            print(f"Extracted {name} from ROM offset {hex(offset)} (size: {packet_len} bytes)")
        except Exception as e:
            print(f"Failed to extract {name}: {e}")
            
    return files

class NESServer(BaseHTTPRequestHandler):
    files = {}

    def do_GET(self):
        path = self.path
        if path == "/": path = "/index.html"
        path = path.lstrip("/")
        
        if path in self.files:
            self.send_response(200)
            mime_type, _ = mimetypes.guess_type(path)
            self.send_header('Content-type', mime_type or 'text/html')
            self.send_header('Cache-Control', 'no-store, no-cache, must-revalidate, max-age=0')
            self.send_header('Pragma', 'no-cache')
            self.send_header('Expires', '0')
            self.end_headers()
            self.wfile.write(self.files[path].encode('utf-8'))
        else:
            self.send_response(404)
            self.send_header('Content-type', 'text/html')
            self.end_headers()
            self.wfile.write(b"404 Not Found")
            
    def log_message(self, format, *args):
        # Suppress verbose logging
        pass

def start_server():
    rom_path = os.path.join(ROOT, "build", "nes_web_server.nes")
    if not os.path.exists(rom_path):
        print(f"ROM not found at {rom_path}")
        return
        
    print("Extracting files from ROM...")
    NESServer.files = extract_files_from_rom(rom_path)
    
    server_address = ('', 3002)
    httpd = HTTPServer(server_address, NESServer)
    print("Starting NES ROM Emulator server on port 3002...", flush=True)
    print("Visit http://localhost:3002 in your browser.", flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down server.")
        httpd.server_close()

if __name__ == "__main__":
    start_server()
