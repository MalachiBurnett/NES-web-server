import os
import re
from collections import Counter

# Project root is one level up from this scripts/ directory
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def optimize_tokens(input_text, max_tokens=128, min_length=3, max_length=30):
    current_text = input_text
    dictionary = []
    total_saved = 0

    print(f"Initial size: {len(input_text)} bytes")
    print("-" * 40)

    # We use characters from 0x01 to 0x1F and 0x80 to 0xFF as placeholders
    # to avoid the script finding patterns in the placeholders themselves.
    placeholders = [chr(i) for i in range(1, 32)] + [chr(i) for i in range(128, 256)]
    
    for i in range(min(max_tokens, len(placeholders))):
        candidates = Counter()
        # Only search for substrings in the parts of the text that aren't placeholders
        if i == 0:
            valid_segments = [current_text]
        else:
            # Create a regex to split by any of the placeholders we've already used
            pattern = "[" + "".join(re.escape(p) for p in placeholders[:i]) + "]"
            valid_segments = re.split(pattern, current_text)
        
        for segment in valid_segments:
            if len(segment) < min_length: continue
            for length in range(min_length, max_length + 1):
                for j in range(len(segment) - length + 1):
                    substring = segment[j:j+length]
                    candidates[substring] += 1

        best_gain = -1
        best_token = None

        for substring, count in candidates.items():
            # Gain = (Count * (Len - 1)) - Len
            gain = (count * (len(substring) - 1)) - len(substring)
            
            if gain > best_gain:
                best_gain = gain
                best_token = substring

        if not best_token or best_gain <= 0:
            print(f"Stopped after {i} tokens. No more profitable patterns found.")
            break

        dictionary.append(best_token)
        total_saved += best_gain
        
        # Replace the best token with the next available placeholder
        current_text = current_text.replace(best_token, placeholders[i])
        
        if (i + 1) % 10 == 0 or i < 10:
            display_token = best_token.replace('\n', '\\n').replace('\r', '\\r')
            print(f"Token {i+1:3}: '{display_token}' | Saved: {best_gain} bytes")

    print("-" * 40)
    print(f"Final Size (estimated): {len(input_text) - total_saved} bytes")
    print(f"Total Saved: {total_saved} bytes ({ (total_saved/len(input_text))*100:.1f}%)")
    return dictionary

if __name__ == "__main__":
    # Load your files
    try:
        with open(os.path.join(ROOT, "site-to-serve", "index.html"), "r", encoding="utf-8") as f:
            html = f.read()
        with open(os.path.join(ROOT, "site-to-serve", "style.css"), "r", encoding="utf-8") as f:
            css = f.read()
            
        # Combine them as the ESP32 would see them (likely served separately but using the same dictionary)
        combined_text = html + "\n" + css
        
        best_tokens = optimize_tokens(combined_text)
        
        # Save results for your ESP32 dictionary
        with open(os.path.join(ROOT, "build", "tokens_found.txt"), "w", encoding="utf-8") as f:
            for i, token in enumerate(best_tokens):
                f.write(f"{128+i}: {token}\n")
                
    except FileNotFoundError as e:
        print(f"Error: {e}")
