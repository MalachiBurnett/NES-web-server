# Site to Serve - Content Draft

This file contains the text content extracted from [index.html](file:///d:/Projects/GitHub/NES-web-server/site-to-serve/index.html). You can modify and draft the copy here before applying changes back to the HTML.

---

## 1. Console Banner (Top Section)

**Sub-headline:**
```text
this website is hosted on a
```

**Main Brand Name:**
```text
Nintendo Entertainment System
```

---

## 2. Navigation / Console Links

**Link 1 Text:**
```text
Portfolio
```
*Current Target:* `https://portfolio.wiizardsoftware.uk/`

**Link 2 Text:**
```text
Reset
```
*Current Target:* *(empty / reload)*

---

## 3. Info Box: NES Hosting Overview

**Heading:**
```text
Is this website really hosted on an NES? the console released in 1985?
```

```text
Yes! This website is being served directly from the 8-bit memory of a 1985 Nintendo Entertainment System. Because the NES wasn't designed for the internet, we've given it a "translator" - a tiny ESP32 computer that acts as a bridge between the console and the modern web. it takes requests from the internet and turns them into different button presses in the controller port. it then reads the response from the NES and forwards it back to your browser.
```

---

## 4. Cartridge Label

**Cartridge Title:**
```text
NES WEB
```

**Cartridge Version/Sub-label:**
```text
SERVER v1.0
```

**Cartridge Bottom Label:**
```text
HOSTED ON REAL 1985 HARDWARE AND PROGRAMMED IN RAW 6502 ASSEMBLY
```

---

## 5. Info Box: The Hardware Stack

**Heading:**
```text
The Hardware Stack
```

**Paragraph 1:**
```text
The request flows through a Cloudflare Tunnel to a home server, which reverse-proxies the traffic to a USB Serial port. In other words, the requests initially goes to a cloud server which knows to forward the request to my server. my server then sends the request to the "Translator" device i made which connects to the controller port of the NES. the NES is running a custom program i have written that fetches the appropriate file from its memory and then sends it back to the translator which sends it to the server which sends it to the cloud server which sends it to your computer.
```

---

## 6. Info Box: The NHF1 Compression

**Heading:**
```text
The NHF1 Compression
```

**Paragraph 1:**
```text
To fit a modern website into the NES's limited 32KB ROM banks, I developed the NES Huffman Format (NHF1). It uses two layers of compression:
```

**List Items:**
*   **Dictionary Tokenization:**
    ```text
    7-bit Dictionary Tokenization: the most common tokens, which can be anything from a 3 letter word to a longer phrase such as "NES" or "<div class=", are identified and assigned a 7 bit code which can be used to identify them. because ASCII (the set of characters used for english) uses 7 bits for each letter, that phrase becomes 
    ```
*   **Huffman Coding:**
    ```text
    Huffman Coding: A variable-length bit encoding based on character frequency, further shrinking the tokenized data.
    ```

**Paragraph 2:**
```text
When a request arrives, the NES streams the NHF1 data at "Turbo" speeds to the ESP32, which decompressses it in real-time to serve the HTML/CSS back to your browser.
```

---

## 7. Controller Details

**Text:**
```text
Select Start
```
