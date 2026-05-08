struct Node {
  Node *left = nullptr;
  Node *right = nullptr;
  char c = 0;
  bool isLeaf = false;
};

Node* buildTree(const byte* treeData, int& bitPos, int maxBits) {
  if (bitPos >= maxBits) return nullptr;
  
  bool isLeaf = (treeData[bitPos / 8] >> (7 - (bitPos % 8))) & 1;
  bitPos++;
  
  Node* node = new Node();
  if (isLeaf) {
    node->isLeaf = true;
    byte c = 0;
    for (int i = 0; i < 8; i++) {
      if ((treeData[bitPos / 8] >> (7 - (bitPos % 8))) & 1) {
        c |= (1 << (7 - i));
      }
      bitPos++;
    }
    node->c = (char)c;
  } else {
    node->left = buildTree(treeData, bitPos, maxBits);
    node->right = buildTree(treeData, bitPos, maxBits);
  }
  return node;
}

void deleteTree(Node* node) {
  if (!node) return;
  deleteTree(node->left);
  deleteTree(node->right);
  delete node;
}

#define PIN_NES_DATA_IN  2  // NES Latch (OUT0) -> Bit 0 of Response
#define PIN_NES_CLOCK    3  // NES OUT1       -> Clock for Response
#define PIN_NES_DATA_OUT 4  // NES D0 (Input) <- Request Data from Arduino

void setup() {
  Serial.begin(115200);
  
  pinMode(PIN_NES_DATA_IN, INPUT);
  pinMode(PIN_NES_CLOCK, INPUT);
  pinMode(PIN_NES_DATA_OUT, OUTPUT);
  
  digitalWrite(PIN_NES_DATA_OUT, LOW);
  Serial.println("ARDUINO: Gateway Online. Huffman Decoding Enabled.");
}

void loop() {
  if (Serial.available()) {
    String url = Serial.readStringUntil('\n');
    url.trim();
    if (url.length() == 0) return;

    byte requestId = mapUrlToId(url);
    Serial.print("ARDUINO: Mapping ");
    Serial.print(url);
    Serial.print(" to ID: ");
    Serial.println(requestId);
    
    sendIdToNes(requestId);
    receiveResponseFromNes();
  }
}

byte mapUrlToId(String url) {
  if (url == "/" || url == "/index.html") return 0;
  if (url == "/about" || url == "/about.html") return 1;
  return 2; // 404 is now ID 2 in our injected data
}

void sendIdToNes(byte id) {
  while(digitalRead(PIN_NES_DATA_IN) == LOW);
  while(digitalRead(PIN_NES_DATA_IN) == HIGH);
  
  for (int i = 0; i < 8; i++) {
    digitalWrite(PIN_NES_DATA_OUT, (id >> i) & 0x01);
    delayMicroseconds(100);
  }
}

void receiveResponseFromNes() {
  uint16_t totalBytes = receiveByte();
  totalBytes |= (uint16_t)receiveByte() << 8;
  
  if (totalBytes == 0) {
    Serial.println("NES_ERROR: No Data");
    return;
  }

  // 1. Receive Tree Metadata
  byte treeByteCount = receiveByte();
  byte treeData[treeByteCount];
  for (int i = 0; i < treeByteCount; i++) treeData[i] = receiveByte();
  
  int bitPos = 0;
  Node* root = buildTree(treeData, bitPos, treeByteCount * 8);

  // 2. Receive Original Length
  uint16_t origLen = receiveByte();
  origLen |= (uint16_t)receiveByte() << 8;

  Serial.print("NES_PKT_SIZE: "); Serial.println(totalBytes);
  Serial.print("NES_ORIG_SIZE: "); Serial.println(origLen);
  Serial.print("NES_CONTENT: ");

  Node* currentNode = root;
  uint16_t charsDecoded = 0;
  
  // 3. Receive and Decode Compressed Data
  // Remaining bytes = totalBytes - 1 (treeByteCount byte) - treeByteCount - 2 (origLen)
  int dataBytesToRead = totalBytes - 1 - treeByteCount - 2;
  
  for (int i = 0; i < dataBytesToRead; i++) {
    byte b = receiveByte();
    for (int bit = 7; bit >= 0; bit--) {
      bool val = (b >> bit) & 0x01;
      
      if (val == 0) currentNode = currentNode->left;
      else currentNode = currentNode->right;

      if (currentNode && currentNode->isLeaf) {
        Serial.print(currentNode->c);
        charsDecoded++;
        currentNode = root;
        if (charsDecoded >= origLen) break;
      }
    }
    if (charsDecoded >= origLen) break;
  }
  Serial.println(); 
  deleteTree(root);
}

byte receiveByte() {
  byte data = 0;
  bool lastClock = LOW;
  int bitsReceived = 0;
  
  while (bitsReceived < 8) {
    bool currentClock = digitalRead(PIN_NES_CLOCK);
    if (currentClock == HIGH && lastClock == LOW) {
      bool bit = digitalRead(PIN_NES_DATA_IN);
      data |= (bit << bitsReceived);
      bitsReceived++;
    }
    lastClock = currentClock;
  }
  return data;
}
