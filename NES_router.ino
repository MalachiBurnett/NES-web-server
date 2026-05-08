/* 
 * NES WEB SERVER ROUTER
 * Bridges Serial (PC) to Controller Port (NES)
 */

#define PIN_NES_DATA_IN  2  // NES Latch (OUT0) -> Bit 0 of Response
#define PIN_NES_CLOCK    3  // NES OUT1       -> Clock for Response
#define PIN_NES_DATA_OUT 4  // NES D0 (Input) <- Request Data from Arduino

void setup() {
  Serial.begin(115200);
  
  pinMode(PIN_NES_DATA_IN, INPUT);
  pinMode(PIN_NES_CLOCK, INPUT);
  pinMode(PIN_NES_DATA_OUT, OUTPUT);
  
  digitalWrite(PIN_NES_DATA_OUT, LOW);
  Serial.println("ARDUINO: Gateway Online. Waiting for URL...");
}

void loop() {
  // 1. Wait for URL from the PC (Reverse Proxy)
  if (Serial.available()) {
    String url = Serial.readStringUntil('\n');
    url.trim();
    
    // 2. Map URL to 8-bit ID
    byte requestId = mapUrlToId(url);
    Serial.print("ARDUINO: Mapping ");
    Serial.print(url);
    Serial.print(" to ID: ");
    Serial.println(requestId);
    
    // 3. Send ID to NES (Wait for Strobe first)
    sendIdToNes(requestId);
    
    // 4. Receive Response from NES
    receiveResponseFromNes();
  }
}

byte mapUrlToId(String url) {
  if (url == "/" || url == "/index.html") return 0;
  if (url == "/about" || url == "/about.html") return 1;
  return 255; // Default 404
}

void sendIdToNes(byte id) {
  // Wait for NES to signal start (Strobe pulse on PIN_NES_DATA_IN)
  while(digitalRead(PIN_NES_DATA_IN) == LOW);
  while(digitalRead(PIN_NES_DATA_IN) == HIGH);
  
  // Shift out 8 bits to the NES
  // The NES pulses the Clock pin (which is hardwired to its $4016 read)
  // But in our current main.asm, the NES handles the timing with delays.
  for (int i = 0; i < 8; i++) {
    digitalWrite(PIN_NES_DATA_OUT, (id >> i) & 0x01);
    delayMicroseconds(100); // Match the RecDelay in main.asm
  }
}

void receiveResponseFromNes() {
  byte len = receiveByte();
  Serial.print("NES_RESPONSE_LEN: ");
  Serial.println(len);
  
  Serial.print("NES_CONTENT: ");
  for (int i = 0; i < len; i++) {
    char c = (char)receiveByte();
    Serial.print(c);
  }
  Serial.println(); // End of response
}

byte receiveByte() {
  byte data = 0;
  bool lastClock = LOW;
  int bitsReceived = 0;
  
  while (bitsReceived < 8) {
    bool currentClock = digitalRead(PIN_NES_CLOCK);
    
    // Detect Rising Edge
    if (currentClock == HIGH && lastClock == LOW) {
      bool bit = digitalRead(PIN_NES_DATA_IN);
      data |= (bit << bitsReceived);
      bitsReceived++;
    }
    lastClock = currentClock;
  }
  return data;
}
