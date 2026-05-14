import os
import re
import heapq
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
        node1 = heapq.heappop(heap)
        node2 = heapq.heappop(heap)
        merged = HuffmanNode(None, node1.freq + node2.freq)
        merged.left = node1
        merged.right = node2
        heapq.heappush(heap, merged)
    
    return heap[0]

def get_huffman_codes(node, prefix="", codes={}):
    if node:
        if node.char is not None:
            codes[node.char] = prefix
        get_huffman_codes(node.left, prefix + "0", codes)
        get_huffman_codes(node.right, prefix + "1", codes)
    return codes

def serialize_tree(node, bits=""):
    """
    Serializes tree: 0 for internal node, 1 for leaf followed by 8 bits of symbol.
    """
    if node.char is not None:
        return bits + "1" + format(node.char, '08b')
    else:
        bits += "0"
        bits = serialize_tree(node.left, bits)
        bits = serialize_tree(node.right, bits)
        return bits

def compress_site(html_path, css_path, output_path):
    # 1. Load Data
    with open(html_path, "r", encoding="utf-8") as f: html = f.read()
    with open(css_path, "r", encoding="utf-8") as f: css = f.read()
    text_data = html + "\n" + css
    
    print(f"Original Size: {len(text_data)} bytes")

    # 2. Dynamic Tokenization
    dictionary = []
    # We use non-printable placeholders for internal processing
    placeholders = [chr(i) for i in range(1, 32)] + [chr(i) for i in range(128, 256)]
    current_text = text_data
    
    for i in range(128):
        candidates = Counter()
        # Find substrings only in non-placeholder regions
        if i == 0:
            valid_segments = [current_text]
        else:
            pattern = "[" + "".join(re.escape(p) for p in placeholders[:i]) + "]"
            valid_segments = re.split(pattern, current_text)
        
        for segment in valid_segments:
            if len(segment) < 4: continue
            for length in range(4, 31):
                for j in range(len(segment) - length + 1):
                    substring = segment[j:j+length]
                    candidates[substring] += 1

        best_gain = -1
        best_token = None
        for substring, count in candidates.items():
            gain = (count * (len(substring) - 1)) - len(substring)
            if gain > best_gain:
                best_gain = gain
                best_token = substring

        if not best_token or best_gain <= 0: break
        
        dictionary.append(best_token.encode('utf-8'))
        current_text = current_text.replace(best_token, placeholders[i])
        if (i+1) % 20 == 0: print(f"Found {i+1} tokens...")

    # Convert placeholders back to byte values 128-255 for the huffman alphabet
    final_payload = []
    placeholder_map = {p: 128 + i for i, p in enumerate(placeholders[:len(dictionary)])}
    
    for char in current_text:
        if char in placeholder_map:
            final_payload.append(placeholder_map[char])
        else:
            final_payload.append(ord(char))

    # 3. Huffman Encoding
    freqs = Counter(final_payload)
    tree = build_huffman_tree(freqs)
    codes = get_huffman_codes(tree)
    
    # 4. Build Binary Output
    def bits_to_bytes(bit_str):
        res = bytearray()
        for i in range(0, len(bit_str), 8):
            b = bit_str[i:i+8].ljust(8, '0')
            res.append(int(b, 2))
        return res

    with open(output_path, "wb") as f:
        f.write(b"NHF1") 
        f.write(bytes([len(dictionary)]))
        for item in dictionary:
            f.write(bytes([len(item)]))
            f.write(item)
            
        tree_bits = serialize_tree(tree)
        f.write(len(tree_bits).to_bytes(2, 'little'))
        f.write(bits_to_bytes(tree_bits))
        
        payload_bits = "".join(codes[b] for b in final_payload)
        f.write(len(payload_bits).to_bytes(4, 'little'))
        f.write(bits_to_bytes(payload_bits))

    print(f"Compression Complete! Output: {output_path}")
    print(f"Final ROM File Size: {os.path.getsize(output_path)} bytes")

if __name__ == "__main__":
    compress_site(
        os.path.join(ROOT, "site-to-serve", "index.html"),
        os.path.join(ROOT, "site-to-serve", "style.css"),
        os.path.join(ROOT, "build", "compressed_site.bin")
    )
