#define PIN_NES_DATA_IN  2
#define PIN_NES_CLOCK    3
#define PIN_NES_DATA_OUT 4

struct Node {
  Node *left = nullptr, *right = nullptr;
  uint16_t val = 0; // 0-255, or >255 for internal
  bool isLeaf = false;
};

String dictionary[128];
Node* huffmanRoot = nullptr;
char* responseBuffer = nullptr;
uint32_t bufferSize = 0;

void setup() {
  Serial.begin(115200);
  pinMode(PIN_NES_DATA_IN, INPUT);
  pinMode(PIN_NES_CLOCK, INPUT);
  pinMode(PIN_NES_DATA_OUT, OUTPUT);
  digitalWrite(PIN_NES_DATA_OUT, LOW);
  Serial.println("ESP32: Hybrid-7 Gateway Online.");
}

byte receiveByte() {
  byte data = 0;
  bool lastClock = LOW;
  for (int i = 0; i < 8; i++) {
    while (digitalRead(PIN_NES_CLOCK) == lastClock); // Wait for edge
    data |= (digitalRead(PIN_NES_DATA_IN) << i);
    lastClock = !lastClock;
  }
  return data;
}

// Bit-stream reader for 7-bit and tree bits
struct BitReader {
  byte currentByte;
  int bitPos = 8;
  int bytesRead = 0;
  
  bool readBit() {
    if (bitPos >= 8) {
      currentByte = receiveByte();
      bitPos = 0;
      bytesRead++;
    }
    return (currentByte >> (bitPos++)) & 1;
  }
  
  byte readBits(int n) {
    byte res = 0;
    for (int i = 0; i < n; i++) if (readBit()) res |= (1 << i);
    return res;
  }
};

Node* parseTree(BitReader& br) {
  Node* n = new Node();
  if (br.readBit()) { // Leaf
    n->isLeaf = true;
    n->val = 0;
    for(int i=0; i<8; i++) if(br.readBit()) n->val |= (1 << i);
  } else {
    n->left = parseTree(br);
    n->right = parseTree(br);
  }
  return n;
}

void receiveResponse() {
  // 1. Verify Magic "NHF1"
  char magic[5] = {0};
  for(int i=0; i<4; i++) magic[i] = receiveByte();
  if (strcmp(magic, "NHF1") != 0) return;

  // 2. Load 7-bit Dictionary
  byte dictCount = receiveByte();
  for (int i = 0; i < dictCount; i++) {
    byte len = receiveByte();
    dictionary[i] = "";
    BitReader br; // New bit reader for each string to keep it simple
    for (int j = 0; j < len; j++) dictionary[i] += (char)br.readBits(7);
  }

  // 3. Load Huffman Tree
  uint16_t treeBits = receiveByte() | (receiveByte() << 8);
  BitReader tr;
  if (huffmanRoot) deleteTree(huffmanRoot);
  huffmanRoot = parseTree(tr);

  // 4. Decode Payload
  uint16_t origLen = receiveByte() | (receiveByte() << 8);
  uint32_t payloadBits = receiveByte() | (receiveByte() << 8) | (receiveByte() << 16) | (receiveByte() << 24);
  
  if (responseBuffer) free(responseBuffer);
  responseBuffer = (char*)malloc(origLen + 1);
  uint32_t decodedChars = 0;
  BitReader pr;
  Node* curr = huffmanRoot;

  while (decodedChars < origLen) {
    curr = pr.readBit() ? curr->right : curr->left;
    if (curr->isLeaf) {
      if (curr->val < 128) {
        responseBuffer[decodedChars++] = (char)curr->val;
      } else {
        String token = dictionary[curr->val - 128];
        for (char c : token) responseBuffer[decodedChars++] = c;
      }
      curr = huffmanRoot;
    }
  }
  responseBuffer[decodedChars] = '\0';
  Serial.print("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n");
  Serial.print(responseBuffer);
}

void deleteTree(Node* n) {
  if (!n) return;
  deleteTree(n->left); deleteTree(n->right);
  delete n;
}

void loop() {
  if (Serial.available()) {
    String url = Serial.readStringUntil('\n');
    url.trim();
    if (url.length() > 0) {
      // Send ID (simplified)
      byte id = (url.indexOf("style") != -1) ? 1 : 0;
      digitalWrite(PIN_NES_DATA_OUT, id); // Just one bit for now
      receiveResponse();
    }
  }
}
