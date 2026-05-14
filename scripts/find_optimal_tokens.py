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
        n1, n2 = heapq.heappop(heap), heapq.heappop(heap)
        m = HuffmanNode(None, n1.freq + n2.freq)
        m.left, m.right = n1, n2
        heapq.heappush(heap, m)
    return heap[0]

def get_huffman_codes(node, prefix="", codes={}):
    if node:
        if node.char is not None: codes[node.char] = prefix
        get_huffman_codes(node.left, prefix + "0", codes)
        get_huffman_codes(node.right, prefix + "1", codes)
    return codes

def count_tree_bits(node, symbol_bits=8):
    if node.char is not None:
        return 1 + symbol_bits
    return 1 + count_tree_bits(node.left, symbol_bits) + count_tree_bits(node.right, symbol_bits)

def simulate_compression(text_data, max_tokens_to_test=255):
    # 1. Find the full list of 255 tokens first (Greedy)
    print("Finding 255 best tokens...")
    all_tokens = []
    placeholders = [chr(i+1000) for i in range(max_tokens_to_test)] # Safe placeholders
    current_text = text_data
    
    # Pre-calculate tokens for the sweep
    for i in range(max_tokens_to_test):
        candidates = Counter()
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
        
        best_gain, best_token = -1, None
        for sub, count in candidates.items():
            gain = (count * (len(sub) - 1)) - len(sub)
            if gain > best_gain:
                best_gain, best_token = gain, sub
        
        if not best_token or best_gain <= 0: break
        all_tokens.append(best_token)
        current_text = current_text.replace(best_token, placeholders[i])

    print(f"Found {len(all_tokens)} profitable tokens. Starting sweep...")

    # 2. Sweep
    results = []
    # We'll use 16 bits for symbols if tokens > 128, else 8 bits
    for k in range(1, len(all_tokens) + 1):
        # Re-tokenize with exactly k tokens
        test_text = text_data
        k_tokens = all_tokens[:k]
        for i, t in enumerate(k_tokens):
            test_text = test_text.replace(t, chr(i+1000))
        
        # Build frequency map
        final_payload = [ord(c) if ord(c) < 1000 else 128 + (ord(c)-1000) for c in test_text]
        freqs = Counter(final_payload)
        
        # Calculate Sizes
        dict_size = sum(len(t.encode('utf-8')) + 1 for t in k_tokens) + 1 # +1 for count byte
        
        tree = build_huffman_tree(freqs)
        symbol_bits = 8 if k <= 128 else 16
        tree_bits = count_tree_bits(tree, symbol_bits)
        tree_size = (tree_bits + 7) // 8 + 2 # +2 for bit-count header
        
        codes = get_huffman_codes(tree, "", {})
        payload_bits = sum(freqs[b] * len(codes[b]) for b in freqs)
        payload_size = (payload_bits + 7) // 8 + 4 # +4 for bit-count header
        
        total_size = 4 + dict_size + tree_size + payload_size # 4 for magic
        results.append((k, total_size))

    # Find the winner
    best_k, min_size = min(results, key=lambda x: x[1])
    print("-" * 40)
    for k, size in results[::10]: # Print every 10th
        print(f"Tokens: {k:3} | Total Size: {size:5} bytes")
    print("-" * 40)
    print(f"WINNER: {best_k} tokens | Optimal Size: {min_size} bytes")

if __name__ == "__main__":
    with open(os.path.join(ROOT, "site-to-serve", "index.html"), "r", encoding="utf-8") as f: html = f.read()
    with open(os.path.join(ROOT, "site-to-serve", "style.css"), "r", encoding="utf-8") as f: css = f.read()
    simulate_compression(html + "\n" + css)
