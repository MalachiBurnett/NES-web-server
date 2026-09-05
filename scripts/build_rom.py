import os
import re
import heapq
import subprocess
from collections import Counter

# Project root is one level up from this scripts/ directory
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

MAGIC = b"NHF2"

FILES = ["index.html", "style.css"]   # explicit order for ID mapping
FALLBACK = "404.html"                 # served for any id the ROM has no page for

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
        if node.char is not None: codes[node.char] = prefix or "0"
        get_huffman_codes(node.left, prefix + "0", codes)
        get_huffman_codes(node.right, prefix + "1", codes)
    return codes

def serialize_tree(node):
    if node.char is not None:
        return "1" + format(node.char, '08b')
    return "0" + serialize_tree(node.left) + serialize_tree(node.right)

def pack_bits(bits):
    """MSB first, zero padded to a byte boundary. Both decoders must match."""
    res = bytearray()
    for i in range(0, len(bits), 8):
        res.append(int(bits[i:i+8].ljust(8, '0'), 2))
    return res

def pack_7bit_string(s):
    return pack_bits("".join(format(ord(c) & 0x7F, '07b') for c in s))

def find_tokens(content):
    """Greedily pick repeated substrings worth replacing with a token byte."""
    all_tokens = []
    placeholders = [chr(i + 1000) for i in range(128)]
    current_text = content
    for j in range(128):
        candidates = Counter()
        segments = re.split("[" + "".join(re.escape(p) for p in placeholders[:j]) + "]", current_text) if j > 0 else [current_text]
        for segment in segments:
            if len(segment) < 4: continue
            for length in range(4, 31):
                for k in range(len(segment) - length + 1):
                    candidates[segment[k:k + length]] += 1
        best_gain, best_token = -1, None
        for sub, count in candidates.items():
            gain = (count * (len(sub) - 1)) - len(sub)
            if gain > best_gain: best_gain, best_token = gain, sub
        if not best_token or best_gain <= 0: break
        all_tokens.append(best_token)
        current_text = current_text.replace(best_token, placeholders[j])
    return all_tokens

def encode_packet(content, tokens):
    """One NHF2 packet: header, dictionary, Huffman tree, payload."""
    raw = content.encode('utf-8')

    t_content = raw
    for j, token in enumerate(tokens):
        t_content = t_content.replace(token.encode('utf-8'), bytes([128 + j]))
    symbols = list(t_content)

    tree = build_huffman_tree(Counter(symbols))
    tree_bits = serialize_tree(tree)
    codes = get_huffman_codes(tree)

    packet = bytearray(MAGIC)
    packet.append(len(tokens))
    for t in tokens:
        packet.append(len(t))
        packet.extend(pack_7bit_string(t))

    packet.extend(len(tree_bits).to_bytes(2, 'little'))
    packet.extend(pack_bits(tree_bits))

    bits = "".join(codes[b] for b in symbols)
    packet.extend(len(symbols).to_bytes(2, 'little'))   # symbols to decode
    packet.extend(len(raw).to_bytes(2, 'little'))       # bytes after detokenising
    packet.extend(len(bits).to_bytes(4, 'little'))
    packet.extend(pack_bits(bits))
    return packet

def compress(content, label):
    """Brute force the token count that gives the smallest packet."""
    raw = content.encode('utf-8')
    if any(b >= 128 for b in raw):
        raise ValueError(f"{label}: non-ASCII bytes collide with token ids 128-255")

    print(f"Optimizing dictionary for {label}...")
    all_tokens = find_tokens(content)

    print(f"Brute forcing optimal token count for {label}...")
    best_packet, best_token_count = None, 0
    for token_count in range(len(all_tokens) + 1):
        packet = encode_packet(content, all_tokens[:token_count])
        if best_packet is None or len(packet) < len(best_packet):
            best_packet, best_token_count = packet, token_count

    print(f"File: {label:12} | Raw: {len(raw):5} B | Compressed: {len(best_packet):5} B "
          f"({(len(best_packet) / len(raw)) * 100:5.1f}%) | Optimal Tokens: {best_token_count}")
    return best_packet

def minify(filename, content):
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
    return content

def write_page(f, name, packet):
    f.write(f"{name}:\n")
    f.write(f"    .dw {len(packet)}\n")   # length prefix for NES SendResponse
    # Write in chunks of 16 for readability
    for j in range(0, len(packet), 16):
        chunk = packet[j:j + 16]
        f.write("    .db " + ", ".join(f"${b:02x}" for b in chunk) + "\n")
    f.write("\n")

def load_site():
    """Minified text for every page in the ROM, in lookup table order."""
    site_dir = os.path.join(ROOT, "site-to-serve")
    pages = []
    for name in FILES + [FALLBACK]:
        with open(os.path.join(site_dir, name), "r", encoding="utf-8") as f:
            pages.append((name, minify(name, f.read())))
    return pages

def build_rom():
    pages = load_site()
    packets = [compress(text, name) for name, text in pages]

    # the fallback page is not part of the site's own size figures
    total_uncompressed_bytes = sum(len(t.encode('utf-8')) for _, t in pages[:len(FILES)])
    total_compressed_bytes = sum(len(p) for p in packets[:len(FILES)])

    with open(os.path.join(ROOT, "build", "data.asm"), "w") as f:
        f.write("; --- AUTOMATICALLY GENERATED DATA ---\n\n")
        f.write(f"PageCount = {len(FILES)}\n\n")
        f.write("LookupTable:\n")
        for i in range(len(FILES)):
            f.write(f"    .dw Page{i}\n")
        f.write("    .dw Page404\n\n")

        for i in range(len(FILES)):
            write_page(f, f"Page{i}", packets[i])
        write_page(f, "Page404", packets[-1])

    print("-" * 40)
    print(f"Uncompressed Total: {total_uncompressed_bytes:5} bytes ({(total_uncompressed_bytes / 1024):.2f} KB)")
    print(f"Compressed Total:   {total_compressed_bytes:5} bytes ({(total_compressed_bytes / 1024):.2f} KB)")
    if total_uncompressed_bytes > 0:
        savings = (1 - (total_compressed_bytes / total_uncompressed_bytes)) * 100
        print(f"Compression Ratio:  {(total_uncompressed_bytes / total_compressed_bytes):.2f}:1 ({savings:.1f}% saved)")
    print("Injection Complete! data.asm generated with optimal per-file tokens.")
    print("-" * 40)

    # Assemble ROM
    print("Assembling NES ROM...")
    assembler = os.path.join(ROOT, "tools", "assembler", "assemble.exe")
    source = os.path.join(ROOT, "src", "nes", "main.asm")
    output = os.path.join(ROOT, "build", "nes_web_server.nes")

    try:
        # Run assembler in the source directory so relative includes work
        result = subprocess.run(
            [assembler, "main.asm", output],
            cwd=os.path.dirname(source),
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
