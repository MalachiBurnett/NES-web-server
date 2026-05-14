import os
import re
import heapq
import subprocess
from collections import Counter

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

def build_huffman_tree(frequencies):
    heap = [HuffmanNode(char, freq) for char, freq in frequencies.items()]
    heapq.heapify(heap)
    while len(heap) > 1:
        n1, n2 = heapq.heappop(heap), heapq.heappop(heap)
        m = HuffmanNode(None, n1.freq + n2.freq)
        m.left, m.right = n1, n2
        heapq.heappush(heap, m)
    return heap[0]

def get_huffman_codes(node, prefix="", codes=None):
    if codes is None: codes = {}
    if node:
        if node.char is not None: codes[node.char] = prefix
        get_huffman_codes(node.left, prefix + "0", codes)
        get_huffman_codes(node.right, prefix + "1", codes)
    return codes

def serialize_tree(node):
    if node.char is not None:
        return "1" + format(node.char, '08b')
    return "0" + serialize_tree(node.left) + serialize_tree(node.right)

def pack_7bit_string(s):
    bits = "".join(format(ord(c) & 0x7F, '07b') for c in s)
    res = bytearray()
    for i in range(0, len(bits), 8):
        b = bits[i:i+8].ljust(8, '0')
        res.append(int(b, 2))
    return res

def build_rom():
    site_dir = os.path.join(ROOT, "site-to-serve")
    files = ["index.html", "style.css"] # Explicit order for ID mapping
    
    file_data = {}
    combined_text = ""
    for filename in files:
        with open(os.path.join(site_dir, filename), "r", encoding="utf-8") as f:
            content = f.read()
            
            # Basic minification: strip unnecessary whitespace
            if filename.endswith(".html"):
                # Remove comments
                content = re.sub(r"<!--.*?-->", "", content, flags=re.DOTALL)
                # Collapse whitespace between tags
                content = re.sub(r">\s+<", "><", content)
                # Trim lines and remove empty ones
                content = "\n".join(line.strip() for line in content.splitlines() if line.strip())
            elif filename.endswith(".css"):
                # Remove comments
                content = re.sub(r"/\*.*?\*/", "", content, flags=re.DOTALL)
                # Strip spaces around symbols
                content = re.sub(r"\s*([\{\}:;,])\s*", r"\1", content)
                # Collapse multiple spaces
                content = re.sub(r"\s+", " ", content).strip()
                
            file_data[filename] = content
            combined_text += content + "\n"

    # 1. Optimize Tokens (Global Dictionary)
    print("Optimizing global dictionary...")
    all_tokens = []
    placeholders = [chr(i+1000) for i in range(128)]
    current_text = combined_text
    for i in range(128):
        candidates = Counter()
        segments = re.split("[" + "".join(re.escape(p) for p in placeholders[:i]) + "]", current_text) if i > 0 else [current_text]
        for segment in segments:
            if len(segment) < 4: continue
            for length in range(4, 31):
                for j in range(len(segment) - length + 1):
                    candidates[segment[j:j+length]] += 1
        best_gain, best_token = -1, None
        for sub, count in candidates.items():
            gain = (count * (len(sub) - 1)) - len(sub)
            if gain > best_gain: best_gain, best_token = gain, sub
        if not best_token or best_gain <= 0: break
        all_tokens.append(best_token)
        current_text = current_text.replace(best_token, placeholders[i])

    # For this site, we found 50 tokens is optimal
    best_tokens = all_tokens[:50]
    
    # 2. Huffman Tree (Global)
    print("Building global Huffman tree...")
    tokenized_contents = {}
    total_freqs = Counter()
    for name, content in file_data.items():
        t_content = content
        for i, token in enumerate(best_tokens):
            t_content = t_content.replace(token, chr(128 + i))
        bytes_list = [ord(c) for c in t_content]
        tokenized_contents[name] = bytes_list
        total_freqs.update(bytes_list)
    
    tree = build_huffman_tree(total_freqs)
    tree_bits = serialize_tree(tree)
    codes = get_huffman_codes(tree)

    # 3. Assemble Header (Dict + Tree)
    header = bytearray(b"NHF1")
    header.append(len(best_tokens))
    for t in best_tokens:
        packed = pack_7bit_string(t)
        header.append(len(t))
        header.extend(packed)
    
    tree_bytes = bytearray()
    for i in range(0, len(tree_bits), 8):
        tree_bytes.append(int(tree_bits[i:i+8].ljust(8, '0'), 2))
    header.extend(len(tree_bits).to_bytes(2, 'little'))
    header.extend(tree_bytes)

    # 4. Generate data.asm
    with open(os.path.join(ROOT, "build", "data.asm"), "w") as f:
        f.write("; --- AUTOMATICALLY GENERATED DATA ---\n\n")
        f.write("LookupTable:\n")
        for i in range(len(files)):
            f.write(f"    .dw Page{i}\n")
        f.write("    .dw Page404\n\n")
        
        total_compressed_bytes = 0
        total_uncompressed_bytes = 0
        for i, filename in enumerate(files):
            f.write(f"Page{i}:\n")
            content = file_data[filename]
            uncompressed_len = len(content.encode('utf-8'))
            total_uncompressed_bytes += uncompressed_len
            
            bits = "".join(codes[b] for b in tokenized_contents[filename])
            payload = bytearray()
            for j in range(0, len(bits), 8):
                payload.append(int(bits[j:j+8].ljust(8, '0'), 2))
            
            # Full Packet = Header + OrigLen (2) + PayloadBitLen (4) + Payload
            full_packet = bytearray(header)
            full_packet.extend(len(tokenized_contents[filename]).to_bytes(2, 'little'))
            full_packet.extend(len(bits).to_bytes(4, 'little'))
            full_packet.extend(payload)
            
            total_compressed_bytes += len(full_packet)
            print(f"File: {filename:12} | Raw: {uncompressed_len:5} B | Compressed: {len(full_packet):5} B ({(len(full_packet)/uncompressed_len)*100:5.1f}%)")
            
            f.write(f"    .dw {len(full_packet)}\n") # Length prefix for NES SendResponse
            # Write in chunks of 16 for readability
            for j in range(0, len(full_packet), 16):
                chunk = full_packet[j:j+16]
                f.write("    .db " + ", ".join(f"${b:02x}" for b in chunk) + "\n")
            f.write("\n")
            
        f.write("Page404:\n    .dw 0 ; Placeholder for now\n")

    print("-" * 40)
    print(f"Uncompressed Total: {total_uncompressed_bytes:5} bytes ({(total_uncompressed_bytes/1024):.2f} KB)")
    print(f"Compressed Total:   {total_compressed_bytes:5} bytes ({(total_compressed_bytes/1024):.2f} KB)")
    if total_uncompressed_bytes > 0:
        savings = (1 - (total_compressed_bytes / total_uncompressed_bytes)) * 100
        print(f"Compression Ratio:  {(total_uncompressed_bytes/total_compressed_bytes):.2f}:1 ({savings:.1f}% saved)")
    print(f"Injection Complete! data.asm generated with {len(best_tokens)} tokens.")
    print("-" * 40)

    # 5. Assemble ROM
    print("Assembling NES ROM...")
    assembler = os.path.join(ROOT, "tools", "assembler", "assemble.exe")
    source = os.path.join(ROOT, "src", "nes", "main.asm")
    output = os.path.join(ROOT, "build", "nes_web_server.nes")
    
    try:
        # Run assembler in the source directory so relative includes work
        source_dir = os.path.dirname(source)
        result = subprocess.run(
            [assembler, "main.asm", os.path.join(ROOT, "build", "nes_web_server.nes")], 
            cwd=source_dir,
            capture_output=True, 
            text=True
        )
        if result.returncode == 0:
            print(f"SUCCESS: ROM built at build/nes_web_server.nes")
            print(result.stdout.strip())
        else:
            print("ERROR: Assembly failed!")
            print(result.stdout)
            print(result.stderr)
    except Exception as e:
        print(f"ERROR: Could not run assembler: {e}")

if __name__ == "__main__":
    build_rom()
