import heapq
import os
import re

# Project root is one level up from this scripts/ directory
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

class HuffmanNode:
    def __init__(self, char, freq):
        self.char = char
        self.freq = freq
        self.left = None
        self.right = None

    def __lt__(self, other):
        return self.freq < other.freq

def build_huffman_tree(text):
    if not text:
        return None
    
    frequencies = {}
    for char in text:
        frequencies[char] = frequencies.get(char, 0) + 1
    
    # Ensure all possible ASCII/Characters used in 404 etc are here
    # Actually, the user might want a specific set of pages.
    
    priority_queue = [HuffmanNode(char, freq) for char, freq in frequencies.items()]
    heapq.heapify(priority_queue)
    
    while len(priority_queue) > 1:
        node1 = heapq.heappop(priority_queue)
        node2 = heapq.heappop(priority_queue)
        
        merged = HuffmanNode(None, node1.freq + node2.freq)
        merged.left = node1
        merged.right = node2
        heapq.heappush(priority_queue, merged)
        
    return priority_queue[0]

def build_codes(node, current_code, codes):
    if node is None:
        return
    
    if node.char is not None:
        codes[node.char] = current_code
        return
    
    build_codes(node.left, current_code + "0", codes)
    build_codes(node.right, current_code + "1", codes)

def encode_text(text, codes):
    encoded_bits = []
    for char in text:
        for bit in codes[char]:
            encoded_bits.append(1 if bit == '1' else 0)
    return encoded_bits

def serialize_tree_bitstream(node, bits):
    if node.char is not None:
        bits.append(1)
        # character (8 bits)
        char_val = ord(node.char)
        for i in range(7, -1, -1):
            bits.append((char_val >> i) & 1)
    else:
        bits.append(0)
        serialize_tree_bitstream(node.left, bits)
        serialize_tree_bitstream(node.right, bits)

def bits_to_bytes(bits):
    byte_list = []
    for i in range(0, len(bits), 8):
        byte = 0
        chunk = bits[i:i+8]
        for j, bit in enumerate(chunk):
            if bit:
                byte |= (1 << (7 - j))
        byte_list.append(byte)
    return byte_list

def inject_into_asm(asm_path, compressed_pages):
    with open(asm_path, 'r') as f:
        content = f.read()
    
    data_section = "; --- Data ---\nLookupTable:\n"
    
    # 1. Generate LookupTable entries
    labels = []
    for i in range(len(compressed_pages)):
        labels.append(f"Page{i}")
    
    # Pad to 10 entries as in original
    while len(labels) < 10:
        labels.append("0")
    
    # Group in pairs for .dw
    for i in range(0, len(labels), 2):
        data_section += f"    .dw {labels[i]}, {labels[i+1]}\n"
    
    data_section += "\n"
    
    # 2. Generate Page Data
    for i, (name, data) in enumerate(compressed_pages.items()):
        comp_data = data['comp_data']
        orig_len = data['orig_len']
        tree_bytes = data['tree_bytes']
        
        # New structure: [TotalLen_L, TotalLen_H][TreeSize][TreeData...][OrigLen_L, OrigLen_H][CompData...]
        # TotalLen does NOT include the 2 bytes of TotalLen itself.
        total_len = 1 + len(tree_bytes) + 2 + len(comp_data)
        
        total_len_low = total_len & 0xFF
        total_len_high = (total_len >> 8) & 0xFF
        
        tree_hex = ", ".join([f"${b:02x}" for b in tree_bytes])
        comp_hex = ", ".join([f"${b:02x}" for b in comp_data])
        
        orig_len_low = orig_len & 0xFF
        orig_len_high = (orig_len >> 8) & 0xFF
        
        data_section += f"Page{i}: .db ${total_len_low:02x}, ${total_len_high:02x}, ${len(tree_bytes):02x}, {tree_hex}, ${orig_len_low:02x}, ${orig_len_high:02x}, {comp_hex} ; {name}\n"

    # 3. Replace in ASM
    new_content = re.sub(r"; --- Data ---.*?(?=\.pad|\Z)", data_section, content, flags=re.DOTALL)
    
    with open(asm_path, 'w') as f:
        f.write(new_content)
    print(f"Injected into {asm_path}")

def generate_arduino_header(header_path, arduino_nodes, root_idx):
    with open(header_path, 'w') as f:
        f.write("#ifndef HUFFMAN_DATA_H\n#define HUFFMAN_DATA_H\n\n")
        f.write("#include <Arduino.h>\n\n")
        f.write(f"const int16_t HUFFMAN_NODES[][2] PROGMEM = {{\n")
        for left, right in arduino_nodes:
            f.write(f"  {{ {left}, {right} }},\n")
        f.write(f"}};\n\n")
        f.write(f"const int16_t HUFFMAN_ROOT = {root_idx};\n\n")
        f.write("#endif\n")
    print(f"Generated {header_path}")

def main():
    asm_path = os.path.join(ROOT, "src", "nes", "main.asm")
    header_path = os.path.join(ROOT, "src", "firmware", "huffman_data.h")
    
    # Define pages to compress
    pages = {
        "index.html": "<html><head><title>NES Portfolio</title></head><body><h1>My NES Portfolio</h1><p>Welcome to my site hosted on an 8-bit console!</p></body></html>",
        "about.html": "<html><head><title>About</title></head><body><h1>About Me</h1><p>I build weird stuff with old hardware.</p></body></html>",
        "404.html": "<html><body><h1>404</h1><p>Page Not Found</p></body></html>"
    }
    
    # 1. Build global tree
    all_text = "".join(pages.values())
    root = build_huffman_tree(all_text)
    codes = {}
    build_codes(root, "", codes)
    
    # 2. Serialize tree bitstream
    tree_bits = []
    serialize_tree_bitstream(root, tree_bits)
    tree_bytes = bits_to_bytes(tree_bits)
    
    # 3. Compress pages
    compressed_pages = {}
    for name, content in pages.items():
        bits = encode_text(content, codes)
        bytes_data = bits_to_bytes(bits)
        compressed_pages[name] = {
            "orig_len": len(content),
            "comp_data": bytes_data,
            "tree_bytes": tree_bytes
        }
        
    # 4. Inject into NES ASM
    inject_into_asm(asm_path, compressed_pages)

if __name__ == "__main__":
    main()
